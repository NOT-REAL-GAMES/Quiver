#!/usr/bin/env python3
"""Check QuakeC jump debouncing and immediate post-landing impulses over UDP.

An original QC fixture uses ONGROUND/JUMPRELEASED and adds the usual jump
impulse to current vertical velocity. Real client packets hold jump across the
first landing, keep it held through the next landing (no auto-jump), then release
and repress during repeated flights. No saved user settings or GPU are used.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import time

sys.dont_write_bytecode = True
import run_jolt_entities_smoke as entities
from run_world_network_smoke import UNRELIABLE

PASS = "Jolt jump fixture PASS"


class JumpClient(entities.Client):
    first_input = None

    def send(self, data):
        if len(data) >= 27 and struct.unpack_from(">I", data)[0] & UNRELIABLE and data[8] == 3:
            now = time.monotonic()
            if self.first_input is None:
                self.first_input = now
            elapsed = now - self.first_input
            # Hold through a landing without a new press, then explicitly
            # release/repress. Later presses arrive before touchdown too.
            pressed = elapsed < 4 or (elapsed >= 4.25 and (elapsed - 4.25) % .5 < .35)
            data = data[:-2] + bytes((2 if pressed else 0, 0))
        super().send(data)


def generate(root):
    p = entities.Program(root)
    for name in ("main", "StartFrame", "PlayerPreThink", "PlayerPostThink", "ClientKill", "ClientConnect",
                 "PutClientInServer", "ClientDisconnect", "SetNewParms", "SetChangeParms",
                 "info_player_start", "info_player_deathmatch"):
        p.function(name)
    for name, number in (("setorigin", 2), ("setsize", 4), ("error", 10), ("spawn", 14), ("dprint", 25)):
        p.function(name, -number)
    g = p.global_offsets
    player = p.global_value("jump_player", 4)
    platform = p.global_value("jump_platform", 4)
    pointer = p.global_value("jump_pointer", 7)
    scratch = p.vector("jump_scratch")
    velocity = p.vector("jump_velocity")
    flags = p.global_value("jump_flags", 2)
    count = p.global_value("jump_count", 2)
    created = p.global_value("jump_created", 2)
    held = p.global_value("jump_held_checked", 2)
    complete = p.global_value("jump_complete", 2)

    def scalar(name, value):
        return p.global_value("jump_" + name, 2, struct.unpack("<I", struct.pack("<f", value))[0])

    zero, one = scalar("zero", 0), scalar("one", 1)
    walk, push, bbox, slidebox = scalar("walk", 3), scalar("push", 7), scalar("bbox", 2), scalar("slidebox", 3)
    client, ground, released = scalar("client", 8), scalar("ground", 512), scalar("released", 4096)
    health, future = scalar("health", 100), scalar("future", 10000)
    impulse, minimum = scalar("impulse", 270), scalar("minimum_impulse", 260)
    hold_time, finish_count = scalar("hold_time", 3), scalar("finish_count", 8)
    playerlo, playerhi = p.vector("jump_playerlo", (-16, -16, -24)), p.vector("jump_playerhi", (16, 16, 32))
    platformlo, platformhi = p.vector("jump_platformlo", (-512, -512, -32)), p.vector("jump_platformhi", (512, 512, 0))
    floor, origin = p.vector("jump_floor", (128, 128, 128)), p.vector("jump_origin", (128, 128, 640))
    failure = p.text("jump_failure", "Jolt jump fixture assertion failed")
    accepted = p.text("jump_accepted", "Jolt jump impulse accepted\n")
    success = p.text("jump_success", PASS + ": immediate landing impulse and held-button debounce\n")

    def store(entity, name, value, kind="F"):
        p.op("ADDRESS", entity, g[name], pointer)
        p.op("STOREP_" + kind, value, pointer)

    def patch(jump):
        op, a, _, c = p.statements[jump]
        p.statements[jump] = (op, a, len(p.statements) - jump, c)

    p.function("worldspawn", len(p.statements))
    p.call("spawn"); p.op("STORE_ENT", 1, platform)
    store(platform, "movetype", push); store(platform, "solid", bbox); store(platform, "nextthink", future)
    p.call("setsize", (platform, False), (platformlo, True), (platformhi, True))
    p.call("setorigin", (platform, False), (floor, True))
    p.op("DONE")

    p.function("PutClientInServer", len(p.statements))
    p.op("STORE_ENT", g["self"], player)
    store(player, "movetype", walk); store(player, "solid", slidebox); store(player, "health", health)
    p.op("BITOR", client, released, flags); store(player, "flags", flags)
    p.call("setsize", (player, False), (playerlo, True), (playerhi, True))
    p.call("setorigin", (player, False), (origin, True))
    p.op("STORE_F", g["time"], created)
    p.op("DONE")

    p.function("PlayerPreThink", len(p.statements))
    unspawned = p.op("IFNOT", player)
    p.op("LOAD_F", player, g["flags"], flags)
    p.op("LOAD_F", player, g["button2"], scratch)
    pressed = p.op("IF", scratch)
    p.op("BITOR", flags, released, flags); store(player, "flags", flags)
    p.op("DONE")
    patch(pressed)
    p.op("BITAND", flags, ground, scratch); airborne = p.op("IFNOT", scratch)
    p.op("BITAND", flags, released, scratch); debounced = p.op("IFNOT", scratch)
    p.op("SUB_F", flags, ground, flags); p.op("SUB_F", flags, released, flags)
    store(player, "flags", flags)
    p.op("LOAD_V", player, g["velocity"], velocity)
    p.op("ADD_F", velocity + 2, impulse, velocity + 2)
    p.op("GE", velocity + 2, minimum, scratch); p.assert_true(scratch, failure)
    store(player, "velocity", velocity, "V")
    p.op("ADD_F", count, one, count); p.call("dprint", (accepted, False))
    patch(debounced); patch(airborne); patch(unspawned); p.op("DONE")

    p.function("PlayerPostThink", len(p.statements))
    unspawned = p.op("IFNOT", player); done = p.op("IF", complete)
    already_held = p.op("IF", held)
    p.op("SUB_F", g["time"], created, scratch)
    p.op("GE", scratch, hold_time, scratch); early = p.op("IFNOT", scratch)
    # Holding jump across its second landing must leave the player grounded.
    p.op("EQ_F", count, one, scratch); p.assert_true(scratch, failure)
    p.op("LOAD_F", player, g["flags"], scratch)
    p.op("BITAND", scratch, ground, scratch); p.assert_true(scratch, failure)
    p.op("STORE_F", one, held)
    patch(early); patch(already_held)
    p.op("GE", count, finish_count, scratch); unfinished = p.op("IFNOT", scratch)
    p.assert_true(held, failure)
    p.op("STORE_F", one, complete); p.call("dprint", (success, False))
    patch(unfinished); patch(done); patch(unspawned); p.op("DONE")
    return p.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    os.environ.update(entities.copied_engine_environment(args.binary))
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-jolt-jump-"))
    (work / "id1").mkdir(); (work / "jolt_fixture").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    (work / "jolt_fixture/progs.dat").write_bytes(generate(entities.ROOT))
    binary = work / "quiver-server"; shutil.copy2(args.binary.resolve(), binary)
    entities.Client, entities.PASS = JumpClient, PASS
    summary = {"work": str(work), "success": False}
    try:
        summary.update(entities.run(binary, work, args.timeout))
        summary["accepted_jumps"] = (work / "server.log").read_text().count("Jolt jump impulse accepted")
    finally:
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
