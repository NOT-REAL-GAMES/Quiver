#!/usr/bin/env python3
"""Walk west through streamed chunks using one real UDP client.

The test sends only ordinary forward movement: no jumping, noclip, teleport, or
position writes. It requires three westward canonical chunk boundary crossings
within 45 seconds, checks that the player remains above the solid lower layer,
and records server RSS, chunk transfers, grounded frames, and world diagnostics.
Only game assets are linked into an isolated temporary game directory.
"""

import argparse
import json
from pathlib import Path
import re
import selectors
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

from run_world_network_smoke import Client, UNRELIABLE, WIRE_PREFIX, control, packet


class WalkingClient(Client):
    def __init__(self, address, selector):
        super().__init__(0, address, selector)
        self.walking = False
        self.chunks = []
        self.transitions = []
        self.grounded_frames = 0
        self.clientdata_frames = 0
        self.position_samples = 0
        self.first_position = None
        self.last_position = None
        self.minimum_height = None

    def send(self, data):
        # Reuse connection/reliable handling from Client, replacing just its
        # rotating movement sample with a fixed yaw and ordinary walk input.
        if len(data) == 27 and struct.unpack_from(">I", data)[0] & UNRELIABLE and data[8] == 3:
            sequence = struct.unpack_from(">I", data, 4)[0]
            move = b"\x03" + struct.pack("<fHHHhhhBB", self.server_time, 0, 32768, 0,
                                         320 if self.walking else 0, 0, 0, 0, 0)
            data = packet(UNRELIABLE, sequence, move)
        super().send(data)

    def receive(self, data, address, now, started):
        super().receive(data, address, now, started)
        if len(data) < 56 or not struct.unpack_from(">I", data)[0] & UNRELIABLE:
            return
        payload = data[8:]
        if payload[:4] != b"\x57\x00\x2c\x00" or payload[4:12] != WIRE_PREFIX:
            return
        chunk = struct.unpack_from("<qqq", payload, 12)
        if not self.chunks or self.chunks[-1] != chunk:
            self.chunks.append(chunk)
            self.transitions.append({"seconds": now - started, "chunk": list(chunk)})
        if chunk[2] < 0:
            raise RuntimeError(f"player fell below the generated solid lower layer: chunk {chunk}")
        # RMQ FLOATCOORD/SHORTANGLE snapshots put entity updates before the
        # clientdata service. Parse their lengths instead of searching arbitrary
        # payload bytes for a service value. Fresh reserved-player baselines are
        # at origin zero in this engine-generated fixture.
        offset = 48
        while offset < len(payload):
            service = payload[offset]
            offset += 1
            if service & 128:
                bits = service & 127
                if bits & 1:
                    bits |= payload[offset] << 8
                    offset += 1
                if bits & (1 << 15):
                    bits |= payload[offset] << 16
                    offset += 1
                if bits & (1 << 23):
                    bits |= payload[offset] << 24
                    offset += 1
                entity_size = 2 if bits & (1 << 14) else 1
                entity = int.from_bytes(payload[offset:offset + entity_size], "little")
                offset += entity_size
                for flag in (10, 6, 11, 12, 13):
                    offset += bool(bits & (1 << flag))
                local = [0.0, 0.0, 0.0]
                for axis, angle_bit in enumerate((8, 4, 9)):
                    if bits & (1 << (axis + 1)):
                        local[axis] = struct.unpack_from("<f", payload, offset)[0]
                        offset += 4
                    if bits & (1 << angle_bit):
                        offset += 2
                for flag in (16, 20, 17, 18, 19):
                    offset += bool(bits & (1 << flag))
                if entity == 1:
                    position = [chunk[axis] * 1024 + local[axis] for axis in range(3)]
                    if position[2] < 87.5:
                        raise RuntimeError(f"player fell below the terrain's minimum floor height: {position}")
                    self.position_samples += 1
                    if self.first_position is None:
                        self.first_position = position
                    self.last_position = position
                    self.minimum_height = position[2] if self.minimum_height is None else min(self.minimum_height, position[2])
            elif service == 7:
                offset += 4  # svc_time
            elif service == 10:
                offset += 6  # svc_setangle
            elif service == 19:
                offset += 14  # svc_damage: armor, blood, three float coordinates
            elif service == 15:
                bits = struct.unpack_from("<H", payload, offset)[0]
                self.clientdata_frames += 1
                self.grounded_frames += bool(bits & (1 << 10))
                # Clientdata is last for this non-FTE, combat-free fixture.
                return
            else:
                # Optional effects are outside this test's parser. Other frames
                # still supply authoritative movement and grounded samples.
                return



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--duration", type=float, default=45, help="walking budget in seconds (default: 45)")
    args = parser.parse_args()
    if not 1 <= args.duration <= 120:
        parser.error("duration must be between 1 and 120 seconds")
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak game assets")
    if (source / "progs.dat").is_file():
        assets.append(source / "progs.dat")
    work = Path(tempfile.mkdtemp(prefix="quiver-travel-smoke-"))
    (work / "id1").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    binary = work / "vkquake-test"
    shutil.copy2(args.binary.resolve(), binary)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    command = [str(binary), "-basedir", str(work), "-userdir", str(work / "user"),
               "-dedicated", "1", "-port", str(port), "+developer", "1", "+map", "__octree"]
    selector = selectors.DefaultSelector()
    client = None
    error = None
    max_rss = 0
    rss = []
    started = time.monotonic()
    walking_at = None
    walking_elapsed = 0.0
    initial = None
    crossed = 0
    log_path = work / "server.log"
    with log_path.open("wb") as log:
        server = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.settimeout(0.05)
                while True:
                    if server.poll() is not None or time.monotonic() - started > 60:
                        raise RuntimeError("server did not finish loading the octree world")
                    probe.sendto(control(b"\x02QUAKE\x00\x03"), ("127.0.0.1", port))
                    try:
                        response, _ = probe.recvfrom(4096)
                    except socket.timeout:
                        continue
                    if len(response) > 5 and response[4] == 0x83 and b"__octree\x00" in response:
                        break
            client = WalkingClient(("127.0.0.1", port), selector)
            diagnostic_at = started - 5
            sample_at = started - 1
            while True:
                now = time.monotonic()
                if server.poll() is not None:
                    raise RuntimeError(f"server exited unexpectedly: {server.returncode}")
                if walking_at is None and now - started > 60:
                    raise RuntimeError("client did not spawn")
                if client.frames and walking_at is None:
                    initial = client.chunks[0]
                    walking_at = now
                    client.walking = True
                    print(f"Walking west from canonical chunk {initial}", flush=True)
                if walking_at is not None:
                    walking_elapsed = now - walking_at
                    crossed = initial[0] - min(chunk[0] for chunk in client.chunks)
                    if crossed >= 3:
                        if not client.grounded_frames or not client.position_samples or not client.chunk_fragments:
                            raise RuntimeError("boundary travel lacks grounded frames or streamed chunk data")
                        break
                    if now - walking_at >= args.duration:
                        raise RuntimeError(f"only crossed {crossed}/3 westward chunk boundaries in {args.duration:g} seconds")
                client.pump(now, started, True, False)
                for key, _ in selector.select(0.005):
                    while True:
                        try:
                            data, address = key.data.socket.recvfrom(65536)
                        except BlockingIOError:
                            break
                        key.data.receive(data, address, time.monotonic(), started)
                if now - diagnostic_at >= 5:
                    server.stdin.write(b"world_stats;edict 1\n")
                    server.stdin.flush()
                    diagnostic_at = now
                if now - sample_at >= 0.2:
                    sample_at = now
                    try:
                        match = re.search(r"VmRSS:\s+(\d+)", Path(f"/proc/{server.pid}/status").read_text())
                        if match:
                            value = int(match.group(1)) * 1024
                            max_rss = max(max_rss, value)
                            rss.append({"seconds": now - started, "bytes": value})
                    except FileNotFoundError:
                        pass
        except Exception as exception:
            error = str(exception)
        finally:
            if server.poll() is None:
                try:
                    if client:
                        client.walking = False
                    server.stdin.write(b"status;world_stats;edict 1;quit\n")
                    server.stdin.flush()
                    server.wait(timeout=15)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    server.kill()
                    server.wait()
            if client:
                client.socket.close()
            selector.close()
    text = log_path.read_text(errors="replace")
    if error is None and (server.returncode != 0 or "Host_Error" in text or "Sys_Error" in text):
        error = "server exited with an error; see server log"
    summary = {
        "kind": "real UDP walking and streaming smoke; no teleport, noclip, jumping, or attack",
        "success": error is None,
        "error": error,
        "server_returncode": server.returncode,
        "duration_seconds": time.monotonic() - started,
        "walking_seconds": walking_elapsed,
        "westward_boundaries_crossed": crossed,
        "canonical_transitions": client.transitions if client else [],
        "snapshot_frames": client.frames if client else 0,
        "grounded_frames": client.grounded_frames if client else 0,
        "clientdata_frames": client.clientdata_frames if client else 0,
        "position_samples": client.position_samples if client else 0,
        "first_position": client.first_position if client else None,
        "last_position": client.last_position if client else None,
        "minimum_player_height": client.minimum_height if client else None,
        "unknown_chunk_fault_injection": False,
        "chunk_fragments": client.chunk_fragments if client else 0,
        "movement_inputs": client.move_commands if client else 0,
        "server_rss_sampled_max_bytes": max_rss,
        "rss_samples": rss,
        "server_log": str(log_path),
    }
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    raise SystemExit(0 if error is None else 1)


if __name__ == "__main__":
    main()
