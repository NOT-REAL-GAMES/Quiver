#!/usr/bin/env python3
"""Save/restart a real server with edited terrain and a distant single-player entity.

Only game assets are symlinked. Configurations/saves in the supplied basedir are
never changed. The retained temporary directory contains logs and the database.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.dont_write_bytecode = True
from run_world_network_smoke import Client, FAR_CHUNK, control, copied_engine_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    args = parser.parse_args()
    os.environ.update(copied_engine_environment(args.binary))
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    work = Path(tempfile.mkdtemp(prefix="quiver-checkpoint-smoke-"))
    (work / "id1").mkdir()
    assets = list(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak")
    if (source / "progs.dat").is_file():
        assets.append(source / "progs.dat")
    for path in assets:
        (work / "id1" / path.name).symlink_to(path.resolve())
    binary = work / "vkquake-test"
    shutil.copy2(args.binary.resolve(), binary)
    results = []
    for phase in ("save", "load"):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        command = [str(binary), "-basedir", str(work), "-userdir", str(work / "user"),
                   "-dedicated", "1", "-port", str(port), "+developer", "1"]
        command += ["+world_create", "checkpoint", "terrain", "12345"] if phase == "save" else ["+world_load", "checkpoint"]
        log_path = work / f"{phase}.log"
        selector = selectors.DefaultSelector()
        client = None
        started = time.monotonic()
        with log_path.open("wb") as log:
            server = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    probe.settimeout(0.05)
                    while True:
                        if server.poll() is not None or time.monotonic() - started > 60:
                            raise RuntimeError(f"{phase}: server startup failed; see {log_path}")
                        probe.sendto(control(b"\x02QUAKE\x00\x03"), ("127.0.0.1", port))
                        try:
                            response, _ = probe.recvfrom(4096)
                        except socket.timeout:
                            continue
                        if len(response) > 5 and response[4] == 0x83 and b"__octree\x00" in response:
                            break
                client = Client(0, ("127.0.0.1", port), selector)
                stage, stage_time = 0, time.monotonic()
                while True:
                    now = time.monotonic()
                    if server.poll() is not None or now - started > 60:
                        raise RuntimeError(f"{phase}: test stalled at stage {stage}; see {log_path}")
                    client.pump(now, started, False, False)
                    for key, _ in selector.select(0.005):
                        while True:
                            try:
                                data, address = key.data.socket.recvfrom(65536)
                            except BlockingIOError:
                                break
                            key.data.receive(data, address, time.monotonic(), started)
                    if phase == "load":
                        if client.far_frames and client.far_fragments:
                            server.stdin.write(b"edict 1;world_stats;quit\n")
                            server.stdin.flush()
                            break
                    elif stage == 0 and client.frames and now - stage_time > 1:
                        client.edit(1, 0, 60)
                        stage, stage_time = 1, now
                    elif stage == 1 and client.edit_replies:
                        server.stdin.write(f"world_teleport {FAR_CHUNK[0]} {FAR_CHUNK[1]} 0\n".encode())
                        server.stdin.flush()
                        stage, stage_time = 2, now
                    elif stage == 2 and client.far_frames and client.far_fragments:
                        server.stdin.write(b"edict 1;world_save;quit\n")
                        server.stdin.flush()
                        break
                server.wait(timeout=15)
                if server.returncode != 0:
                    raise RuntimeError(f"{phase}: server exited {server.returncode}; see {log_path}")
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()
                if client is not None:
                    client.socket.close()
                selector.close()
        text = log_path.read_text(errors="replace")
        expected = "World saved" if phase == "save" else "World checkpoint restored:"
        if expected not in text or "Host_Error" in text or "Sys_Error" in text:
            raise RuntimeError(f"{phase}: missing successful checkpoint result; see {log_path}")
        results.append({"phase": phase, "frames": client.frames, "far_frames": client.far_frames,
                        "far_fragments": client.far_fragments, "log": str(log_path)})
    databases = list(work.rglob("checkpoint.sqlite3"))
    if len(databases) != 1:
        raise RuntimeError("expected exactly one named world database")
    with sqlite3.connect(databases[0]) as db:
        assert db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        # Current terrain survives ordinary reload without retaining generated
        # full-chunk payloads in the legacy table.
        assert db.execute("SELECT COUNT(*) FROM chunks").fetchone() == (0,)
        assert db.execute("SELECT revision FROM terrain_revisions WHERE x=0 AND y=0 AND z=0").fetchone()[0] >= 1
        assert db.execute("SELECT COUNT(*) FROM world_snapshots WHERE name='latest'").fetchone() == (1,)
        checkpoint_bytes = db.execute("SELECT length(data) FROM world_checkpoint WHERE id=1").fetchone()[0]
        assert checkpoint_bytes > 0
    # Recompute the storage checksum so the engine's typed-value validation,
    # rather than the SQLite payload checksum, must reject this malformed file.
    invalid = databases[0].with_name("invalid.sqlite3")
    with sqlite3.connect(databases[0]) as source_db, sqlite3.connect(invalid) as target_db:
        source_db.backup(target_db)
        data = bytearray(target_db.execute("SELECT data FROM world_checkpoint WHERE id=1").fetchone()[0])
        assert data[:4] in (b"QWS1", b"QWS2")
        offset = 45 + 64 * 4  # Header plus player spawn parameters.
        value_count = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        mutated = False
        for _ in range(value_count):
            length = struct.unpack_from("<I", data, offset)[0]
            offset += 4 + length
            kind = struct.unpack_from("<H", data, offset)[0]
            offset += 2
            if kind == 2:  # ev_float
                struct.pack_into("<I", data, offset, 0x7fc00000)
                mutated = True
                break
            if kind == 1:
                length = struct.unpack_from("<I", data, offset)[0]
                offset += 4 + length
            else:
                offset += 12 if kind == 3 else 8 if kind in (10, 11, 12) else 4
        assert mutated, "test save contains no persistent scalar to corrupt"
        target_db.execute("UPDATE world_checkpoint SET data=?,checksum=? WHERE id=1", (data, zlib.crc32(data)))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    invalid_log = work / "invalid.log"
    command = [str(binary), "-basedir", str(work), "-userdir", str(work / "user"), "-dedicated", "1", "-port", str(port), "+world_load", "invalid"]
    with invalid_log.open("wb") as log:
        rejected = subprocess.run(command, input=b"quit\n", stdout=log, stderr=subprocess.STDOUT, cwd=work, timeout=20)
    assert "nonfinite checkpoint float/vector" in invalid_log.read_text(errors="replace"), "invalid checkpoint was not rejected"
    summary = {"success": True, "database": str(databases[0]), "checkpoint_bytes": checkpoint_bytes,
               "nonfinite_checkpoint_rejected": True, "invalid_log": str(invalid_log), "runs": results}
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
