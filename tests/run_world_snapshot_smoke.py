#!/usr/bin/env python3
"""Exercise named terrain/game-state snapshots through real dedicated-server commands.

Only game assets are symlinked into a temporary directory. Test saves and logs are
retained there. Uses real UDP clients and the versioned world protocol.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import time

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
    assets = list(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak")
    if (source / "progs.dat").is_file():
        assets.append(source / "progs.dat")
    work = Path(tempfile.mkdtemp(prefix="quiver-snapshot-smoke-"))
    (work / "id1").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    binary = work / "vkquake-test"
    shutil.copy2(args.binary.resolve(), binary)
    results = []
    for phase in ("original", "changed", "unsaved", "restore", "verify"):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        command = [str(binary), "-basedir", str(work), "-userdir", str(work / "user"),
                   "-dedicated", "1", "-port", str(port), "+developer", "1"]
        command += ["+world_create", "snapshots", "terrain", "12345"] if phase == "original" else ["+world_load", "snapshots"]
        log_path = work / f"{phase}.log"
        selector = selectors.DefaultSelector()
        client = None
        started = time.monotonic()
        with log_path.open("wb") as log:
            server = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            def send(text):
                server.stdin.write((text + "\n").encode())
                server.stdin.flush()
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    probe.settimeout(0.05)
                    while True:
                        if server.poll() is not None or time.monotonic() - started > 60:
                            raise RuntimeError(f"{phase}: startup failed; see {log_path}")
                        probe.sendto(control(b"\x02QUAKE\x00\x03"), ("127.0.0.1", port))
                        try:
                            response, _ = probe.recvfrom(4096)
                        except socket.timeout:
                            continue
                        if len(response) > 5 and response[4] == 0x83 and b"__octree\x00" in response:
                            break
                client = Client(0, ("127.0.0.1", port), selector, godmode=True)
                stage, stage_time = 0, time.monotonic()
                initial_root = None
                while True:
                    now = time.monotonic()
                    if server.poll() is not None or now - started > 60:
                        raise RuntimeError(f"{phase}: stalled at {stage}: {dict(client.edit_rejections)}; see {log_path}")
                    client.pump(now, started, False, False)
                    for key, _ in selector.select(0.005):
                        while True:
                            try:
                                data, address = key.data.socket.recvfrom(65536)
                            except BlockingIOError:
                                break
                            key.data.receive(data, address, time.monotonic(), started)
                    if stage == 0 and client.frames and client.godmode_confirmed:
                        initial_root = client.world_root.hex()
                        if phase == "restore":
                            # Inserted map reload executes ahead of the remaining commands.
                            send("world_snapshot restore original;wait;quit")
                            break
                        if phase == "verify":
                            if not client.far_frames:
                                raise RuntimeError("snapshot did not restore the saved player position")
                            send("world_snapshot list;world_snapshot delete changed;world_snapshot gc;world_snapshot list;quit")
                            break
                        if phase == "changed" and not client.far_frames:
                            raise RuntimeError("initial snapshot did not persist its saved player")
                        send("world_teleport 0 0 0 128 128 88")
                        stage, stage_time = 1, now
                    elif stage == 1 and client.last_frame_chunk == (0, 0, 0) and (0, 0, 0) in client.chunk_revisions and now - stage_time > 1:
                        revision = client.chunk_revisions[(0, 0, 0)]
                        client.edit(1, revision, 60, placing=phase == "changed", x=172 if phase == "unsaved" else 164)
                        stage, stage_time = 2, now
                    elif stage == 2 and client.edit_replies:
                        if phase == "unsaved":
                            # Wait for the root announcement, then exit without updating
                            # latest. An acknowledged edit must survive ordinary loading.
                            if client.world_root.hex() != initial_root:
                                send("quit")
                                break
                        elif phase == "original":
                            send(f"world_teleport {FAR_CHUNK[0]} {FAR_CHUNK[1]} 0")
                            stage, stage_time = 3, now
                        else:
                            send("world_snapshot create changed;world_save;quit")
                            break
                    elif stage == 3 and client.far_frames and client.far_fragments:
                        send("world_snapshot create original;world_snapshot create original;world_save;quit")
                        break
                server.wait(timeout=30)
                if server.returncode != 0:
                    raise RuntimeError(f"{phase}: server exited {server.returncode}; see {log_path}")
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()
                if client:
                    client.socket.close()
                selector.close()
        output = log_path.read_text(errors="replace")
        if "Host_Error" in output or "Sys_Error" in output:
            raise RuntimeError(f"{phase}: engine error; see {log_path}")
        required = {"original": "World snapshot create: original", "changed": "World snapshot create: changed",
                    "unsaved": "Teleported to chunk 0 0 0", "restore": "World snapshot restore: original",
                    "verify": "World snapshot delete: changed"}[phase]
        if required not in output:
            raise RuntimeError(f"{phase}: missing {required}; see {log_path}")
        if phase == "original" and "World snapshot create failed:" not in output:
            raise RuntimeError("duplicate snapshot name was overwritten")
        if phase == "verify" and "World snapshot garbage collection complete" not in output:
            raise RuntimeError("snapshot GC command failed")
        if phase == "restore" and output.count("World checkpoint restored:") != 2:
            raise RuntimeError("snapshot restore did not perform a session reload")
        results.append({"phase": phase, "initial_root": initial_root, "final_root": client.world_root.hex(), "epoch": client.authority_epoch,
                        "far_frames": client.far_frames, "edit_replies": [list(r[:3]) for r in client.edit_replies], "log": str(log_path)})
    runs = {run["phase"]: run for run in results}
    # Ordinary loading preserves durable post-checkpoint edits. Only an explicit
    # snapshot restore rolls both terrain and game state back to the named pin.
    assert runs["unsaved"]["initial_root"] != runs["unsaved"]["final_root"], "fixture did not commit its unsaved edit"
    assert runs["restore"]["initial_root"] == runs["unsaved"]["final_root"], "acknowledged edit was lost on ordinary cold load"
    assert runs["changed"]["initial_root"] == runs["verify"]["initial_root"], "terrain root was not restored"
    assert runs["restore"]["initial_root"] != runs["verify"]["initial_root"], "fixture failed to alter terrain"
    assert runs["verify"]["epoch"] > runs["restore"]["epoch"], "restoration did not advance authority epoch"
    summary = {"success": True, "work": str(work), "runs": results}
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
