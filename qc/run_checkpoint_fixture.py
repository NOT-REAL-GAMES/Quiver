#!/usr/bin/env python3
"""Run the generated classic QuakeC checkpoint fixture in an isolated dedicated server."""

import argparse
import json
import os
from pathlib import Path
import pty
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from generate_checkpoint_fixture import generate


def run(binary, work, restart, timeout):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    master, slave = pty.openpty()
    os.set_blocking(master, False)
    command = [str(binary), "-basedir", str(work), "-userdir", str(work), "-game", "worldpos_fixture", "-dedicated", "1", "-port", str(port), "+developer", "1"]
    command += ["+world_load", "checkpoint_fixture"] if restart else ["+world_create", "checkpoint_fixture", "empty", "1"]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=slave, stderr=slave, cwd=work)
    os.close(slave)
    selector = selectors.DefaultSelector()
    selector.register(master, selectors.EVENT_READ)
    output = bytearray()

    def wait_for(text, occurrence=1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if output.count(text.encode()) >= occurrence:
                return
            if process.poll() is not None:
                raise RuntimeError(f"server exited {process.returncode} before {text!r}")
            for _, _ in selector.select(0.1):
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    chunk = b""
                output.extend(chunk)
        raise RuntimeError(f"timeout waiting for {text!r}")

    def command(text):
        process.stdin.write((text + "\n").encode())
        process.stdin.flush()

    failure = None
    try:
        if restart:
            wait_for("World position fixture remap PASS")
        else:
            wait_for("World position fixture PASS")
            command("world_save")
            wait_for("World saved")
            command("world_load checkpoint_fixture")
            wait_for("World position fixture remap PASS")
            command("world_load checkpoint_fixture")
            wait_for("World position fixture remap PASS", 2)
        command("quit")
        process.wait(timeout=10)
    except Exception as error:
        failure = str(error)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        while True:
            try:
                chunk = os.read(master, 65536)
                if not chunk:
                    break
                output.extend(chunk)
            except (BlockingIOError, OSError):
                break
        selector.close()
        os.close(master)
        log = work / ("restart.log" if restart else "checkpoint.log")
        log.write_bytes(output)
    if failure:
        raise RuntimeError(f"{failure}; log: {log}\n" + output.decode(errors="replace")[-5000:])
    if process.returncode:
        raise RuntimeError(f"server exited {process.returncode}; log: {log}")
    return str(log)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-qc-checkpoint-"))
    (work / "id1").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    (work / "worldpos_fixture").mkdir()
    (work / "worldpos_fixture/progs.dat").write_bytes(generate(Path(__file__).resolve().parents[1]))
    binary = work / "quiver-server"
    shutil.copy2(args.binary.resolve(), binary)
    summary = {"kind": "classic QC registration and canonical handle checkpoint smoke", "success": False, "work": str(work)}
    try:
        summary["checkpoint_log"] = run(binary, work, False, args.timeout)
        summary["restart_log"] = run(binary, work, True, args.timeout)
        summary["success"] = True
    finally:
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
