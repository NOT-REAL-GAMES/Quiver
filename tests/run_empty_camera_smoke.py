#!/usr/bin/env python3
"""Exercise the native __empty camera through an actual UDP client.

The generated QuakeC fixture aborts if any gameplay callback runs in __empty.
A second fixture has no gameplay or spawn functions at all. Both must sign on,
fly along the view direction, move vertically, idle without gravity, survive
attack/kill commands, change levels, disconnect, and reconnect. The first also
transitions into a named octree world, where normal worldspawn is permitted.

Only pak*.pak assets are linked into fresh temporary directories. The generated
program, configuration, saves, database, binary copy and logs stay isolated from
the supplied game installation. This tests server camera behavior and transport;
it does not test rendering or graphical-client prediction.

Example:
  python3 tests/run_empty_camera_smoke.py --binary build/vkquake --basedir .
"""

import argparse
import json
import math
import os
from pathlib import Path
import pty
import re
import selectors
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "qc"))
from generate_checkpoint_fixture import Program
from run_world_network_smoke import Client, MARKER, UNRELIABLE, packet


FAILURE = "EMPTY_CAMERA_UNEXPECTED_CALLBACK:"
TRANSITION = "EMPTY_CAMERA_OCTREE_WORLDSPAWN_OK"
NEW_PARMS = "EMPTY_CAMERA_GAMEPLAY_PARMS_OK"
GAMEPLAY_SPAWN = "EMPTY_CAMERA_GAMEPLAY_SPAWN_OK"
CALLBACKS = (
    "worldspawn", "SetNewParms", "ClientConnect", "PutClientInServer", "StartFrame",
    "PlayerPreThink", "PlayerPostThink", "ClientDisconnect", "ClientKill",
    "SetChangeParms", "SV_ParseClientCommand", "info_player_start", "info_player_deathmatch", "CSQC_Init", "CSQC_DrawHud",
)
FIELDS = (
    "origin", "velocity", "health", "deadflag", "movetype", "solid", "model", "modelindex",
    "weaponmodel", "weapon", "takedamage", "view_ofs", "classname", "flags",
)


def generate(hostile):
    p = Program(ROOT)
    p.function("main")
    if not hostile:
        return p.encode()
    p.function("error", -10)
    p.function("dprint", -25)
    empty_name = p.text("fixture_empty_name", "__empty")
    result = p.global_value("fixture_result", 2)
    transition = p.text("fixture_transition", TRANSITION + "\n")
    new_parms = p.text("fixture_new_parms", NEW_PARMS + "\n")
    gameplay_spawn = p.text("fixture_gameplay_spawn", GAMEPLAY_SPAWN + "\n")
    spawn_value = p.global_value("fixture_spawn_value", 2, struct.unpack("<I", struct.pack("<f", 123))[0])
    health_value = p.global_value("fixture_health", 2, struct.unpack("<I", struct.pack("<f", 100))[0])
    health_field = p.global_value("fixture_health_field", 5, p.field_offsets["health"])
    pointer = p.global_value("fixture_pointer", 7)
    for name in CALLBACKS:
        failure = p.text("fixture_failure_" + name, FAILURE + name)
        p.function(name, len(p.statements))
        if name in ("CSQC_Init", "CSQC_DrawHud"):
            p.call("error", (failure, False))
            p.op("DONE")
            continue
        p.op("NE_S", p.global_offsets["mapname"], empty_name, result)
        p.assert_true(result, failure)
        if name == "worldspawn":
            p.call("dprint", (transition, False))
        elif name == "SetNewParms":
            p.op("STORE_F", spawn_value, p.global_offsets["parm1"])
            p.call("dprint", (new_parms, False))
        elif name == "PutClientInServer":
            p.op("EQ_F", p.global_offsets["parm1"], spawn_value, result)
            p.assert_true(result, failure)
            p.op("ADDRESS", p.global_offsets["self"], health_field, pointer)
            p.op("STOREP_F", health_value, pointer)
            p.call("dprint", (gameplay_spawn, False))
        p.op("DONE")
    return p.encode()


class CameraClient(Client):
    """Reuse the existing reliable UDP transport without its octree assumptions."""

    def __init__(self, index, address, selector):
        super().__init__(index, address, selector)
        self.name = f"emptycamera{index:03d}"
        self.input = (0, 0, 0, 0, 0, 0, 0)  # pitch/yaw, forward/side/up, buttons/impulse
        self.last_input = 0
        self.signons = 0
        self.reconnections = 0
        self.expect_disconnect = False

    def pump(self, now, started, moving=False, shooting=False):
        # Keep the helper's retry/ACK state machine, replacing its rotating input.
        self.last_move = now
        super().pump(now, started, False, False)
        if self.signon >= 3 and now - self.last_input >= 0.025:
            pitch, yaw, forward, side, up, buttons, impulse = self.input
            angles = [int(value * 65536 / 360) & 0xffff for value in (pitch, yaw, 0)]
            payload = b"\x03" + struct.pack("<fHHHhhhBB", self.server_time, *angles, forward, side, up, buttons, impulse)
            self.send(packet(UNRELIABLE, self.unreliable_sequence, payload))
            self.unreliable_sequence += 1
            self.move_commands += 1
            self.last_input = now

    def receive(self, data, address, now, started):
        if (self.expect_disconnect and len(data) >= 9 and data[8:] == b"\x02" and
                struct.unpack_from(">I", data)[0] & UNRELIABLE):
            return
        super().receive(data, address, now, started)
        if len(data) < 9 or not struct.unpack_from(">I", data)[0] & UNRELIABLE:
            return
        payload = data[8:]
        if payload[:1] == b"\x07" and len(payload) >= 5:
            self.server_time = struct.unpack_from("<f", payload, 1)[0]
            self.frames += 1
            health = self.snapshot_health(payload, 0)
            if health is not None:
                self.health = health

    def message(self, payload):
        if b"\x09reconnect\n\x00" in payload:
            self.signon = 0
            self.reconnections += 1
        if b"cmd pext\n\x00" in payload and not self.pext_sent:
            self.command(f"pext {MARKER} 1\n")
            self.pext_sent = True
        stage = self.signon_stage(payload)
        if stage == 1 or (stage is not None and stage > self.signon):
            self.signon = stage
            if stage == 1:
                self.command(f'name "{self.name}"\n', "prespawn\n")
            elif stage == 2:
                self.command("spawn\n")
            else:
                self.signons += 1
                self.command("begin\n")
        if payload == b"\x02" and not self.expect_disconnect:
            raise RuntimeError("camera client disconnected unexpectedly")


class Server:
    def __init__(self, binary, work, timeout):
        self.work, self.timeout = work, timeout
        self.output = bytearray()
        self.selector = selectors.DefaultSelector()
        self.client = None
        self.clients_created = 0
        self.snapshots = []
        self.started = time.monotonic()
        self.master, slave = pty.openpty()
        os.set_blocking(self.master, False)
        self.selector.register(self.master, selectors.EVENT_READ, "console")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_socket:
            port_socket.bind(("127.0.0.1", 0))
            self.port = port_socket.getsockname()[1]
        # The engine's reconstructed command line is bounded; short cwd-relative
        # roots keep +map intact even when the retained output path is long.
        command = [os.path.relpath(binary, work), "-basedir", ".", "-userdir", "user",
                   "-game", "empty_fixture", "-dedicated", "1", "-port", str(self.port),
                   "+developer", "1", "+sv_protocol", "999", "+sv_altnoclip", "0",
                   "+deathmatch", "0", "+coop", "0", "+map", "__empty"]
        self.process = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=slave, stderr=slave)
        os.close(slave)

    def connect(self):
        self.client = CameraClient(self.clients_created, ("127.0.0.1", self.port), self.selector)
        self.clients_created += 1
        self.wait(lambda: self.client.signons == 1 and self.client.frames > 0 and self.client.health == 100, "native camera signon")

    def send(self, command):
        # Sys_ConsoleInput strips newlines before appending console text, so an
        # explicit delimiter also keeps several queued lines from concatenating.
        self.process.stdin.write((command + ";\n").encode())
        self.process.stdin.flush()

    def pump(self):
        if self.process.poll() is not None:
            raise RuntimeError(f"server exited unexpectedly: {self.process.returncode}")
        if self.client is not None:
            self.client.pump(time.monotonic(), self.started)
        for key, _ in self.selector.select(0.01):
            if key.data == "console":
                try:
                    self.output.extend(os.read(self.master, 65536))
                except OSError:
                    pass
            else:
                client = key.data
                while True:
                    try:
                        data, address = client.socket.recvfrom(65536)
                    except BlockingIOError:
                        break
                    client.receive(data, address, time.monotonic(), self.started)
        if FAILURE.encode() in self.output:
            raise RuntimeError("gameplay callback ran inside __empty")
        if len(self.output) > 8 * 1024 * 1024:
            raise RuntimeError("server log exceeded test budget")

    def wait(self, predicate, description):
        deadline = time.monotonic() + self.timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise RuntimeError("timeout waiting for " + description)
            self.pump()

    def duration(self, seconds):
        deadline = time.monotonic() + seconds
        self.wait(lambda: time.monotonic() >= deadline, f"{seconds}s of camera input")

    def snapshot(self, label):
        marker = f"EMPTY_CAMERA_SNAPSHOT_{len(self.snapshots)}"
        offset = len(self.output)
        self.send(";\n".join([*("edict 1 " + field for field in FIELDS), "echo " + marker]))
        self.wait(lambda: marker.encode() in self.output[offset:], "entity snapshot " + label)
        raw = self.output[offset:].decode(errors="replace")
        values = dict(re.findall(r"Edict 1\.(\w+)==([^\r\n]*)", raw))
        if any(field not in values for field in FIELDS):
            raise RuntimeError("incomplete entity snapshot: " + raw)
        result = {"label": label}
        for field, value in values.items():
            if field in ("origin", "velocity", "view_ofs"):
                result[field] = [float(component) for component in value.split()]
                if len(result[field]) != 3 or not all(math.isfinite(component) for component in result[field]):
                    raise RuntimeError("non-finite or malformed entity vector")
            elif field in ("model", "weaponmodel", "classname"):
                result[field] = value
            else:
                result[field] = float(value)
        if (result["health"] != 100 or result["deadflag"] != 0 or result["movetype"] != 8 or
                result["solid"] != 0 or result["model"] or result["modelindex"] != 0 or
                result["weaponmodel"] or result["weapon"] != 0 or result["takedamage"] != 0 or
                result["view_ofs"] != [0, 0, 0] or result["classname"] != "freefly_camera" or
                not int(result["flags"]) & 8):
            raise RuntimeError("invalid native camera state: " + repr(result))
        self.snapshots.append(result)
        return result["origin"]

    def move(self, label, *, pitch=0, forward=0, up=0):
        self.client.input = (pitch, 0, forward, 0, up, 0, 0)
        self.duration(0.6)
        self.client.input = (pitch, 0, 0, 0, 0, 0, 0)
        self.duration(0.15)
        return self.snapshot(label)

    def disconnect(self):
        self.client.send(packet(UNRELIABLE, self.client.unreliable_sequence, b"\x02"))
        self.selector.unregister(self.client.socket)
        self.client.socket.close()
        self.client = None
        self.duration(0.3)
        offset = len(self.output)
        self.send("status")
        self.wait(lambda: re.search(rb"players:\s+0 active", self.output[offset:]), "client disconnect")

    def close(self):
        failure = None
        if self.process.poll() is None:
            if self.client is not None:
                self.client.expect_disconnect = True
            self.send("quit")
            deadline = time.monotonic() + 10
            while self.process.poll() is None and time.monotonic() < deadline:
                try:
                    self.pump()
                except RuntimeError as error:
                    if self.process.poll() is None:
                        failure = str(error)
                        break
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait()
        while True:
            try:
                data = os.read(self.master, 65536)
                if not data:
                    break
                self.output.extend(data)
            except (OSError, BlockingIOError):
                break
        if self.client is not None:
            self.client.socket.close()
        self.selector.close()
        os.close(self.master)
        self.process.stdin.close()
        log = self.work / "server.log"
        log.write_bytes(self.output)
        if failure or self.process.returncode or FAILURE.encode() in self.output:
            raise RuntimeError(f"{failure or 'server or callback failed'}; log: {log}")


def run(binary, work, timeout, hostile):
    server = Server(binary, work, timeout)
    try:
        server.connect()
        origin = server.snapshot("spawn")
        if origin != [0, 0, 96]:
            raise RuntimeError("camera did not spawn at the native origin")
        pitched = server.move("pitched forward", pitch=-45, forward=200)
        dx, dy, dz = [after - before for before, after in zip(origin, pitched)]
        if dx < 40 or dz < 40 or abs(dy) > 0.1 or not 0.9 < dz / dx < 1.1:
            raise RuntimeError("forward movement did not follow camera pitch: " + repr((dx, dy, dz)))
        raised = server.move("up", up=200)
        lowered = server.move("down", up=-200)
        for before, after, sign in ((pitched, raised, 1), (raised, lowered, -1)):
            if abs(after[0] - before[0]) > 0.1 or abs(after[1] - before[1]) > 0.1 or sign * (after[2] - before[2]) < 60:
                raise RuntimeError("vertical camera movement failed")
        server.duration(0.75)
        idle = server.snapshot("idle without gravity")
        if math.dist(idle, lowered) > 0.01 or server.snapshots[-1]["velocity"] != [0, 0, 0]:
            raise RuntimeError("idle camera drifted or fell")
        server.client.input = (0, 0, 0, 0, 0, 3, 255)
        server.client.command("kill\n", "empty_camera_unknown_command\n", "status\n")
        server.wait(lambda: not server.client.queue and server.client.pending is None, "client command acknowledgement")
        server.duration(0.25)
        server.client.input = (0, 0, 0, 0, 0, 0, 0)
        server.snapshot("attack, jump and kill ignored")
        signons, frames = server.client.signons, server.client.frames
        server.send("changelevel __empty")
        server.wait(lambda: server.client.signons > signons and server.client.frames > frames + 2, "changelevel and repeated signon")
        if server.snapshot("changelevel camera") != [0, 0, 96]:
            raise RuntimeError("changelevel did not reset the camera")
        server.disconnect()
        server.connect()
        server.snapshot("reconnected camera")
        if hostile:
            signons = server.client.signons
            server.send("changelevel __octree")
            server.wait(lambda: GAMEPLAY_SPAWN.encode() in server.output and server.client.signons > signons,
                        "same-connection transition with fresh gameplay spawn parameters")
            if NEW_PARMS.encode() not in server.output or TRANSITION.encode() not in server.output:
                raise RuntimeError("gameplay callbacks did not resume after leaving __empty")
            signons, frames = server.client.signons, server.client.frames
            server.send("changelevel __empty")
            server.wait(lambda: server.client.signons > signons and server.client.frames > frames + 2,
                        "gameplay-to-empty transition")
            server.snapshot("camera after gameplay transition")
            offset = len(server.output)
            server.client.expect_disconnect = True
            server.send("world_create empty_camera_transition empty 42")
            server.wait(lambda: TRANSITION.encode() in server.output[offset:], "world_create gameplay transition")
            server.duration(0.2)
            server.send("status;echo EMPTY_CAMERA_TRANSITION_STATUS")
            server.wait(lambda: b"EMPTY_CAMERA_TRANSITION_STATUS" in server.output, "post-transition console response")
            if not re.search(rb"map:\s+__octree", server.output):
                raise RuntimeError("world_create did not switch to the octree map")
        result = {"fixture": "hostile callbacks" if hostile else "no gameplay functions", "snapshots": server.snapshots,
                  "clients_connected": server.clients_created, "final_client_frames": server.client.frames,
                  "same_connection_gameplay_spawn_parameters": hostile,
                  "world_create_transition": hostile, "log": str(work / "server.log")}
    except Exception as error:
        raise RuntimeError(f"{error}; log: {work / 'server.log'}") from error
    finally:
        server.close()
    if hostile:
        databases = list(work.rglob("empty_camera_transition.sqlite3"))
        if len(databases) != 1:
            raise RuntimeError("world_create did not create exactly one isolated database")
        with sqlite3.connect(databases[0]) as db:
            if db.execute("PRAGMA integrity_check").fetchone() != ("ok",):
                raise RuntimeError("world_create database failed integrity_check")
        result["database"] = str(databases[0])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30, help="maximum seconds per awaited event")
    parser.add_argument("--output", type=Path, help="new output directory; otherwise retained under /tmp")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 1:
        parser.error("timeout must be finite and greater than one second")
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak assets")
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="quiver-empty-camera-smoke-"))
    if args.output:
        work.mkdir(parents=True, exist_ok=False)
    binary = work / "quiver-server"
    shutil.copy2(args.binary.resolve(), binary)
    summary = {"kind": "native empty-world camera and callback isolation smoke", "success": False, "work": str(work), "runs": []}
    try:
        for hostile in (True, False):
            variant = work / ("hostile" if hostile else "minimal")
            (variant / "id1").mkdir(parents=True)
            (variant / "empty_fixture").mkdir()
            for asset in assets:
                (variant / "id1" / asset.name).symlink_to(asset.resolve())
            (variant / "empty_fixture/progs.dat").write_bytes(generate(hostile))
            summary["runs"].append(run(binary, variant, args.timeout, hostile))
        summary["success"] = True
    except Exception as error:
        summary["error"] = str(error)
        raise
    finally:
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
