#!/usr/bin/env python3
"""Exercise the production peer service through two isolated Linux NAT gateways.

Requires unprivileged user/network namespaces, iproute2 and nftables. All links,
routes, forwarding and firewall rules live in disposable namespaces. No host
network configuration or public STUN/signaling service is used.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import selectors
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib


def command(*args, input=None):
    result = subprocess.run(list(map(str, args)), input=input, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, args))}: {result.stderr.strip()}")
    return result.stdout


class Network:
    def __init__(self):
        self.holders = []
        self.nodes = {}
        try:
            self.setup()
        except Exception:
            self.close()
            raise

    def setup(self):
        command('ip', 'link', 'set', 'lo', 'up')
        command('ip', 'link', 'add', 'public', 'type', 'bridge')
        command('ip', 'address', 'add', '203.0.113.1/24', 'dev', 'public')
        command('ip', 'link', 'set', 'public', 'up')
        for index in (1, 2):
            for kind in ('router', 'client'):
                name = f'{kind}{index}'
                proc = subprocess.Popen(['unshare', '--net', 'sleep', '600'])
                self.holders.append(proc)
                self.nodes[name] = proc.pid
                deadline = time.monotonic() + 3
                own = os.readlink('/proc/self/ns/net')
                while os.readlink(f'/proc/{proc.pid}/ns/net') == own:
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('network namespace creation failed')
                    time.sleep(.01)
                self.run(name, 'ip', 'link', 'set', 'lo', 'up')
            router, client = f'router{index}', f'client{index}'
            command('ip', 'link', 'add', f'pub{index}', 'type', 'veth', 'peer', 'name', f'wan{index}')
            command('ip', 'link', 'set', f'pub{index}', 'master', 'public')
            command('ip', 'link', 'set', f'pub{index}', 'up')
            command('ip', 'link', 'set', f'wan{index}', 'netns', self.nodes[router])
            self.run(router, 'ip', 'link', 'set', f'wan{index}', 'name', 'wan')
            self.run(router, 'ip', 'address', 'add', f'203.0.113.{index + 1}/24', 'dev', 'wan')
            self.run(router, 'ip', 'link', 'set', 'wan', 'up')
            command('ip', 'link', 'add', f'lan{index}', 'type', 'veth', 'peer', 'name', f'host{index}')
            command('ip', 'link', 'set', f'lan{index}', 'netns', self.nodes[router])
            command('ip', 'link', 'set', f'host{index}', 'netns', self.nodes[client])
            self.run(router, 'ip', 'link', 'set', f'lan{index}', 'name', 'lan')
            self.run(client, 'ip', 'link', 'set', f'host{index}', 'name', 'eth0')
            self.run(router, 'ip', 'address', 'add', f'10.60.{index}.1/24', 'dev', 'lan')
            self.run(router, 'ip', 'link', 'set', 'lan', 'up')
            self.run(client, 'ip', 'address', 'add', f'10.60.{index}.2/24', 'dev', 'eth0')
            self.run(client, 'ip', 'link', 'set', 'eth0', 'up')
            self.run(client, 'ip', 'route', 'add', 'default', 'via', f'10.60.{index}.1')
            self.run(router, 'sysctl', '-q', '-w', 'net.ipv4.ip_forward=1')
            # Linux MASQUERADE can create destination-dependent UDP mappings,
            # which cannot necessarily be punched without TURN. Model a full-cone
            # router: mappings exist only after an outbound packet, retain its port,
            # and expire automatically. Nothing forwards unopened ports.
            rules = f'''table ip nat {{
 map ports {{ type inet_service : ipv4_addr; flags dynamic,timeout; timeout 60s; size 4096; }}
 chain prerouting {{ type nat hook prerouting priority dstnat; policy accept;
  iifname "wan" meta l4proto udp dnat to udp dport map @ports;
 }}
 chain postrouting {{ type nat hook postrouting priority srcnat; policy accept;
  oifname "wan" meta l4proto udp update @ports {{ udp sport : ip saddr }} snat to 203.0.113.{index + 1};
 }}
}}
table inet filter {{
 chain forward {{ type filter hook forward priority filter; policy drop;
  ct state established,related accept; ct status dnat accept; iifname "lan" accept;
 }}
}}
'''
            self.run(router, 'nft', '-f', '-', input=rules)

    def argv(self, name, *args):
        return ['nsenter', '--target', str(self.nodes[name]), '--net', *map(str, args)]

    def run(self, name, *args, input=None):
        return command(*self.argv(name, *args), input=input)

    def block_peer_routes(self):
        # Keep the local STUN server available; only traffic to the other NAT
        # gateway is dropped. Signaling still works through the test broker.
        for index in (1, 2):
            self.run(f'router{index}', 'nft', 'insert', 'rule', 'inet', 'filter', 'forward',
                     'ip', 'daddr', f'203.0.113.{4 - index}', 'drop')

    def restrict_peer_routes(self):
        # Keep the stable outbound-created NAT mapping, but only allow replies
        # from the exact remote address/port that this private UDP port contacted.
        # A STUN request therefore cannot open the filter to the other peer.
        rules = '''delete table inet filter
table inet filter {
 set contacts { type inet_service . ipv4_addr . inet_service;
  flags dynamic,timeout; timeout 60s; size 4096; }
 chain forward { type filter hook forward priority filter; policy drop;
  iifname "lan" meta l4proto udp update @contacts { udp sport . ip daddr . udp dport } accept;
  iifname "lan" accept;
  ct state established,related accept;
  iifname "wan" meta l4proto udp udp dport . ip saddr . udp sport @contacts accept;
 }
}
'''
        for index in (1, 2):
            self.run(f'router{index}', 'nft', '-f', '-', input=rules)

    def close(self):
        for proc in reversed(self.holders):
            proc.terminate()
        for proc in self.holders:
            proc.wait(timeout=3)


class Stun:
    def __init__(self):
        self.requests = []
        self.stop = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('203.0.113.1', 3478))
        self.sock.settimeout(.1)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            try:
                data, address = self.sock.recvfrom(8192)
            except socket.timeout:
                continue
            if len(data) < 20:
                continue
            kind, size, cookie = struct.unpack_from('!HHI', data)
            if kind != 1 or cookie != 0x2112A442 or len(data) != 20 + size:
                continue
            self.requests.append(address)
            attribute = struct.pack('!HHBBHI', 0x20, 8, 0, 1, address[1] ^ (cookie >> 16),
                                    int(ipaddress.IPv4Address(address[0])) ^ cookie)
            response = struct.pack('!HHI', 0x101, len(attribute) + 8, cookie) + data[8:20] + attribute
            response += struct.pack('!HHI', 0x8028, 4, zlib.crc32(response) ^ 0x5354554e)
            self.sock.sendto(response, address)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        self.sock.close()


def protobuf_fields(data):
    """Read top-level field boundaries without depending on generated GNS code."""
    cursor = 0

    def integer():
        nonlocal cursor
        value = shift = 0
        while cursor < len(data) and shift < 70:
            byte = data[cursor]; cursor += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
        raise RuntimeError('invalid protobuf integer in genuine GNS signal')

    fields = []
    while cursor < len(data):
        start = cursor
        tag = integer()
        wire = tag & 7
        if wire == 0:
            integer()
            payload = b''
        else:
            if wire == 1:
                size = 8
            elif wire == 5:
                size = 4
            elif wire == 2:
                size = integer()
            else:
                raise RuntimeError('unexpected protobuf wire type in genuine GNS signal')
            payload = data[cursor:cursor + size]
            if len(payload) != size:
                raise RuntimeError('truncated genuine GNS signal')
            cursor += size
        fields.append((tag >> 3, wire, start, cursor, payload))
    return fields


def protobuf_integer(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def identity_probes(data):
    """Mutate actual GNS signals, retaining their real connection IDs and crypto."""
    fields = protobuf_fields(data)
    identities = {field: (start, end, payload) for field, wire, start, end, payload in fields
                  if field in (8, 10) and wire == 2}
    if set(identities) != {8, 10}:
        raise RuntimeError('identity test requires both identities in the genuine signal')

    def encoded(field, value):
        return protobuf_integer(field * 8 + 2) + protobuf_integer(len(value)) + value

    def replace(field, value):
        start, end, _ = identities[field]
        return data[:start] + encoded(field, value) + data[end:]

    source_start, source_end, source = identities[8]
    target = identities[10][2]
    return {
        'duplicate_source': data + encoded(8, source),
        'duplicate_target': data + encoded(10, target),
        'conflicting_source': data + encoded(8, target),
        'conflicting_target': data + encoded(10, source),
        'missing_source': data[:source_start] + data[source_end:],
        'empty_source': replace(8, b''),
        'empty_target': replace(10, b''),
        'invalid_source': replace(8, b'not-an-identity'),
        'invalid_target': replace(10, b'not-an-identity'),
        'nul_source': replace(8, source + b'\0'),
        'nul_target': replace(10, target + b'\0'),
        'long_source': replace(8, b'x' * 1024),
        'long_target': replace(10, b'x' * 1024),
        'wrong_target': replace(10, source),
        'source_wire': data + bytes((8 * 8, 1)),
        'target_wire': data + bytes((10 * 8, 1)),
        'truncated_source': data + bytes((8 * 8 + 2, 127, 1)),
    }


def scenario(network, stun, fixture, work, name):
    blocked = name == 'blocked'
    isolated = name == 'isolation'
    revoked = name == 'revoked'
    binding = name == 'binding'
    if blocked:
        network.block_peer_routes()
    if name == 'restricted':
        network.restrict_peer_routes()
    processes, logs, ready = {}, {}, set()
    buffers = {}
    session, auth = secrets.token_hex(32), secrets.token_hex(32)
    stun_start = len(stun.requests)
    selector = selectors.DefaultSelector()
    signal_count = signal_bytes = 0
    result = None
    results = []
    pending_checks, checked, captured = {}, [], {}
    pre_checked = post_checked = repeated = False
    start = time.monotonic()

    def check(target, source, label, payload, expected=False):
        if label in pending_checks or label in checked:
            raise RuntimeError('duplicate identity probe label')
        pending_checks[label] = (target, expected)
        processes[target].stdin.write(f'CHECK {label} {source} {payload.hex()}\n'.encode())
        processes[target].stdin.flush()

    def probe(target, source, prefix, payload):
        # Peer 3 is explicitly introduced in both services, but does not own the
        # valid identity carried by this real signal from the other connection.
        check(target, 3, prefix + '_misattributed', payload)
        for label, malformed in identity_probes(payload).items():
            check(target, source, prefix + '_' + label, malformed)

    try:
        for identity, role in ((1, 'provider'), (2, 'receiver')):
            log = (work / f'{name}-{role}.log').open('w')
            logs[identity] = log
            proc = subprocess.Popen(network.argv(f'client{identity}', fixture, '--identity', identity,
                '--peer', 3 - identity, '--role', role, '--stun', '203.0.113.1:3478',
                '--session', secrets.token_hex(32) if isolated and identity == 2 else session, '--auth', auth),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, bufsize=0)
            processes[identity] = proc
            buffers[identity] = b''
            selector.register(proc.stdout, selectors.EVENT_READ, identity)
        fetching = False
        while time.monotonic() - start < 25:
            for key, _ in selector.select(.05):
                identity = key.data
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    raise RuntimeError(f'{name}: fixture {identity} exited unexpectedly')
                buffers[identity] += chunk
                if len(buffers[identity]) > 65536:
                    raise RuntimeError('fixture output exceeded line budget')
                while b'\n' in buffers[identity]:
                    raw, buffers[identity] = buffers[identity].split(b'\n', 1)
                    line = raw.decode()
                    logs[identity].write(line + '\n'); logs[identity].flush()
                    parts = line.strip().split()
                    if not parts:
                        continue
                    if parts[0] == 'READY':
                        ready.add(identity)
                    elif parts[0] == 'SIGNAL':
                        if len(parts) != 3 or int(parts[1]) != 3 - identity:
                            raise RuntimeError('signaling attempted to escape the admitted peer pair')
                        payload = bytes.fromhex(parts[2])
                        if len(payload) > 8192:
                            raise RuntimeError('oversized signaling message')
                        signal_count += 1; signal_bytes += len(payload)
                        if signal_count > 256 or signal_bytes > 1024 * 1024:
                            raise RuntimeError('signaling exceeded bounded negotiation budget')
                        target_id = int(parts[1])
                        captured[target_id] = (identity, payload)
                        if binding and identity == 2 and not pre_checked:
                            probe(target_id, identity, 'before', payload)
                            pre_checked = True
                        target = processes[target_id]
                        target.stdin.write(f'SIGNAL {identity} {parts[2]}\n'.encode()); target.stdin.flush()
                    elif parts[0] == 'CHECK':
                        if len(parts) != 3 or parts[1] not in pending_checks:
                            raise RuntimeError(f'unexpected identity check result: {parts}')
                        target, expected = pending_checks.pop(parts[1])
                        if identity != target or parts[2] != str(int(expected)):
                            raise RuntimeError(f'identity binding check failed: {parts}, expected {expected}')
                        checked.append(parts[1])
                    elif parts[0] == 'REVOKED':
                        processes[2].stdin.write(b'FETCH\n'); processes[2].stdin.flush()
                    elif parts[0] == 'RESULT':
                        result = parts[1:]
                        results.append(result)
                        if len(results) == 1 and name in ('nat', 'restricted', 'revoked') and result[0] == 'success':
                            if revoked:
                                processes[1].stdin.write(b'REVOKE\n'); processes[1].stdin.flush()
                            else:
                                processes[2].stdin.write(b'FETCH\n'); processes[2].stdin.flush()
                            result = None
                        elif binding and len(results) == 1 and result[0] == 'success':
                            result = None
            if ready == {1, 2} and not fetching:
                processes[2].stdin.write(b'FETCH\n'); processes[2].stdin.flush()
                fetching = True
            if binding and len(results) == 1 and not pending_checks:
                if not post_checked:
                    if not pre_checked or set(captured) != {1, 2}:
                        raise RuntimeError('identity test did not capture the negotiated signals')
                    for target, (source, payload) in captured.items():
                        # A nonzero destination connection ID specifically exercises
                        # GNS's existing-connection dispatch path, not its new-peer callback.
                        connection_ids = [value for field, wire, _, _, value in protobuf_fields(payload)
                                          if field == 1 and wire == 5]
                        if len(connection_ids) != 1 or not any(connection_ids[0]):
                            raise RuntimeError('established signal lacks destination connection ID')
                        probe(target, source, f'after_{target}', payload)
                        check(target, source, f'after_{target}_valid', payload, expected=True)
                    post_checked = True
                elif not repeated:
                    processes[2].stdin.write(b'FETCH\n'); processes[2].stdin.flush()
                    repeated = True
            if result:
                break
        if not result:
            raise RuntimeError(f'{name}: negotiation did not complete within 25 seconds')
        expected = 'failure' if blocked or isolated or revoked else 'success'
        if result[0] != expected:
            raise RuntimeError(f'{name}: expected {expected}, received {result}')
        if expected == 'success' and (len(result) < 3 or int(result[1]) < 65536 or int(result[2]) < 1):
            raise RuntimeError('NAT transfer did not deliver a complete object')
        if name in ('nat', 'restricted', 'binding') and (len(results) != 2 or int(results[1][2]) != int(results[0][2]) + 1 or results[1][3] != results[0][3]):
            raise RuntimeError('repeat object request did not reuse its ICE connection')
        if binding and (not repeated or pending_checks or not checked):
            raise RuntimeError('identity binding probes did not complete')
        if revoked and (len(results) != 2 or results[0][0] != 'success'):
            raise RuntimeError('revocation fixture never established its initial authorized transfer')
        if not signal_count:
            raise RuntimeError('no custom signaling exchange occurred')
        if not isolated and not {'203.0.113.2', '203.0.113.3'}.issubset({address[0] for address in stun.requests[stun_start:]}):
            raise RuntimeError('both distinct NAT gateways were not observed by STUN')
        return {'name': name, 'results': results, 'seconds': time.monotonic() - start,
                'signals': signal_count, 'signal_bytes': signal_bytes,
                'identity_checks': checked,
                'stun_addresses': sorted({address[0] for address in stun.requests[stun_start:]})}
    finally:
        for proc in processes.values():
            if proc.poll() is None:
                proc.terminate()
        for proc in processes.values():
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        for log in logs.values(): log.close()
        selector.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--work', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    fixture = args.fixture.resolve()
    work = args.work or Path(tempfile.mkdtemp(prefix='quiver-nat-smoke-'))
    if not args.inside:
        commandline = ['unshare', '--user', '--map-root-user', '--mount', '--net', sys.executable,
                       str(Path(__file__).resolve()), '--inside', '--fixture', str(fixture), '--work', str(work)]
        sys.exit(subprocess.call(commandline))
    network = stun = None
    try:
        network = Network()
        stun = Stun()
        runs = [scenario(network, stun, fixture, work, name)
                for name in ('nat', 'binding', 'restricted', 'revoked', 'isolation', 'blocked')]
        summary = {'success': True, 'work': str(work), 'runs': runs}
        (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
    finally:
        if stun: stun.close()
        if network: network.close()


if __name__ == '__main__':
    main()
