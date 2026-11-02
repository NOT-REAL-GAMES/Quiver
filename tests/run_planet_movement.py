#!/usr/bin/env python3
"""Exercise actual player walking across streamed planet chunks at several heights.

Uses isolated configs/worlds and ordinary +forward/+jump input. Records
application frame intervals (including scheduling/presentation, not GPU time)
and authoritative player diagnostics. --require-jolt checks the active backend
and verifies collision extraction and mesh cooking remain off the main thread;
--controller-checks also exercises construction, ceiling contacts, terrain edits
and a BSP transition. Only pak assets are linked.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import statistics
import subprocess
import tempfile
import time

from run_terrain_lod_smoke import Run, WRAPPER
from run_terrain_collision_smoke import player_state
from run_terrain_smoothing_smoke import camera


def physics_snapshot(run, label):
    if not run.args.require_jolt:
        return None
    output = run.command("world_stats")
    extraction = re.search(
        r"Collision workers: (\d+) pending \(([\d.]+) MiB\), (\d+) worker builds, (\d+) main-thread builds, (\d+) stale results", output)
    cooking = re.search(
        r"World physics workers: (\d+) pending \(([\d.]+) MiB\), (\d+) worker cooks, (\d+) main-thread cooks, (\d+) deferred updates", output)
    if not extraction or not cooking:
        raise RuntimeError("missing asynchronous collision instrumentation: " + output)
    result = {"label": label,
              "extraction_pending": int(extraction[1]), "extraction_pending_mib": float(extraction[2]),
              "collision_worker_builds": int(extraction[3]), "collision_sync_builds": int(extraction[4]),
              "collision_stale_results": int(extraction[5]),
              "cook_pending": int(cooking[1]), "cook_pending_mib": float(cooking[2]),
              "worker_cooks": int(cooking[3]), "main_thread_cooks": int(cooking[4]),
              "deferred_updates": int(cooking[5])}
    run.physics_samples.append(result)
    if result["collision_sync_builds"] or result["main_thread_cooks"]:
        raise RuntimeError("live collision performed main-thread extraction or cooking: " + json.dumps(result))
    if result["extraction_pending"] > 32 or result["extraction_pending_mib"] > 256:
        raise RuntimeError("collision extraction queue exceeded its bounded budget: " + json.dumps(result))
    return result


def settled(run, expected_floor=None):
    samples = []
    deadline = min(run.deadline, time.monotonic() + 12)
    while time.monotonic() < deadline:
        state = player_state(run)
        samples.append(state)
        if state["movetype"] != 3:
            raise RuntimeError("fixture is not walking: " + json.dumps(state))
        if len(samples) >= 3 and all(s["grounded"] and abs(s["velocity"][2]) < .1 for s in samples[-3:]):
            if expected_floor is not None and abs(state["feet_z"] - expected_floor) > .75:
                raise RuntimeError("player floor mismatch: " + json.dumps(state))
            return state
        run.delay(.1)
    raise RuntimeError("player did not land: " + json.dumps(samples[-3:]))


def frame_summary(run, sequence):
    values = [int(row.split(",")[1]) / 1e6 for row in (run.work / "frame_intervals.csv").read_text().splitlines()
              if row.startswith(str(sequence) + ",")]
    if len(values) < 20 or not all(math.isfinite(v) and v > 0 for v in values):
        raise RuntimeError("insufficient movement timing samples")
    ordered = sorted(values)
    return {"frames": len(values), "mean_ms": statistics.mean(values), "median_ms": statistics.median(values),
            "p95_ms": ordered[math.ceil(len(values) * .95) - 1], "p99_ms": ordered[math.ceil(len(values) * .99) - 1],
            "max_ms": max(values), "frames_over_100ms": sum(v > 100 for v in values), "sample_id": sequence}


def walk(run, seconds, minimum_distance, floor=None):
    run.last_movement = None
    start = settled(run, floor)
    physics_before = physics_snapshot(run, f"before_walk_{run.measurements + 1}")
    run.measurements += 1
    os.write(run.write_fd, b"b")
    run.command("+forward")
    samples, latencies = [], []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        run.delay(.25)
        begun = time.monotonic()
        state = player_state(run)
        latencies.append(time.monotonic() - begun)
        samples.append(state)
        if floor is not None and state["grounded"] and abs(state["feet_z"] - floor) > .75:
            run.last_movement = {"start": start, "end": state, "samples": samples,
                                 "distance": state["position"][0] - start["position"][0],
                                 "incomplete": True, "expected_floor": floor}
            raise RuntimeError("walking separated from the known platform: " + json.dumps(state))
    run.command("-forward")
    os.write(run.write_fd, b"e")
    run.delay(.15)
    end = settled(run, floor)
    result = {"start": start, "end": end, "samples": samples, "host_frames": frame_summary(run, run.measurements),
              "max_console_latency_seconds": max(latencies), "distance": end["position"][0] - start["position"][0]}
    run.last_movement = result
    result["physics_before"] = physics_before
    result["physics"] = physics_snapshot(run, f"walk_{run.measurements}")
    if physics_before:
        result["physics_delta"] = {key: result["physics"][key] - physics_before[key] for key in
                                   ("collision_worker_builds", "collision_sync_builds", "worker_cooks",
                                    "main_thread_cooks", "deferred_updates")}
    if result["distance"] < minimum_distance:
        raise RuntimeError("walking stalled: " + json.dumps(result))
    if sum(s["grounded"] for s in samples) < len(samples) // 2:
        raise RuntimeError("walking repeatedly lost support: " + json.dumps(result))
    if result["host_frames"]["max_ms"] > run.args.max_frame_ms:
        raise RuntimeError("walking exceeded the requested frame-time bound: " + json.dumps(result))
    return result


def jump(run, floor):
    start = settled(run, floor)
    run.command("+jump")
    samples = []
    for _ in range(3):
        run.delay(.1)
        samples.append(player_state(run))
    run.command("-jump")
    end = settled(run, floor)
    if max(s["feet_z"] for s in samples) - start["feet_z"] < 16 or all(s["grounded"] for s in samples):
        raise RuntimeError("jump did not leave the support surface")
    return {"start": start, "samples": samples, "landed": end, "physics": physics_snapshot(run, "jump")}


def platform(run, floor):
    # Brushes are centered on the render eye; with cl_bob=0 its view offset is
    # 22 units. Read back the actual center so the independent floor oracle
    # remains correct if the gamecode changes that offset.
    camera(run, f"1536 128 {floor - 54} 0 0 0")
    output = run.command("world_stats")
    match = re.search(r"World position: chunk (-?\d+) (-?\d+) (-?\d+) local ([\d.-]+) ([\d.-]+) ([\d.-]+)", output)
    if not match:
        raise RuntimeError("missing platform center")
    actual_floor = int(match[3]) * 1024 + float(match[6]) + 32
    output = run.command("world_brush add box 4096 384 64 0 0 0")
    if "Solid edit committed." not in output:
        raise RuntimeError("platform edit failed: " + output)
    camera(run, f"128 128 {actual_floor + 100} 0 0 0")
    run.command("noclip 0\ngod 1")
    settled(run, actual_floor)
    return actual_floor


def brush(run, center, size, operation="add"):
    # Brushes address the eye, which may be in a different Z chunk from the
    # player's origin. Validate the reconstructed eye rather than equating them.
    origin = (center[0], center[1], center[2] - 22)
    chunks = [math.floor(value / 1024) for value in origin]
    local = [value - chunk * 1024 for value, chunk in zip(origin, chunks)]
    run.command("world_teleport " + " ".join(map(str, chunks + local)) + "\nwait\nsetpos " +
                " ".join(map(str, local + [0, 0, 0])))
    run.delay(.15)
    output = run.command("world_stats")
    match = re.search(r"World position: chunk (-?\d+) (-?\d+) (-?\d+) local ([\d.-]+) ([\d.-]+) ([\d.-]+)", output)
    if not match:
        raise RuntimeError("missing brush eye position: " + output)
    eye = [int(chunk) * 1024 + float(local) for chunk, local in zip(match.groups()[:3], match.groups()[3:])]
    if any(abs(actual - expected) > .1 for actual, expected in zip(eye, center)):
        raise RuntimeError("brush eye did not reach fixture center: " + output)
    output = run.command("world_brush " + operation + " box " + " ".join(map(str, size)) + " 0 0 0")
    if "Solid edit committed." not in output:
        raise RuntimeError("controller fixture edit failed: " + output)


def controller_checks(run):
    floor, y = 133120, 2048
    results = {}
    brush(run, (1536, y, floor - 32), (4096, 512, 64))
    brush(run, (768, y, floor + 8), (512, 256, 16))
    camera(run, f"384 {y} {floor + 100} 0 0 0")
    run.command("noclip 0")
    result = walk(run, 1.2, 280)
    if abs(result["end"]["feet_z"] - (floor + 16)) > .75:
        raise RuntimeError("player did not climb a 16-unit stair: " + json.dumps(result))
    results["sixteen_unit_stair"] = result

    brush(run, (1536, y, floor + 96), (32, 256, 192))
    camera(run, f"1280 {y} {floor + 100} 0 0 0")
    run.command("noclip 0")
    result = walk(run, 1.3, 150, floor)
    if result["end"]["position"][0] > 1504.75:
        raise RuntimeError("player passed through the construction wall: " + json.dumps(result))
    results["wall_block"] = result

    brush(run, (2304, y, floor + 96), (512, 256, 32))
    camera(run, f"2304 {y} {floor + 24} 0 0 0")
    run.command("noclip 0")
    start = settled(run, floor)
    run.command("+jump")
    samples = []
    for _ in range(12):
        run.delay(.04)
        samples.append(player_state(run))
    run.command("-jump")
    rise = max(state["feet_z"] for state in samples) - start["feet_z"]
    if not 8 < rise < 25:
        raise RuntimeError("jump did not respect the low ceiling: " + json.dumps(samples))
    results["ceiling_jump"] = {"start": start, "samples": samples, "landed": settled(run, floor)}

    brush(run, (3072, y, floor - 160), (256, 256, 64))
    camera(run, f"3072 {y} {floor + 100} 0 0 0")
    run.command("noclip 0")
    start = settled(run, floor)
    # Remove support while the player remains standing on it. The 256-high
    # brush is centered at the eye, cutting the upper slab but not the lower.
    output = run.command("world_brush subtract box 128 128 256 0 0 0")
    if "Solid edit committed." not in output:
        raise RuntimeError("support removal failed: " + output)
    results["edited_support_removal"] = {"before": start, "landed": settled(run, floor - 128)}
    physics_snapshot(run, "controller_edits_before_bsp")

    run.map_command("map start")
    run.command("god 1")
    states = []
    for _ in range(12):
        run.delay(.1)
        states.append(player_state(run, terrain=False))
        if states[-1]["grounded"]:
            break
    if states[-1]["movetype"] != 3 or not states[-1]["grounded"]:
        raise RuntimeError("BSP player did not resume ordinary grounded movement")
    results["bsp_transition"] = states[-1]
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--global-cache", type=Path)
    parser.add_argument("--source-world", type=Path, help="back up an existing fixture database to retain its generator version")
    parser.add_argument("--natural-only", action="store_true", help="only walk and jump on the generated terrain")
    parser.add_argument("--world-kind", choices=("planet", "terrain"), default="planet")
    parser.add_argument("--require-jolt", action="store_true", help="require Jolt and zero main-thread collision extraction/cooking")
    parser.add_argument("--controller-checks", action="store_true", help="exercise stairs, walls, ceiling, support edits, and BSP return")
    parser.add_argument("--max-frame-ms", type=float, default=1000, help="upper movement-frame bound; use 100 for stall acceptance")
    parser.add_argument("--timeout", type=float, default=360)
    parser.add_argument("--stage-timeout", type=float, default=60)
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--validation", action="store_true")
    args = parser.parse_args()
    if not 0 < args.timeout <= 900 or not 0 < args.stage_timeout <= 180:
        parser.error("timeout/stage bounds are 900/180 seconds")
    if not math.isfinite(args.max_frame_ms) or args.max_frame_ms <= 0:
        parser.error("max-frame-ms must be finite and positive")
    if args.controller_checks and args.natural_only:
        parser.error("controller-checks and natural-only are mutually exclusive")
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("no pak*.pak assets found")
    work = Path(tempfile.mkdtemp(prefix="quiver-planet-movement-"))
    (work / "id1/worlds").mkdir(parents=True)
    (work / "user").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    if args.global_cache:
        shutil.copy2(args.global_cache, work / "id1/worlds/movement.sqlite3.planetcache")
    if args.source_world:
        with sqlite3.connect(f"file:{args.source_world.resolve()}?mode=ro", uri=True) as source_db:
            with sqlite3.connect(work / "id1/worlds/movement.sqlite3") as destination_db:
                source_db.backup(destination_db)
    shutil.copy2(args.binary.resolve(), work / "vkquake")
    (work / "autokey.c").write_text(WRAPPER)
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", str(work / "autokey.c"), "-o", str(work / "autokey.so"), "-ldl"], check=True, timeout=30)
    startup = '''vid_fullscreen 0
vid_vsync 0
vid_fsaa 0
host_maxfps 144
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
world_view_chunks 1
r_octree_distance 2048
r_octree_mesh_mb 128
r_octree_upload_mb 8
r_octree_lod 1
r_octree_lod_distance 16384
r_octree_smooth 1
bind F12 "exec smoke_command.cfg"
world_create movement planet 1
'''
    startup = startup.replace("world_create movement planet 1", f"world_create movement {args.world_kind} 1")
    if args.source_world:
        startup = startup.replace(f"world_create movement {args.world_kind} 1", "world_load movement")
    (work / "id1/startup.cfg").write_text(startup)
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "work": str(work), "binary": str(args.binary.resolve()), "checks": {},
               "frame_cap": 144, "timings": "application input-batch intervals; not isolated GPU timings",
               "collision_path": "unverified", "world_kind": args.world_kind, "max_frame_ms": args.max_frame_ms}
    run = None
    print(work, flush=True)
    try:
        run = Run(args, work)
        run.physics_samples = []
        run.wait(lambda text: "CL_SignonReply: 4" in text, "planet signon")
        camera(run, "128 128 256 0 0 0")
        run.command("noclip 0\ngod 1")
        stats = run.command("world_stats")
        jolt = re.search(r"World physics: Jolt[^\n]*", stats)
        summary["collision_path"] = jolt[0] if jolt else "legacy terrain collision"
        if args.require_jolt and not jolt:
            raise RuntimeError("world_stats did not confirm Jolt: " + stats)
        settled(run, 128 if args.world_kind == "planet" else 64)
        if args.require_jolt:
            stats = run.command("world_stats")
            active = re.search(r"World physics: Jolt, (\d+) player controllers, (\d+) bodies", stats)
            if not active or not all(int(value) > 0 for value in active.groups()):
                raise RuntimeError("no active Jolt controller and collision bodies after landing: " + stats)
            summary["collision_path"] = re.search(r"World physics: Jolt[^\n]*", stats)[0]
            physics_snapshot(run, "initial_landing")
        summary["checks"]["natural_walk"] = walk(run, 9, 2100)
        summary["checks"]["natural_jump"] = jump(run, None)
        for requested_floor in (() if args.natural_only else (1024, 133120)):
            floor = platform(run, requested_floor)
            label = f"platform_{requested_floor}"
            summary["checks"][label] = walk(run, 9, 2100, floor)
            summary["checks"][label + "_jump"] = jump(run, floor)
        if not args.natural_only:
            # A 1:4 ramp checks uphill movement and the planet tangent-support path.
            floor = 133120
            camera(run, f"512 128 {floor + 42} 0 0 0")
            output = run.command("world_brush add wedge 512 256 128 0 0 0")
            if "Solid edit committed." not in output:
                raise RuntimeError("ramp edit failed: " + output)
            camera(run, f"320 128 {floor + 140} 0 0 0")
            run.command("noclip 0")
            before = settled(run)
            result = walk(run, 1.0, 180)
            if result["end"]["feet_z"] - before["feet_z"] < 40:
                raise RuntimeError("player did not climb the gentle ramp: " + json.dumps(result))
            summary["checks"]["ramp_climb"] = result
        summary["world_stats"] = run.command("world_stats")
        if args.controller_checks:
            summary["checks"].update(controller_checks(run))
        run.command("echo PLANET_MOVEMENT_COMPLETE\nbind F12 quit")
        os.write(run.write_fd, b"k")
        run.process.wait(timeout=10)
        if run.process.returncode:
            raise RuntimeError(f"engine exit {run.process.returncode}")
        summary["success"] = True
    except Exception as error:
        summary["error"] = str(error)
        if run is not None and hasattr(run, "last_movement"):
            summary["failed_movement"] = run.last_movement
    finally:
        if run is not None:
            summary["physics_samples"] = getattr(run, "physics_samples", [])
            run.close()
        if args.validation:
            output = (work / "process.log").read_text(errors="replace")
            failures = re.findall(r"[^\n]*(?:VUID-|Validation Error)[^\n]*", output)
            summary["validation_error_count"] = len(failures)
            # Release builds do not expose native -validation initialization;
            # VK_INSTANCE_LAYERS can enable it through the Vulkan loader. Its
            # insertion log proves activation, unlike listing available layers.
            summary["validation_layer_enabled"] = ("Using VK_LAYER_KHRONOS_validation" in output or
                re.search(r'Insert instance layer[^\n]*VK_LAYER_KHRONOS_validation', output) is not None)
            if failures or not summary["validation_layer_enabled"]:
                summary["success"] = False
                summary.setdefault("error", "validation errors or layer not enabled")
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({key: summary[key] for key in ("success", "work", "error") if key in summary}), flush=True)
    if not summary["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
