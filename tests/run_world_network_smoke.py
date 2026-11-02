#!/usr/bin/env python3
"""Real UDP clients against an isolated dedicated Quiver server.

This qualifies connection, signon, command transport and canonical frame/chunk
delivery. The harness runs with deathmatch/coop disabled and explicitly enables
godmode on every client before movement or attacks. This isolates transport from
unmigrated base-game death/respawn code when a player leaves the local frame.
It does not validate combat, respawning, rendering, prediction, persistence, or
server tick performance.

Example:
  python3 tests/run_world_network_smoke.py --binary build/vkquake --basedir /path/to/quake --clients 64
Only pak*.pak and an optional progs.dat are linked into the temporary game dir.
User configurations and saves are never loaded or modified. The server accepts
connections on a randomly selected local test port and runs for a bounded time.
"""

import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import statistics
import struct
import subprocess
import tempfile
import time


CONTROL = 0x80000000
DATA, ACK, EOM, UNRELIABLE = 0x10000, 0x20000, 0x80000, 0x100000
MARKER = 0x51565231
WORLD_PROTOCOL_VERSION = 5
ROOT_PREFIX = struct.pack("<IHH", MARKER, WORLD_PROTOCOL_VERSION, 5)
CONFIG_PREFIX = struct.pack("<IHH", MARKER, WORLD_PROTOCOL_VERSION, 1)
WIRE_PREFIX = struct.pack("<IHH", MARKER, WORLD_PROTOCOL_VERSION, 2)
CHUNK_PREFIX = struct.pack("<IHH", MARKER, WORLD_PROTOCOL_VERSION, 4)
EDIT_PREFIX = struct.pack("<IHH", MARKER, WORLD_PROTOCOL_VERSION, 3)
FAR_CHUNK = (1_000_000_000, -1_000_000_000, 0)


def copied_engine_environment(original_binary):
    """Preserve resolved optional runtime libraries when testing a copied binary."""
    environment = os.environ.copy()
    if shutil.which("ldd"):
        result = subprocess.run(["ldd", str(Path(original_binary).resolve())], text=True, capture_output=True)
        libraries = re.findall(r"libGameNetworkingSockets[^ ]* => (/[^ ]+)", result.stdout)
        if libraries:
            directories = list(dict.fromkeys(str(Path(path).parent) for path in libraries))
            if environment.get("LD_LIBRARY_PATH"):
                directories.append(environment["LD_LIBRARY_PATH"])
            environment["LD_LIBRARY_PATH"] = ":".join(directories)
    return environment


def control(payload):
    return struct.pack(">I", CONTROL | (4 + len(payload))) + payload


def packet(flags, sequence, payload=b""):
    return struct.pack(">II", flags | (8 + len(payload)), sequence) + payload


class Client:
    def __init__(self, index, address, selector, godmode=False):
        self.index = index
        self.name = f"worldsmoke{index:03d}"
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.setblocking(False)
        selector.register(self.socket, selectors.EVENT_READ, self)
        self.address = address
        self.connected = False
        self.signon = 0
        self.pext_sent = False
        self.queue = collections.deque()
        self.pending = None
        self.send_sequence = 0
        self.receive_sequence = 0
        self.unreliable_sequence = 0
        self.last_send = 0
        self.last_move = 0
        self.assembly = bytearray()
        self.frames = 0
        self.far_frames = 0
        self.chunk_fragments = 0
        self.far_fragments = 0
        self.in_bytes = 0
        self.out_bytes = 0
        self.move_commands = 0
        self.attack_commands = 0
        self.server_time = 0.0
        self.spawn_seconds = None
        self.frame_gaps = []
        self.last_frame = None
        self.edit_replies = []
        self.authority_epoch = 1
        self.world_root = bytes(32)
        self.chunk_revisions = {}
        self.peer_id = 0
        self.peer_session = None
        self.peer_directory = {}
        self.peer_signals = []
        self.proof_fragments = {}
        self.drop_first_chunk_ack = False
        self.dropped_ack_key = None
        self.ack_retries = 0
        self.edit_rejections = collections.Counter()
        self.request_godmode = godmode
        self.godmode_confirmed = False
        self.health = None
        self.minimum_health = None
        self.last_health_at = None
        self.far_health = None
        self.last_frame_chunk = None

    @staticmethod
    def snapshot_health(payload, offset=48):
        """Read health from this harness's non-FTE RMQ snapshot layout."""
        try:
            while offset < len(payload):
                service = payload[offset]
                offset += 1
                if service & 128:
                    bits = service & 127
                    for flag, shift in ((0, 8), (15, 16), (23, 24)):
                        if bits & (1 << flag):
                            bits |= payload[offset] << shift
                            offset += 1
                    offset += 2 if bits & (1 << 14) else 1
                    offset += sum(bool(bits & (1 << flag)) for flag in (10, 6, 11, 12, 13))
                    for axis, angle_bit in enumerate((8, 4, 9)):
                        offset += 4 * bool(bits & (1 << (axis + 1))) + 2 * bool(bits & (1 << angle_bit))
                    offset += sum(bool(bits & (1 << flag)) for flag in (16, 20, 17, 18, 19))
                elif service == 7:  # svc_time
                    offset += 4
                elif service == 10:  # svc_setangle: three SHORTANGLE values
                    offset += 6
                elif service == 19:  # svc_damage: armor, blood, three FLOATCOORD values
                    offset += 14
                elif service == 15:  # svc_clientdata
                    bits = struct.unpack_from("<H", payload, offset)[0]
                    offset += 2
                    for flag, shift in ((15, 16), (23, 24)):
                        if bits & (1 << flag):
                            bits |= payload[offset] << shift
                            offset += 1
                    offset += sum(bool(bits & (1 << flag)) for flag in range(8))
                    offset += 4  # items is always present
                    offset += sum(bool(bits & (1 << flag)) for flag in (12, 13, 14))
                    return struct.unpack_from("<h", payload, offset)[0]
                else:
                    return None  # Optional effects are outside this narrow parser.
        except (IndexError, struct.error):
            return None
        return None

    def signon_stage(self, payload):
        if len(payload) >= 2 and payload[-2] == 25 and payload[-1] in (1, 2, 3):
            return payload[-1]
        if self.signon != 2:
            return None
        # Spawn info may be followed by broadcast score updates in the same
        # reliable message. Walk its known services instead of searching raw
        # bytes, which could mistake a score or field value for signon3.
        offset = 0
        try:
            while offset < len(payload):
                service = payload[offset]
                offset += 1
                if service == 25:
                    return payload[offset] if payload[offset] == 3 else None
                if service in (8, 9, 26):  # print, stufftext, centerprint
                    offset = payload.index(0, offset) + 1
                elif service in (12, 13):  # lightstyle, updatename
                    offset = payload.index(0, offset + 1) + 1
                elif service in (3, 5, 7, 10, 14, 17, 32):
                    offset += {3: 5, 5: 2, 7: 4, 10: 6, 14: 3, 17: 2, 32: 2}[service]
                elif service == 15:
                    bits = struct.unpack_from("<H", payload, offset)[0]
                    offset += 2
                    for flag, shift in ((15, 16), (23, 24)):
                        if bits & (1 << flag):
                            bits |= payload[offset] << shift
                            offset += 1
                    offset += sum(bool(bits & (1 << flag)) for flag in range(8))
                    offset += 4 + sum(bool(bits & (1 << flag)) for flag in (12, 13, 14))
                    offset += 8  # health short, six ammo/weapon bytes
                    offset += sum(bool(bits & (1 << flag)) for flag in (*range(16, 23), 24, 25))
                else:
                    return None
        except (IndexError, ValueError, struct.error):
            return None
        return None

    def send(self, data):
        self.socket.sendto(data, self.address)
        self.out_bytes += len(data)

    def command(self, *commands):
        self.queue.append(b"".join(b"\x04" + command.encode("ascii") + b"\x00" for command in commands))

    def edit(self, request_id, revision, z, placing=False, x=132.0):
        payload = EDIT_PREFIX + struct.pack("<QQQQqqqfffHh", request_id, revision, 0, self.authority_epoch, 0, 0, 0,
                                           float(x), 132.0, float(z), 1 if placing else 0, -2 if placing else -1)
        self.queue.append(b"\x57" + struct.pack("<H", len(payload)) + payload)

    def advertise_nat(self, port=0):
        data = b"\xa1" + struct.pack("<HB", port, 1)
        self.queue.append(b"\x57" + struct.pack("<H", len(data)) + data)

    def signal(self, destination, message_id, payload, epoch=None, offset=0, total=None):
        data = b"\xa3" + struct.pack("<IHHQQQHHH", MARKER, WORLD_PROTOCOL_VERSION, 8,
            self.authority_epoch if epoch is None else epoch, destination, message_id,
            len(payload) if total is None else total, offset, len(payload)) + payload
        self.queue.append(b"\x57" + struct.pack("<H", len(data)) + data)

    def pump(self, now, started, moving, shooting):
        if not self.connected:
            if now - self.last_send > 1:
                self.send(control(b"\x01QUAKE\x00\x03"))
                self.last_send = now
            return
        if self.pending is not None:
            if now - self.last_send > 1:
                self.send(self.pending)
                self.last_send = now
        elif self.queue:
            self.pending = packet(DATA | EOM, self.send_sequence, self.queue.popleft())
            self.send(self.pending)
            self.last_send = now
        if self.signon >= 3 and now - self.last_move >= 0.05:
            # RMQ with SHORTANGLE, no FTE prediction extension: timestamp, 3 uint16
            # angles, 3 int16 velocities, buttons, impulse. These are actual input
            # packets; the test deliberately makes no claims about combat outcomes.
            yaw = int((self.index * 137.5 + (now - started) * 12) * 65536 / 360) & 0xffff
            buttons = 1 if shooting and self.index % 4 == 0 else 0
            payload = b"\x03" + struct.pack("<fHHHhhhBB", self.server_time, 0, yaw, 0, 200 if moving else 0, 0, 0, buttons, 0)
            self.send(packet(UNRELIABLE, self.unreliable_sequence, payload))
            self.unreliable_sequence += 1
            self.move_commands += 1
            self.attack_commands += bool(buttons)
            self.last_move = now

    def receive(self, data, address, now, started):
        self.in_bytes += len(data)
        if len(data) < 4:
            raise RuntimeError("truncated UDP packet")
        flags_length = struct.unpack_from(">I", data)[0]
        if flags_length & 0xffff != len(data):
            raise RuntimeError("UDP packet length mismatch")
        flags = flags_length & ~0xffff
        if flags & CONTROL:
            if data[4] == 0x82:
                raise RuntimeError(f"client {self.index} rejected: {data[5:-1].decode(errors='replace')}")
            if data[4] == 0x81 and not self.connected:
                self.address = (address[0], struct.unpack_from("<I", data, 5)[0])
                self.connected = True
            return
        if len(data) < 8 or address != self.address:
            raise RuntimeError("unexpected server datagram")
        sequence = struct.unpack_from(">I", data, 4)[0]
        if flags & ACK:
            if self.pending is not None and sequence == self.send_sequence:
                self.pending = None
                self.send_sequence += 1
            return
        if flags & DATA:
            self.send(packet(ACK, sequence))
            if sequence != self.receive_sequence:
                return
            self.receive_sequence += 1
            self.assembly.extend(data[8:])
            if len(self.assembly) > 65536:
                raise RuntimeError("reliable message exceeds receive budget")
            if flags & EOM:
                payload = bytes(self.assembly)
                self.assembly.clear()
                self.message(payload)
            return
        if flags & UNRELIABLE:
            payload = data[8:]
            # OW_WriteFrame prefixes every spawned client's snapshot.
            if len(payload) >= 48 and payload[:4] == b"\x57\x00\x2c\x00" and payload[4:12] == WIRE_PREFIX:
                chunk = struct.unpack_from("<qqq", payload, 12)
                self.last_frame_chunk = chunk
                local = struct.unpack_from("<fff", payload, 36)
                if any(not math.isfinite(value) or not 0 <= value < 1024 for value in local):
                    raise RuntimeError("non-canonical world frame")
                self.frames += 1
                self.far_frames += chunk == FAR_CHUNK
                health = self.snapshot_health(payload)
                if health is not None:
                    self.health = health
                    self.minimum_health = health if self.minimum_health is None else min(self.minimum_health, health)
                    self.last_health_at = now
                    if chunk == FAR_CHUNK:
                        self.far_health = health
                if self.spawn_seconds is None:
                    self.spawn_seconds = now - started
                if self.last_frame is not None:
                    self.frame_gaps.append(now - self.last_frame)
                self.last_frame = now
                if payload[48:49] == b"\x07" and len(payload) >= 53:
                    self.server_time = struct.unpack_from("<f", payload, 49)[0]
            elif payload[:1] == b"\x0f":
                # With many entity updates, the server sends clientdata in its
                # own datagram because it does not fit the world snapshot.
                health = self.snapshot_health(payload, 0)
                if health is not None:
                    self.health = health
                    self.minimum_health = health if self.minimum_health is None else min(self.minimum_health, health)
                    self.last_health_at = now
                    if self.last_frame_chunk == FAR_CHUNK:
                        self.far_health = health
            if payload == b"\x02":
                raise RuntimeError(f"client {self.index} disconnected")

    def message(self, payload):
        # These envelopes are versioned and may follow unrelated reliable Quake
        # services. Only recognize complete envelopes with the exact inner header.
        for kind, packet_type in ((7, 9), (8, 8)):
            cursor = 0
            while True:
                cursor = payload.find(bytes((0x57, kind)), cursor)
                if cursor < 0:
                    break
                if cursor + 4 > len(payload):
                    break
                size = struct.unpack_from("<H", payload, cursor + 2)[0]
                inner = payload[cursor + 4:cursor + 4 + size]
                cursor += 1
                if len(inner) != size or inner[:8] != struct.pack("<IHH", MARKER, WORLD_PROTOCOL_VERSION, packet_type):
                    continue
                if kind == 8:
                    epoch, source, message_id, total, offset, length = struct.unpack_from("<QQQHHH", inner, 8)
                    if len(inner) != 38 + length or not source or not message_id or not 0 < total <= 8192 or length > 512 or offset + length > total:
                        raise RuntimeError("malformed peer signaling relay")
                    self.peer_signals.append((epoch, source, message_id, total, offset, inner[38:]))
                    continue
                epoch, self.peer_id = struct.unpack_from("<QQ", inner, 8)
                self.peer_session = inner[24:56]
                stun_size = struct.unpack_from("<H", inner, 88)[0]
                if stun_size > 255 or len(inner) < 92 + stun_size:
                    raise RuntimeError("malformed peer directory")
                offset = 90 + stun_size
                count = struct.unpack_from("<H", inner, offset)[0]
                offset += 2
                if count > 128 or epoch != self.authority_epoch:
                    raise RuntimeError("invalid peer directory epoch or size")
                peers = {}
                for _ in range(count):
                    peer_id, nat, port, address_size = struct.unpack_from("<QBHB", inner, offset)
                    offset += 12
                    address = inner[offset:offset + address_size].decode("ascii")
                    offset += address_size
                    if not peer_id or nat > 1 or address_size > 15 or peer_id in peers:
                        raise RuntimeError("invalid directory peer")
                    peers[peer_id] = (bool(nat), port, address)
                if offset != len(inner):
                    raise RuntimeError("trailing peer directory bytes")
                self.peer_directory = peers
        config_at = payload.find(CONFIG_PREFIX)
        if config_at >= 0 and len(payload) >= config_at + 63:
            self.authority_epoch = struct.unpack_from("<Q", payload, config_at + 21)[0]
            self.world_root = payload[config_at + 29:config_at + 61]
            if not self.authority_epoch:
                raise RuntimeError("zero world authority epoch")
        root_at = payload.find(b"\x57\x05\x30\x00" + ROOT_PREFIX)
        if root_at >= 0 and len(payload) >= root_at + 52:
            self.authority_epoch = struct.unpack_from("<Q", payload, root_at + 12)[0]
            self.world_root = payload[root_at + 20:root_at + 52]
        if b"godmode ON\n\x00" in payload:
            self.godmode_confirmed = True
        for reason in re.findall(rb"World [ -~]{0,160}\n\x00", payload):
            self.edit_rejections[reason[:-1].decode().strip()] += 1
        start = 0
        while True:
            start = payload.find(b"\x57\x04\x50\x00" + EDIT_PREFIX, start)
            if start < 0:
                break
            inner = payload[start + 4:start + 84]
            if len(inner) == 80:
                request_id, previous, revision = struct.unpack_from("<QQQ", inner, 8)
                if revision != previous + 1:
                    raise RuntimeError("non-monotonic edit acknowledgement")
                self.edit_replies.append((request_id, previous, revision, inner))
            start += 84
        if b"cmd pext\n\x00" in payload and not self.pext_sent:
            self.command(f"pext {MARKER} 1\n")
            self.pext_sent = True
        stage = self.signon_stage(payload)
        if stage is not None:
            if stage > self.signon:
                self.signon = stage
                if stage == 1:
                    if struct.pack("<I", MARKER) not in payload:
                        raise RuntimeError("server did not negotiate Quiver world protocol")
                    self.command(f'name "{self.name}"\n', "prespawn\n")
                elif stage == 2:
                    # Apply protection in the same command packet as spawn,
                    # before a server physics tick or another client's spawn
                    # can damage this player at the shared fixture spawnpoint.
                    self.command("spawn\n", *(["god 1\n"] if self.request_godmode else []))
                else:
                    self.command("begin\n")
        # Other reliable services can precede chunk transfers. A full client
        # parser is unnecessary here: validate the envelope and exact inner magic.
        start = 0
        while True:
            start = payload.find(b"\x57\x01", start)
            if start < 0:
                break
            if start + 4 <= len(payload):
                length = struct.unpack_from("<H", payload, start + 2)[0]
                inner = payload[start + 4:start + 4 + length]
                if len(inner) == length and length >= 58 and inner[:8] == CHUNK_PREFIX:
                    key = struct.unpack_from("<qqq", inner, 8)
                    self.chunk_revisions[key] = struct.unpack_from("<Q", inner, 32)[0]
                    total, offset, count = struct.unpack_from("<IIH", inner, 48)
                    if not total or total > 16 * 1024 * 1024 or offset >= total or offset % 1024 or count != min(1024, total - offset) or length != 58 + count:
                        raise RuntimeError("malformed chunk fragment")
                    transfer_id = struct.unpack_from("<Q", inner, 40)[0]
                    assembly = self.proof_fragments.setdefault(transfer_id, {})
                    assembly[offset] = inner[58:]
                    if sum(map(len, assembly.values())) == total:
                        proof = b"".join(assembly[o] for o in sorted(assembly))
                        del self.proof_fragments[transfer_id]
                        if proof[:8] != struct.pack("<IHH", MARKER, WORLD_PROTOCOL_VERSION, 6):
                            raise RuntimeError("invalid terrain proof header")
                        cursor = 73
                        for _ in range(proof[72]):
                            object_type = proof[cursor]
                            object_hash = proof[cursor + 1:cursor + 33]
                            size = struct.unpack_from("<I", proof, cursor + 33)[0]
                            body = proof[cursor + 37:cursor + 37 + size]
                            if hashlib.sha256(b"QWO\x01" + bytes([object_type]) + body).digest() != object_hash:
                                raise RuntimeError("invalid proof node hash")
                            cursor += 37 + size
                        object_hash = bytes(32)
                        if proof[cursor]:
                            cursor += 1
                            object_type = proof[cursor]
                            object_hash = proof[cursor + 1:cursor + 33]
                            size = struct.unpack_from("<I", proof, cursor + 33)[0]
                            body = proof[cursor + 37:cursor + 37 + size]
                            if not size:  # detached offer: wait for server fallback
                                start += 4 + length
                                continue
                            if hashlib.sha256(b"QWO\x01" + bytes([object_type]) + body).digest() != object_hash:
                                raise RuntimeError("invalid terrain object hash")
                        revision = struct.unpack_from("<Q", inner, 32)[0]
                        if self.drop_first_chunk_ack:
                            self.drop_first_chunk_ack = False
                            self.dropped_ack_key = key
                        else:
                            if key == self.dropped_ack_key:
                                self.ack_retries += 1
                            ack = b"\xa2" + struct.pack("<qqqfffQ", *key, 0, 0, 0, revision) + object_hash
                            self.queue.append(b"\x57" + struct.pack("<H", len(ack)) + ack)
                    self.chunk_fragments += 1
                    self.far_fragments += abs(key[0] - FAR_CHUNK[0]) <= 2 and abs(key[1] - FAR_CHUNK[1]) <= 2
                    start += 4 + length
                    continue
            start += 1
        if payload == b"\x02":
            raise RuntimeError(f"client {self.index} disconnected during signon")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True, help="Quake data root containing id1/pak*.pak, or the id1 directory")
    parser.add_argument("--clients", type=int, default=64)
    parser.add_argument("--duration", type=float, default=10, help="seconds of movement/attack input after all clients spawn")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--output", type=Path, help="new output directory; otherwise retained under /tmp")
    parser.add_argument("--skip-far", action="store_true", help="omit the additional far-away canonical frame/chunk delivery check")
    parser.add_argument("--edit-checks", action="store_true", help="also verify remote edit, replay, stale revision and player overlap handling")
    args = parser.parse_args()
    os.environ.update(copied_engine_environment(args.binary))
    if not 1 <= args.clients <= 128 or args.duration < 0 or args.timeout <= 0:
        parser.error("clients must be 1..128 and times nonnegative")
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak game assets")
    if (source / "progs.dat").is_file():
        assets.append(source / "progs.dat")
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="quiver-network-smoke-"))
    if args.output:
        work.mkdir(parents=True, exist_ok=False)
    (work / "id1").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    binary = work / "vkquake-test"
    shutil.copy2(args.binary.resolve(), binary)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    log_path = work / "server.log"
    command = [str(binary), "-basedir", str(work), "-userdir", str(work / "user"), "-dedicated", str(args.clients), "-port", str(port),
               "+developer", "1", "+hostname", "Quiver-network-smoke", "+deathmatch", "0", "+coop", "0", "+map", "__octree"]
    selector = selectors.DefaultSelector()
    clients = []
    error = None
    max_rss = 0
    started = time.monotonic()
    with log_path.open("wb") as log:
        server = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        try:
            # The engine's stdout is block buffered when redirected to a file.
            # A server-info reply establishes readiness without depending on logs.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.settimeout(0.05)
                while True:
                    if server.poll() is not None or time.monotonic() - started > args.timeout:
                        raise RuntimeError("server did not finish loading the octree world")
                    probe.sendto(control(b"\x02QUAKE\x00\x03"), ("127.0.0.1", port))
                    try:
                        response, _ = probe.recvfrom(4096)
                    except socket.timeout:
                        continue
                    if len(response) > 5 and response[4] == 0x83 and b"__octree\x00" in response:
                        break
            clients = [Client(index, ("127.0.0.1", port), selector, godmode=True) for index in range(args.clients)]
            all_spawned_at = None
            input_started_at = None
            far_at = None
            status_at = None
            edit_stage = 0
            edit_at = 0
            while True:
                now = time.monotonic()
                if server.poll() is not None:
                    raise RuntimeError(f"server exited unexpectedly: {server.returncode}")
                if now - started > args.timeout:
                    raise RuntimeError(f"timeout: {sum(client.frames > 0 for client in clients)}/{args.clients} clients received spawned frames")
                all_spawned = all(client.frames and client.godmode_confirmed and client.health is not None for client in clients)
                if all_spawned and all_spawned_at is None:
                    all_spawned_at = now
                    print(f"{args.clients} actual UDP clients spawned with confirmed godmode transport isolation", flush=True)
                    if args.edit_checks:
                        # Immortal clients sharing a spawnpoint can hold each
                        # other above the floor. Place the edit actor at the
                        # known standing height used by the overlap assertion.
                        server.stdin.write(b"world_teleport 0 0 0 128 128 88\n")
                        server.stdin.flush()
                if all_spawned and any(client.health <= 0 for client in clients):
                    raise RuntimeError("transport fixture player died despite godmode; base-game respawn is outside this test")
                if args.edit_checks and all_spawned and edit_stage < 5:
                    actor = clients[0]
                    if edit_stage == 0 and now - all_spawned_at >= 1:
                        # Keep the successful edit clear of the player hull,
                        # including smoothing's half-cell placement margin.
                        actor.edit(1, 0, 60, x=164)
                        edit_stage = 1
                        edit_at = now
                    elif edit_stage == 1 and any(reply[:3] == (1, 0, 1) for reply in actor.edit_replies):
                        actor.edit(1, 0, 60, x=164)  # Exact duplicate must return the same ACK.
                        edit_stage = 2
                        edit_at = now
                    elif edit_stage == 2 and sum(reply[0] == 1 for reply in actor.edit_replies) >= 2 and now - edit_at > 0.2:
                        replies = [reply[3] for reply in actor.edit_replies if reply[0] == 1]
                        if replies[0] != replies[1]:
                            raise RuntimeError("duplicate edit did not return identical acknowledgement")
                        actor.edit(2, 0, 60, placing=True, x=164)  # Revision zero is now stale.
                        edit_stage = 3
                        edit_at = now
                    elif edit_stage == 3 and actor.edit_rejections["World changed; wait for the new chunk and retry"] and now - edit_at > 0.2:
                        actor.edit(3, 1, 68, placing=True, x=124)  # Intact neighboring floor supports this body-intersecting cell.
                        edit_stage = 4
                        edit_at = now
                    elif edit_stage == 4 and actor.edit_rejections["World placement overlaps an entity"] and now - edit_at > 0.2:
                        actor.edit(4, 1, 60, placing=True, x=164)
                        edit_stage = 5
                        edit_at = now
                edits_done = not args.edit_checks or any(reply[:3] == (4, 1, 2) for reply in clients[0].edit_replies)
                if args.edit_checks and edit_stage and not edits_done and now - edit_at > 10:
                    raise RuntimeError(f"remote edit check stalled at stage {edit_stage}: {dict(clients[0].edit_rejections)}")
                if all_spawned and edits_done and input_started_at is None:
                    input_started_at = now
                    if args.edit_checks:
                        print("Remote edit/replay/stale-revision/player-overlap checks passed", flush=True)
                exercised = input_started_at is not None and now - input_started_at >= args.duration
                if exercised and far_at is None and not args.skip_far:
                    if clients[0].health is None or clients[0].health <= 0 or now - clients[0].last_health_at > 1:
                        raise RuntimeError("far transport check requires a recent living-player snapshot")
                    server.stdin.write(f"world_teleport {FAR_CHUNK[0]} {FAR_CHUNK[1]} 0\n".encode())
                    server.stdin.flush()
                    far_at = now
                far_done = args.skip_far or (clients[0].far_frames > 0 and clients[0].far_fragments > 0 and
                                            clients[0].far_health is not None and clients[0].far_health > 0)
                if exercised and far_done and status_at is None:
                    server.stdin.write(b"status;world_stats\n")
                    server.stdin.flush()
                    status_at = now
                if status_at is not None and now - status_at > 1:
                    break
                moving = input_started_at is not None and not exercised
                shooting = input_started_at is not None and not exercised
                for client in clients:
                    client.pump(now, started, moving, shooting)
                for key, _ in selector.select(0.005):
                    client = key.data
                    while True:
                        try:
                            data, address = client.socket.recvfrom(65536)
                        except BlockingIOError:
                            break
                        client.receive(data, address, time.monotonic(), started)
                try:
                    status = Path(f"/proc/{server.pid}/status").read_text()
                    match = re.search(r"VmRSS:\s+(\d+)", status)
                    if match:
                        max_rss = max(max_rss, int(match.group(1)) * 1024)
                except FileNotFoundError:
                    pass
        except Exception as exc:
            error = str(exc)
        finally:
            if server.poll() is None:
                try:
                    for client in clients:
                        if client.connected:
                            client.send(packet(UNRELIABLE, client.unreliable_sequence, b"\x02"))
                    time.sleep(0.15)
                    server.stdin.write(b"quit\n")
                    server.stdin.flush()
                    server.wait(timeout=10)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    server.kill()
                    server.wait()
            for client in clients:
                client.socket.close()
            selector.close()
    if error is None:
        log_text = log_path.read_text(errors="replace")
        if not re.search(rf"players:\s+{args.clients} active", log_text):
            error = "server status did not confirm the expected active client count"
        elif any(client.name + " entered the game" not in log_text for client in clients):
            error = "server did not log every client's gameplay spawn"
    latencies = [client.spawn_seconds for client in clients if client.spawn_seconds is not None]
    gaps = [gap for client in clients for gap in client.frame_gaps]
    summary = {
        "kind": "actual UDP connection and world stream smoke; not full gameplay qualification",
        "success": error is None,
        "error": error,
        "server_returncode": server.returncode,
        "clients_requested": args.clients,
        "clients_spawned_with_frames": len(latencies),
        "transport_isolation": "deathmatch=0, coop=0; confirmed godmode before movement/attack; live player required for far phase",
        "godmode_clients_confirmed": sum(client.godmode_confirmed for client in clients),
        "clients_with_health_samples": sum(client.health is not None for client in clients),
        "minimum_observed_player_health": min((client.minimum_health for client in clients if client.minimum_health is not None), default=None),
        "far_player_health": clients[0].far_health if clients else None,
        "duration_seconds": time.monotonic() - started,
        "spawn_seconds_max": max(latencies, default=None),
        "received_snapshot_gap_median_ms": statistics.median(gaps) * 1000 if gaps else None,
        "server_rss_max_bytes": max_rss,
        "received_bytes": sum(client.in_bytes for client in clients),
        "sent_bytes": sum(client.out_bytes for client in clients),
        "move_inputs_sent": sum(client.move_commands for client in clients),
        "attack_inputs_sent": sum(client.attack_commands for client in clients),
        "chunk_fragments_received": sum(client.chunk_fragments for client in clients),
        "far_frames_client0": clients[0].far_frames if clients else 0,
        "far_chunk_fragments_client0": clients[0].far_fragments if clients else 0,
        "edit_checks_requested": args.edit_checks,
        "edit_actor_standing_position": [128, 128, 88] if args.edit_checks else None,
        "edit_acknowledgements": [list(reply[:3]) for reply in clients[0].edit_replies] if clients else [],
        "edit_rejections": dict(clients[0].edit_rejections) if clients else {},
        "server_log": str(log_path),
    }
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    if error:
        print(log_path.read_text(errors="replace")[-6000:])
        raise SystemExit(1)


if __name__ == "__main__":
    main()
