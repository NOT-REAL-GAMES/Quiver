#!/usr/bin/env python3
"""Check streamed Jolt player movement against Quake pushers and QC callbacks.

An original generated QC program creates a moving box platform and a trigger.
An actual UDP client lands, rides the platform diagonally, and verifies exact
relative support, groundentity, solid touch, and trigger touch. Assets and saved
settings are isolated. No GPU or external QC compiler is required.
"""

import argparse
import json
import os
from pathlib import Path
import pty
import selectors
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "qc"))
from generate_checkpoint_fixture import Program
from run_world_network_smoke import Client, copied_engine_environment

PASS = "Jolt entity fixture PASS"


def brush_model(lo, hi):
    """Original minimal BSP29: exact box render hull plus two expanded hulls.

    Dedicated-server collision needs planes/nodes/leaves, but no rendered faces
    or textures. The physical bounds deliberately differ from the entity bbox
    expansion, so the fixture exercises brush hull clipping rather than proxies.
    """
    lumps = [b""] * 15
    planes, nodes, clips = [], [], []
    for hull, (hlo, hhi) in enumerate(((lo, hi),
                                      (tuple(lo[a] - (16, 16, 32)[a] for a in range(3)),
                                       tuple(hi[a] + (16, 16, 24)[a] for a in range(3))),
                                      (tuple(lo[a] - (32, 32, 64)[a] for a in range(3)),
                                       tuple(hi[a] + (32, 32, 24)[a] for a in range(3))))):
        for side in range(6):
            axis, upper = divmod(side, 2)
            normal = [0, 0, 0]; normal[axis] = 1
            plane = len(planes)
            planes.append(struct.pack("<4fi", *normal, (hhi if upper else hlo)[axis], axis))
            inside = side + 1 if side < 5 else -1
            children = (-2, inside) if upper else (inside, -2)
            if hull == 0:
                nodes.append(struct.pack("<i8h2H", plane, *children, *lo, *hi, 0, 0))
            else:
                # Clip leaf children are content values (EMPTY=-1,SOLID=-2).
                inside = (hull - 1) * 6 + side + 1 if side < 5 else -2
                children = (-1, inside) if upper else (inside, -1)
                clips.append(struct.pack("<i2h", plane, *children))
    lumps[0] = b'{"classname" "worldspawn"}\0'
    lumps[1] = b"".join(planes)
    lumps[2] = struct.pack("<i", 0)
    lumps[5] = b"".join(nodes)
    lumps[9] = b"".join(clips)
    lumps[10] = b"".join(struct.pack("<2i6h2H4B", contents, -1, *lo, *hi, 0, 0, 0, 0, 0, 0)
                          for contents in (-2, -1))
    lumps[14] = struct.pack("<9f7i", *lo, *hi, 0, 0, 0, 0, 0, 6, 0, 1, 0, 0)
    cursor = 124
    header = [29]
    for lump in lumps:
        header.extend((cursor, len(lump))); cursor += len(lump)
    return struct.pack("<31i", *header) + b"".join(lumps)


def generate(root, brushes=False):
    p = Program(root)
    for name in ("main", "StartFrame", "PlayerPreThink", "PlayerPostThink", "ClientKill", "ClientConnect",
                 "PutClientInServer", "ClientDisconnect", "SetNewParms", "SetChangeParms",
                 "info_player_start", "info_player_deathmatch"):
        p.function(name)
    for name, number in (("setorigin", 2), ("setmodel", 3), ("setsize", 4), ("error", 10), ("spawn", 14),
                         ("precache_model", 20), ("dprint", 25)):
        p.function(name, -number)
    g = p.global_offsets
    actor = p.global_value("fixture_platform", 4)
    stair = p.global_value("fixture_stair", 4)
    trigger = p.global_value("fixture_trigger", 4)
    player = p.global_value("fixture_player", 4)
    pointer = p.global_value("fixture_pointer", 7)
    scratch = p.vector("fixture_scratch")
    position = p.vector("fixture_position")
    relative = p.vector("fixture_relative")
    created = p.global_value("fixture_created", 2)
    touched = p.global_value("fixture_solid_touch", 2)
    triggered = p.global_value("fixture_trigger_touch", 2)
    checked = p.global_value("fixture_checked", 2)
    landed = p.global_value("fixture_landed", 2)
    landing_relative = p.vector("fixture_landing_relative")

    def scalar(name, value):
        return p.global_value("fixture_" + name, 2, struct.unpack("<I", struct.pack("<f", value))[0])

    zero, one = scalar("zero", 0), scalar("one", 1)
    walk, push, bbox, slidebox = scalar("walk", 3), scalar("push", 7), scalar("bbox", 2), scalar("slidebox", 3)
    clientflag, groundflag = scalar("clientflag", 8), scalar("groundflag", 512)
    health, future, wait = scalar("health", 100), scalar("future", 10000), scalar("wait", 6 if brushes else 3)
    minheight, maxheight = scalar("minheight", 39.8 if brushes else 23.8), scalar("maxheight", 40.2 if brushes else 24.2)
    minside, maxside = scalar("minside", -.2), scalar("maxside", .2)
    moved = scalar("moved", 148)
    playerlo, playerhi = p.vector("fixture_playerlo", (-16, -16, -24)), p.vector("fixture_playerhi", (16, 16, 32))
    platformlo, platformhi = p.vector("fixture_platformlo", (-192, -192, -32)), p.vector("fixture_platformhi", (192, 192, 0))
    triggerlo, triggerhi = p.vector("fixture_triggerlo", (-512, -512, -256)), p.vector("fixture_triggerhi", (512, 512, 512))
    origin, spawn = p.vector("fixture_origin", (128, 128, 128)), p.vector("fixture_spawn", (0, 0, 80))
    velocity = p.vector("fixture_velocity", (64, 0, 16))
    bsp = scalar("bsp", 4)
    forward = scalar("forward", 160)
    walk_min, walk_max = scalar("walk_min", 700), scalar("walk_max", 1000)
    floor_model, stair_model = p.text("fixture_floor_model", "maps/floor.bsp"), p.text("fixture_stair_model", "maps/stair.bsp")
    stair_origin = p.vector("fixture_stair_origin", (512, 128, 128))
    failure = p.text("fixture_failure", "Jolt entity fixture assertion failed")
    success = p.text("fixture_success", PASS + (": BSP floor and 16-unit stair" if brushes else ": moving pusher support") +
                     ", solid touch, trigger touch\n")

    def store(entity, name, value, kind="F"):
        p.op("ADDRESS", entity, g[name], pointer)
        p.op("STOREP_" + kind, value, pointer)

    def patch(jump):
        op, a, _, c = p.statements[jump]
        p.statements[jump] = (op, a, len(p.statements) - jump, c)

    for name, count in (("fixture_solid_touch_fn", touched), ("fixture_trigger_touch_fn", triggered)):
        p.function(name, len(p.statements))
        p.op("EQ_E", g["other"], player, scratch)
        skip = p.op("IFNOT", scratch)
        p.op("ADD_F", count, one, count)
        patch(skip)
        p.op("DONE")

    p.function("worldspawn", len(p.statements))
    if brushes:
        p.call("precache_model", (floor_model, False))
        p.call("precache_model", (stair_model, False))
    p.call("spawn"); p.op("STORE_ENT", 1, actor)
    store(actor, "movetype", push); store(actor, "solid", bsp if brushes else bbox)
    store(actor, "touch", g["fixture_solid_touch_fn"], "FNC")
    store(actor, "nextthink", future)
    if brushes:
        p.call("setmodel", (actor, False), (floor_model, False))
    else:
        store(actor, "velocity", velocity, "V")
        p.call("setsize", (actor, False), (platformlo, True), (platformhi, True))
    p.call("setorigin", (actor, False), (origin, True))
    if brushes:
        p.call("spawn"); p.op("STORE_ENT", 1, stair)
        store(stair, "movetype", push); store(stair, "solid", bsp)
        store(stair, "touch", g["fixture_solid_touch_fn"], "FNC")
        p.call("setmodel", (stair, False), (stair_model, False))
        p.call("setorigin", (stair, False), (stair_origin, True))
    p.call("spawn"); p.op("STORE_ENT", 1, trigger)
    store(trigger, "solid", one)
    store(trigger, "touch", g["fixture_trigger_touch_fn"], "FNC")
    p.call("setsize", (trigger, False), (triggerlo, True), (triggerhi, True))
    p.call("setorigin", (trigger, False), (origin, True))
    p.op("DONE")

    p.function("PutClientInServer", len(p.statements))
    p.op("STORE_ENT", g["self"], player)
    store(player, "movetype", walk); store(player, "solid", slidebox)
    store(player, "flags", clientflag); store(player, "health", health)
    p.call("setsize", (player, False), (playerlo, True), (playerhi, True))
    p.op("LOAD_V", actor, g["origin"], position)
    p.op("ADD_V", position, spawn, position)
    p.call("setorigin", (player, False), (position, True))
    p.op("STORE_F", g["time"], created)
    p.op("DONE")

    if brushes:
        p.function("PlayerPreThink", len(p.statements))
        unspawned = p.op("IFNOT", player)
        p.op("SUB_F", g["time"], created, scratch)
        p.op("GE", scratch, one, scratch)
        early = p.op("IFNOT", scratch)
        p.op("LOAD_V", player, g["velocity"], position)
        p.op("STORE_F", forward, position)
        store(player, "velocity", position, "V")
        patch(early); patch(unspawned); p.op("DONE")

    p.function("PlayerPostThink", len(p.statements))
    unspawned = p.op("IFNOT", player)
    done = p.op("IF", checked)
    p.op("LOAD_V", player, g["origin"], position)
    p.op("LOAD_V", actor, g["origin"], relative)
    p.op("SUB_V", position, relative, relative)
    p.op("LOAD_F", player, g["flags"], scratch)
    p.op("BITAND", scratch, groundflag, scratch)
    airborne = p.op("IFNOT", scratch)
    recorded = p.op("IF", landed)
    p.op("STORE_V", relative, landing_relative)
    p.op("STORE_F", one, landed)
    patch(recorded); patch(airborne)
    p.op("SUB_F", g["time"], created, scratch)
    p.op("GE", scratch, wait, scratch)
    early = p.op("IFNOT", scratch)
    p.op("LOAD_F", player, g["flags"], scratch)
    p.op("BITAND", scratch, groundflag, scratch); p.assert_true(scratch, failure)
    p.op("LOAD_ENT", player, g["groundentity"], scratch)
    p.op("EQ_E", scratch, stair if brushes else actor, scratch); p.assert_true(scratch, failure)
    p.op("LOAD_V", player, g["origin"], position)
    p.op("LOAD_V", actor, g["origin"], relative)
    if not brushes:
        p.op("GE", relative + 2, moved, scratch); p.assert_true(scratch, failure)
    p.op("SUB_V", position, relative, relative)
    p.op("GE", relative + 2, minheight, scratch); p.assert_true(scratch, failure)
    p.op("LE", relative + 2, maxheight, scratch); p.assert_true(scratch, failure)
    # The platform may move while the player falls. Its relative horizontal
    # offset must remain stable after contact, with no doubled Jolt carry.
    if brushes:
        p.op("GE", position, walk_min, scratch); p.assert_true(scratch, failure)
        p.op("LE", position, walk_max, scratch); p.assert_true(scratch, failure)
    else:
        p.op("SUB_V", relative, landing_relative, relative)
        for axis in (0, 1):
            p.op("GE", relative + axis, minside, scratch); p.assert_true(scratch, failure)
            p.op("LE", relative + axis, maxside, scratch); p.assert_true(scratch, failure)
    p.assert_true(touched, failure); p.assert_true(triggered, failure)
    p.call("dprint", (success, False)); p.op("STORE_F", one, checked)
    patch(early); patch(done); patch(unspawned)
    p.op("DONE")
    return p.encode()


def run(binary, work, timeout):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
    master, slave = pty.openpty()
    os.set_blocking(master, False)
    process = subprocess.Popen([str(binary), "-basedir", str(work), "-userdir", str(work), "-game", "jolt_fixture",
                                "-dedicated", "1", "-port", str(port), "+developer", "1",
                                "+world_create", "entity_motion", "empty", "1"],
                               stdin=subprocess.PIPE, stdout=slave, stderr=slave, cwd=work)
    os.close(slave)
    selector = selectors.DefaultSelector()
    selector.register(master, selectors.EVENT_READ, "console")
    client = Client(0, ("127.0.0.1", port), selector)
    output = bytearray()
    started = time.monotonic()
    failure = None
    try:
        while PASS.encode() not in output:
            if process.poll() is not None or time.monotonic() - started > timeout:
                raise RuntimeError("server stopped or timed out before entity assertions")
            client.pump(time.monotonic(), started, False, False)
            for key, _ in selector.select(.01):
                if key.data == "console":
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        pass
                else:
                    while True:
                        try:
                            packet, address = client.socket.recvfrom(65536)
                        except BlockingIOError:
                            break
                        client.receive(packet, address, time.monotonic(), started)
        process.stdin.write(b"world_stats;quit\n"); process.stdin.flush()
        process.wait(timeout=10)
    except Exception as error:
        failure = str(error)
    finally:
        if process.poll() is None:
            process.kill(); process.wait()
        while True:
            try:
                data = os.read(master, 65536)
                if not data:
                    break
                output.extend(data)
            except (OSError, BlockingIOError):
                break
        client.socket.close(); selector.close(); os.close(master)
        (work / "server.log").write_bytes(output)
    if failure or process.returncode or b"assertion failed" in output or b"World physics: Jolt" not in output:
        raise RuntimeError((failure or "fixture failed") + "\n" + output.decode(errors="replace")[-6000:])
    return {"frames": client.frames, "log": str(work / "server.log"), "success": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--brushes", action="store_true", help="test exact BSP floor and 16-unit stair entities inside a streamed world")
    args = parser.parse_args()
    os.environ.update(copied_engine_environment(args.binary))
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-jolt-entities-"))
    (work / "id1").mkdir(); (work / "jolt_fixture").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    (work / "jolt_fixture/progs.dat").write_bytes(generate(ROOT, args.brushes))
    if args.brushes:
        (work / "jolt_fixture/maps").mkdir()
        (work / "jolt_fixture/maps/floor.bsp").write_bytes(brush_model((-512, -256, -32), (2048, 256, 0)))
        (work / "jolt_fixture/maps/stair.bsp").write_bytes(brush_model((0, -128, 0), (512, 128, 16)))
    binary = work / "quiver-server"
    shutil.copy2(args.binary.resolve(), binary)
    summary = {"work": str(work), "brushes": args.brushes, "success": False}
    try:
        summary.update(run(binary, work, args.timeout))
    finally:
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
