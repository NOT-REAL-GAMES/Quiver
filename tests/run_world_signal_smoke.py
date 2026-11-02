#!/usr/bin/env python3
"""Exercise server-authorized NAT signaling with real protocol-v5 UDP clients."""
import argparse
import json
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import tempfile
import time
from run_world_network_smoke import Client, control, copied_engine_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    args = parser.parse_args()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    work = Path(tempfile.mkdtemp(prefix='quiver-signaling-smoke-'))
    (work / 'id1').mkdir()
    for asset in source.glob('pak*.pak'): (work / 'id1' / asset.name).symlink_to(asset.resolve())
    if (source / 'progs.dat').is_file(): (work / 'id1/progs.dat').symlink_to(source / 'progs.dat')
    binary = work / 'vkquake'
    shutil.copy2(args.binary.resolve(), binary)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
    log = (work / 'server.log').open('wb')
    server = subprocess.Popen([str(binary), '-basedir', str(work), '-userdir', str(work / 'user'),
        '-dedicated', '4', '-port', str(port), '+developer', '1', '+deathmatch', '0', '+coop', '0', '+map', '__octree'],
        stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=copied_engine_environment(args.binary))
    selector = selectors.DefaultSelector()
    clients = []
    disconnected = set()
    started = time.monotonic()
    summary = {'success': False, 'work': str(work)}
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.settimeout(.05)
            while time.monotonic() - started < 30:
                probe.sendto(control(b'\x02QUAKE\x00\x03'), ('127.0.0.1', port))
                try:
                    if b'__octree\x00' in probe.recv(4096): break
                except socket.timeout: pass
            else: raise RuntimeError('server startup timeout')
        clients = [Client(i, ('127.0.0.1', port), selector, godmode=True) for i in range(3)]
        def pump(duration=0.02):
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                now = time.monotonic()
                if server.poll() is not None: raise RuntimeError('server exited')
                for client in clients:
                    if client.index not in disconnected: client.pump(now, started, False, False)
                for key, _ in selector.select(.002):
                    while True:
                        try: data, address = key.data.socket.recvfrom(65536)
                        except BlockingIOError: break
                        try: key.data.receive(data, address, time.monotonic(), started)
                        except RuntimeError as error:
                            if 'disconnected' not in str(error): raise
                            disconnected.add(key.data.index)
        def wait(predicate, label, seconds=15):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if predicate(): return
                pump()
            raise RuntimeError('timeout: ' + label)
        wait(lambda: all(c.frames and c.godmode_confirmed for c in clients), 'spawn')
        a, b, outsider = clients
        a.advertise_nat(); b.advertise_nat()
        wait(lambda: a.peer_id and b.peer_id and b.peer_id in a.peer_directory and a.peer_id in b.peer_directory, 'mutual introduction')
        wait(lambda: outsider.peer_id != 0, 'outsider directory identity')
        assert a.peer_id != b.peer_id and a.peer_session == b.peer_session
        assert outsider.peer_id not in a.peer_directory
        payload = bytes(i & 255 for i in range(600))
        a.signal(b.peer_id, 11, payload[:512], total=600)
        a.signal(b.peer_id, 11, payload[512:], offset=512, total=600)
        wait(lambda: len([f for f in b.peer_signals if f[2] == 11]) == 2, 'fragment forwarding')
        received = sorted((f for f in b.peer_signals if f[2] == 11), key=lambda f: f[4])
        assert b''.join(f[5] for f in received) == payload
        assert all(f[0] == a.authority_epoch and f[1] == a.peer_id for f in received), 'server failed to authenticate source'
        a.signal(a.peer_id, 12, b'self')
        a.signal(0xffffffffffffffff, 13, b'unknown peer')
        a.signal(outsider.peer_id, 14, b'unintroduced peer')
        a.signal(b.peer_id, 15, b'stale epoch', epoch=a.authority_epoch + 1)
        wait(lambda: not a.queue and a.pending is None, 'rejected routing drained')
        pump(.4)
        assert not any(f[2] in (12, 13, 14, 15) for c in clients for f in c.peer_signals)
        assert not disconnected, 'valid but unauthorized routing should not disconnect clients'
        # Malformed bounds are rejected before forwarding, and only the offending
        # connection is lost; unrelated clients continue receiving world frames.
        frames = b.frames
        a.signal(b.peer_id, 16, b'x', total=8193)
        wait(lambda: 0 in disconnected, 'oversized signal disconnect')
        pump(.2)
        assert b.frames > frames and 1 not in disconnected and 2 not in disconnected
        assert not any(f[2] == 16 for f in b.peer_signals)
        summary.update(success=True, fragments_forwarded=2, authenticated_source=a.peer_id,
            target=b.peer_id, ignored_self_unknown_unintroduced_stale=True,
            oversized_signal_rejected=True, other_clients_survived=True)
    except Exception as error:
        summary['error'] = str(error)
    finally:
        for client in clients: client.socket.close()
        selector.close()
        if server.poll() is None:
            server.stdin.write(b'quit\n'); server.stdin.flush()
            try: server.wait(timeout=10)
            except subprocess.TimeoutExpired: server.kill(); server.wait()
        log.close()
        (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
    return 0 if summary['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
