#!/usr/bin/env python3
"""Exercise player collision against smooth terrain in an isolated graphical game.

Uses ordinary walking and jumping after positioning each fixture. Authoritative
edict diagnostics verify movement mode, grounded state, velocity and hull bounds.
An independent continuous-height calculation checks untouched terrain support;
the C++ collision tests provide the exact marching-tetrahedra mesh oracle. Edits,
checkpoint reloads and BSP transitions run against temporary copies of the game
configuration and world database. Only pak assets are linked from --basedir.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from run_terrain_lod_smoke import Run, WRAPPER
from run_terrain_smoothing_smoke import camera, edit

SEED = 4242
MASK64 = (1 << 64) - 1
FIELDS = ("origin", "mins", "maxs", "velocity", "flags", "movetype", "health")


def terrain_height(x, y):
    """Independent scalar generator oracle; this does not call engine tracing."""
    def lattice(ix, iy):
        value = ((ix & MASK64) * 0x9e3779b97f4a7c15 ^
                 (iy & MASK64) * 0xbf58476d1ce4e5b9 ^ SEED) & MASK64
        value = ((value ^ (value >> 30)) * 0x94d049bb133111eb) & MASK64
        return 8 + (value ^ (value >> 31)) % 9

    gx, gy = x / 256, y / 256
    bx, by = math.floor(gx), math.floor(gy)
    fx, fy = gx - bx, gy - by
    low = lattice(bx, by) * (1 - fx) + lattice(bx + 1, by) * fx
    high = lattice(bx, by + 1) * (1 - fx) + lattice(bx + 1, by + 1) * fx
    height = low * (1 - fy) + high * fy
    if -1024 <= x < 1024 and -1024 <= y < 1024:
        weight = max(0, min(1, (max(abs(x - 128), abs(y - 128)) - 256) / 256))
        height = 8 + (height - 8) * weight
    return height * 8


def support_height(state):
    # Maximum height under a flat-bottomed player hull, not just the center ray.
    # A two-unit grid includes the hull edges and resolves lattice/flattening
    # boundaries; tetrahedral interpolation differs slightly from this analytic
    # generator and is checked exactly by the separate C++ tests.
    x, y = state["position"][:2]
    lo, hi = state["mins"], state["maxs"]
    xs = [x + lo[0] + (hi[0] - lo[0]) * i / 16 for i in range(17)]
    ys = [y + lo[1] + (hi[1] - lo[1]) * i / 16 for i in range(17)]
    return max(terrain_height(px, py) for px in xs for py in ys)


def player_state(run, terrain=True):
    commands = ["edict 1 " + field for field in FIELDS]
    if terrain:
        commands.append("world_stats")
    output = run.command("\n".join(commands))
    result = {}
    for field in FIELDS:
        match = re.search(r"Edict 1\." + field + r"==([^\r\n]+)", output)
        if not match:
            raise RuntimeError("missing authoritative player field " + field + ": " + output)
        values = [float(value) for value in match[1].strip(" '\"").split()]
        if not values or not all(math.isfinite(value) for value in values):
            raise RuntimeError("invalid player field " + field)
        result[field] = values if field in ("origin", "mins", "maxs", "velocity") else values[0]
    if terrain:
        match = re.search(r"World position: chunk (-?\d+) (-?\d+) (-?\d+) local ([\d.-]+) ([\d.-]+) ([\d.-]+)", output)
        if not match:
            raise RuntimeError("missing canonical world position: " + output)
        result["render_chunk"] = list(map(int, match.groups()[:3]))
        render = [chunk * 1024 + float(local) for chunk, local in zip(result["render_chunk"], match.groups()[3:])]
        # The edict origin is relative to the current floating frame. The view
        # can lag by a frame, but differs by far less than half a chunk, so it
        # identifies that frame without using interpolated eye height as feet.
        result["position"] = [origin + round((view - origin) / 1024) * 1024
                              for origin, view in zip(result["origin"], render)]
    else:
        result["position"] = result["origin"]
    result["feet_z"] = result["position"][2] + result["mins"][2]
    result["grounded"] = bool(int(result["flags"]) & 512)
    result["seconds"] = time.monotonic() - run.started
    if result["health"] <= 0:
        raise RuntimeError("player died during collision test")
    return result


def grounded(run, untouched=False):
    deadline = min(run.deadline, time.monotonic() + 12)
    samples = []
    while time.monotonic() < deadline:
        state = player_state(run)
        samples.append(state)
        if state["movetype"] != 3:
            raise RuntimeError("fixture is not walking: " + json.dumps(state))
        if len(samples) >= 4 and all(item["grounded"] and abs(item["velocity"][2]) < .1 for item in samples[-4:]):
            recent = samples[-4:]
            if max(item["feet_z"] for item in recent) - min(item["feet_z"] for item in recent) > .08:
                raise RuntimeError("stationary player is jittering vertically: " + json.dumps(recent))
            if untouched:
                state["expected_support_z"] = support_height(state)
                state["support_error"] = state["feet_z"] - state["expected_support_z"]
                if abs(state["support_error"]) > .75:
                    raise RuntimeError("player hull does not match continuous terrain: " + json.dumps(state))
            return {"final": state, "samples": samples}
        run.delay(.1)
    raise RuntimeError("player did not settle on terrain: " + json.dumps(samples[-4:]))


def drop(run, x, y, z=256, yaw=0):
    camera(run, f"{x} {y} {z} 10 {yaw} 0")
    run.command("noclip 0\ngod 1")
    run.delay(.1)
    return grounded(run, untouched=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--stage-timeout", type=float, default=40)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    args = parser.parse_args()
    if not (0 < args.timeout <= 600 and 0 < args.stage_timeout <= 60):
        parser.error("timeouts must be positive and bounded (600/60 seconds maximum)")
    if not (320 <= args.width <= 3840 and 240 <= args.height <= 2160):
        parser.error("resolution must be between 320x240 and 3840x2160")
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        parser.error("a graphical session is required")
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-terrain-collision-"))
    (work / "id1").mkdir()
    (work / "user").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    shutil.copy2(args.binary.resolve(), work / "vkquake")
    (work / "autokey.c").write_text(WRAPPER)
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", str(work / "autokey.c"),
                    "-o", str(work / "autokey.so"), "-ldl"], check=True, timeout=30)
    (work / "id1/startup.cfg").write_text("""vid_fullscreen 0
vid_vsync 0
vid_fsaa 0
host_maxfps 72
r_tasks 1
r_fastsky 1
r_clouds 0
r_drawviewmodel 0
viewsize 130
cl_nocsqc 1
cl_bob 0
cl_rollangle 0
cl_forwardspeed 320
developer 1
scr_conspeed 100000
con_notifytime -1
world_view_chunks 2
r_octree_distance 4096
r_octree_mesh_mb 128
r_octree_upload_mb 8
r_octree_lod 1
r_octree_smooth 1
r_octree_lod_distance 32768
bind F12 "exec smoke_command.cfg"
world_create collision_smoke terrain 4242
""")
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "work": str(work), "binary": str(args.binary.resolve()), "checks": {},
               "method": "authoritative player hull and grounded diagnostics; independent untouched terrain support oracle",
               "visual_review_required": True, "frame_cap": 72}
    run = None
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "initial terrain signon")
        # The first two fixtures are deliberately about four units away from a
        # voxel-height multiple, so legacy eight-unit collision cannot pass.
        for x, y in ((840, 700), (1120, 700), (1420, 700)):
            result = drop(run, x, y)
            result["screenshot"] = run.photograph(f"grounded_{x}_{y}")
            summary["checks"][f"drop_{x}_{y}"] = result
        start = drop(run, 840, 700)["final"]
        run.command("+forward")
        walking = []
        for _ in range(13):
            run.delay(.15)
            state = player_state(run)
            if state["movetype"] != 3:
                raise RuntimeError("walking changed movement mode")
            if state["grounded"]:
                state["support_error"] = state["feet_z"] - support_height(state)
                if abs(state["support_error"]) > 1:
                    raise RuntimeError("walking hull separates from the terrain: " + json.dumps(state))
            walking.append(state)
        run.command("-forward")
        end = grounded(run, untouched=True)
        if end["final"]["position"][0] - start["position"][0] < 330:
            raise RuntimeError("player was blocked while walking across a chunk boundary")
        if not any(item["render_chunk"][0] >= 1 for item in walking):
            raise RuntimeError("walking did not cross the chunk seam")
        if sum(item["grounded"] for item in walking) < len(walking) // 2:
            raise RuntimeError("player repeatedly lost grounded contact on gentle slopes")
        summary["checks"]["walking_across_seam"] = {"start": start, "samples": walking, "settled": end,
                                                         "screenshot": run.photograph("walked_across_seam")}
        baseline = end["final"]["feet_z"]
        run.command("+jump")
        jumping = []
        for _ in range(3):
            run.delay(.1)
            jumping.append(player_state(run))
        run.command("-jump")
        landed = grounded(run, untouched=True)
        if max(item["feet_z"] for item in jumping) < baseline + 16 or all(item["grounded"] for item in jumping):
            raise RuntimeError("jump failed to leave the surface")
        summary["checks"]["jump_and_land"] = {"samples": jumping, "landed": landed}
        before = drop(run, 128, 128)["final"]
        # Excavate an area larger than the 32-unit player hull, then drop the
        # unchanged hull back at its original standing position. A center-only
        # ray would miss collision at the hull's corners and is insufficient.
        for y in range(108, 149, 8):
            for x in range(108, 149, 8):
                camera(run, f"{x} {y} 160 90 0 0")
                for _ in range(4):
                    edit(run, "world_remove", "Voxel removed")
        camera(run, "128 128 88 30 45 0")
        run.command("noclip 0")
        excavated = grounded(run)
        if excavated["final"]["feet_z"] > before["feet_z"] - 16:
            raise RuntimeError("player did not fall into the excavated terrain")
        # The unchanged solid sample at z=28 has density +8; the removed
        # sample at z=36 has density -4. Their zero crossing is 33 1/3, not
        # the old voxel floor at 32.
        edited_floor = 28 + 8 * 8 / (8 + 4)
        if abs(excavated["final"]["feet_z"] - edited_floor) > .75:
            raise RuntimeError("excavation floor contact is incorrect: " + json.dumps(excavated["final"]))
        summary["checks"]["excavated_floor"] = {"before": before, "after": excavated, "removed_voxels": 144,
                                                     "expected_support_z": edited_floor,
                                                     "screenshot": run.photograph("excavated_floor")}
        # Start on the intact rim, then walk into the hole with real input.
        # This verifies losing support while FL_ONGROUND was previously set,
        # which a teleport-and-drop fixture cannot exercise.
        rim = drop(run, 80, 128, z=88)["final"]
        run.command("cl_forwardspeed 48\n+forward")
        ledge_samples = []
        deadline = min(run.deadline, time.monotonic() + 5)
        while time.monotonic() < deadline:
            run.delay(.05)
            state = player_state(run)
            ledge_samples.append(state)
            if state["position"][0] >= 122:
                break
        run.command("-forward\ncl_forwardspeed 320")
        fallen = grounded(run)
        if fallen["final"]["feet_z"] > rim["feet_z"] - 16 or abs(fallen["final"]["feet_z"] - edited_floor) > .75:
            raise RuntimeError("walking off edited support did not reach the pit floor: " + json.dumps(fallen["final"]))
        summary["checks"]["walk_off_edited_support"] = {"rim": rim, "samples": ledge_samples, "landed": fallen}
        output = run.command("world_save")
        if "World saved" not in output:
            raise RuntimeError("world checkpoint save failed: " + output)
        run.map_command("map e1m1")
        run.delay(1)
        bsp = player_state(run, terrain=False)
        if bsp["movetype"] != 3:
            raise RuntimeError("BSP transition did not restore ordinary walking")
        summary["checks"]["bsp_transition"] = {"player": bsp, "screenshot": run.photograph("bsp_transition")}
        run.map_command("world_load collision_smoke")
        restored = grounded(run)
        if max(abs(a - b) for a, b in zip(restored["final"]["position"], fallen["final"]["position"])) > .1:
            raise RuntimeError("checkpoint changed the player contact position")
        summary["checks"]["checkpoint_restore"] = {"player": restored,
                                                       "screenshot": run.photograph("checkpoint_restored_floor")}
        run.command("echo COLLISION_SMOKE_COMPLETE\nbind F12 quit")
        os.write(run.write_fd, b"k")
        run.process.wait(timeout=10)
        if run.process.returncode:
            raise RuntimeError(f"client exited with status {run.process.returncode}")
        summary["success"] = True
    except Exception as error:
        summary["error"] = str(error)
    finally:
        if run is not None:
            run.close()
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2), flush=True)
    if not summary["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
