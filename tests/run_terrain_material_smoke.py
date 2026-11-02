#!/usr/bin/env python3
"""Compare terrain materials at fixed cameras and exercise renderer lifetimes.

Run --baseline before the change, then pass its summary.json with --reference.
Uses isolated /tmp game data and the normal renderer task path. Screenshots need
visual review for blending, texture projection, LOD seams and aliasing; image
differences alone cannot establish that a material looks natural. Frame interval
samples include normal application scheduling and are not isolated GPU timings.
Requires the graphical session and dependencies of run_terrain_lod_smoke.py.
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

from run_terrain_lighting_smoke import difference, photograph
from run_terrain_lod_smoke import Run, WRAPPER, parse_stats
from run_terrain_smoothing_smoke import camera, edit, geometry, idle_world


VIEWS = (("high", "128 128 768 15 45 0"),
         ("low", "128 128 256 8 45 0"),
         ("close", "956 956 190 45 45 0"),
         ("overhead", "512 512 360 70 20 0"))


def settle_material(run):
    # Looking steeply downward can cull every distant tile while the LOD cache
    # remains active. Require a settled cache and visible nearby terrain, but
    # do not mistake a legitimately empty far draw list for a loading failure.
    deadline = min(run.deadline, time.monotonic() + run.args.stage_timeout)
    previous, stable, last = None, 0, None
    while time.monotonic() < deadline:
        last = parse_stats(run.command("octree_render_stats"))
        if not math.isfinite(last["gpu_mib"]) or last["gpu_mib"] > 128.1:
            raise RuntimeError(f"terrain GPU budget exceeded: {last}")
        if last["cpu_mib"] > 32.01 or last["selected"] > 512 or last["cached"] > 512 or last["pending"] > 8 or last["failed"]:
            raise RuntimeError(f"terrain cache bounds/build failure: {last}")
        ready = (last["selected"] > 0 and last["cached"] >= last["selected"] and not last["pending"] and
                 last["near_triangles"] > 0 and not last["uploaded_kib"] and not last["retired_mib"])
        key = tuple(last[name] for name in ("selected", "cached", "pending", "near_triangles", "far_triangles"))
        stable = stable + 1 if ready and key == previous else 0
        if stable >= 2:
            return last
        previous = key
        run.delay(.35)
    raise RuntimeError(f"terrain materials did not settle: {last}")


def capture(run, label, viewpoint=None, timing=False):
    if viewpoint is not None:
        camera(run, viewpoint)
    stats = settle_material(run)
    result = {"camera": viewpoint, "stats": stats, "world_idle_samples": idle_world(run),
              "screenshot": photograph(run, label)}
    if timing:
        result["timings"] = run.measure(label)
    return result


def no_terrain(run):
    stats = run.settle(False, idle_uploads=True)
    if stats["near_draws"] or stats["near_triangles"]:
        raise RuntimeError(f"old nearby terrain remained visible after map change: {stats}")
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--stage-timeout", type=float, default=45)
    parser.add_argument("--sample-seconds", type=float, default=3)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    args = parser.parse_args()
    if not (0 < args.timeout <= 600 and 0 < args.stage_timeout <= 60 and .25 <= args.sample_seconds <= 15):
        parser.error("timeouts/sample duration must be positive and bounded (600/60/15 seconds maximum)")
    if not (320 <= args.width <= 3840 and 240 <= args.height <= 2160):
        parser.error("resolution must be between 320x240 and 3840x2160")
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        parser.error("a graphical session is required")
    if args.baseline and args.reference:
        parser.error("--baseline and --reference are mutually exclusive")
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak assets")
    reference = json.loads(args.reference.read_text()) if args.reference else None
    if reference and (not reference.get("success") or not reference.get("baseline") or
                      reference.get("resolution") != [args.width, args.height]):
        parser.error("reference must be a successful baseline run at the same resolution")
    work = Path(tempfile.mkdtemp(prefix="quiver-terrain-materials-"))
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
host_maxfps 0
host_speeds 0
r_speeds 0
r_tasks 1
r_parallelmark 1
r_indirect 1
r_gpulightmapupdate 1
r_scale 1
r_fastsky 1
r_clouds 0
r_drawviewmodel 0
viewsize 130
cl_nocsqc 1
developer 1
scr_conspeed 100000
con_notifytime -1
world_view_chunks 2
r_octree_distance 4096
r_octree_mesh_mb 128
r_octree_upload_mb 8
gl_farclip 16384
r_octree_lod 1
r_octree_lod_distance 32768
r_octree_smooth 1
r_sun_azimuth 0
r_sun_angle 35
r_sun_intensity 1
bind F12 "exec smoke_command.cfg"
world_create materials_smoke terrain 4242
""")
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "baseline": args.baseline, "work": str(work), "binary": str(args.binary.resolve()),
               "resolution": [args.width, args.height], "worker_count": "engine default; no override",
               "method": "normal host-frame input-batch intervals; warm stationary camera; clouds disabled",
               "capture_only_fps_cap": 60, "visual_review_required": True,
               "reference": str(args.reference.resolve()) if args.reference else None, "views": {}, "checks": {}}
    run = None
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "initial terrain signon")
        device = re.search(r"^Device: (.+)$", run.text(), re.MULTILINE)
        if device:
            summary["gpu"] = device[1]
        for label, viewpoint in VIEWS:
            result = capture(run, label, viewpoint, timing=True)
            if reference:
                result["baseline_rgb_mean_difference"] = difference(reference["views"][label]["screenshot"], result["screenshot"])
                if result["baseline_rgb_mean_difference"] < .1:
                    raise RuntimeError(f"terrain materials have no visible effect in {label}")
            summary["views"][label] = result
        camera(run, VIEWS[1][1])
        original = run.settle(True, True, idle_uploads=True)
        original_image = photograph(run, "low_repeat")
        run.delay(.5)
        repeated = photograph(run, "low_stationary")
        temporal_delta = difference(original_image, repeated)
        if temporal_delta > .05:
            raise RuntimeError(f"stationary terrain pixels are unstable: {temporal_delta}")
        summary["checks"]["stationary_pixels"] = {"rgb_mean_difference": temporal_delta, "screenshot": repeated}
        output = run.command("r_sun_azimuth 180" + "\nwait\noctree_render_stats" * 8)
        transitions = [parse_stats(part) for part in re.split(r"(?=Octree render:)", output)[1:]]
        if len(transitions) != 8 or any(item["uploaded_kib"] or item["retired_mib"] for item in transitions):
            raise RuntimeError("sun movement uploaded or retired terrain geometry")
        sun_stats = run.settle(True, True, idle_uploads=True)
        if geometry(original) != geometry(sun_stats):
            raise RuntimeError("sun movement changed stable terrain geometry")
        sun_image = photograph(run, "low_sun180")
        sun_delta = difference(original_image, sun_image)
        if sun_delta < .1:
            raise RuntimeError("terrain does not respond to sun movement")
        summary["checks"]["sun_response"] = {"frames_checked": len(transitions), "stats": sun_stats,
                                              "rgb_mean_difference": sun_delta, "screenshot": sun_image}
        run.command("r_sun_azimuth 0")
        if not args.baseline:
            for x, y in ((1020, 1020), (1028, 1020), (1020, 1028), (1028, 1028)):
                camera(run, f"{x} {y} 200 90 0 0")
                for _ in range(5):
                    edit(run, "world_remove", "Voxel removed")
            edit(run, "world_place 3", "Voxel placed")
            summary["checks"]["excavation"] = capture(run, "excavation", VIEWS[2][1])
            for label, coordinates in (("negative_coordinates", "-1 -1"),
                                       ("huge_coordinates", "9007199254740991 -9007199254740991")):
                run.command(f"world_teleport {coordinates} 0 128 128 768")
                summary["checks"][label] = capture(run, label)
            camera(run, VIEWS[0][1])
            run.command("r_octree_distance 2048\nr_octree_lod_distance 65536")
            summary["checks"]["far_lod_transition"] = capture(run, "far_lod_transition")
            run.command("r_octree_distance 4096\nr_octree_lod_distance 32768\nr_octree_smooth 0")
            summary["checks"]["block_materials"] = capture(run, "block_materials", VIEWS[1][1])
            run.command("r_octree_smooth 1")
            run.map_command("world_create materials_empty empty 4242")
            summary["checks"]["empty_generator"] = no_terrain(run)
            run.map_command("map __empty")
            summary["checks"]["empty_map"] = no_terrain(run)
            run.map_command("map e1m1")
            summary["checks"]["bsp"] = {"stats": no_terrain(run),
                                         "screenshot": photograph(run, "bsp_e1m1")}
            run.command("vid_fsaa 4\nvid_restart")
            summary["checks"]["bsp_msaa_restart"] = no_terrain(run)
            run.map_command("world_load materials_smoke")
            summary["checks"]["terrain_msaa_restart"] = capture(run, "terrain_msaa4", VIEWS[2][1])
            run.command("vid_fsaa 0\nvid_restart")
            before_restart = capture(run, "terrain_restart_before")
            run.command("vid_restart")
            after_restart = capture(run, "terrain_restart_after")
            restart_delta = difference(before_restart["screenshot"], after_restart["screenshot"])
            run.command("vid_restart")
            after_second_restart = capture(run, "terrain_restart_after_second")
            second_restart_delta = difference(before_restart["screenshot"], after_second_restart["screenshot"])
            if restart_delta != 0 or second_restart_delta != 0:
                raise RuntimeError(f"video restart changed material pixels: {restart_delta}, {second_restart_delta}")
            summary["checks"]["restart_material_pixels"] = {"rgb_mean_difference": restart_delta,
                                                              "second_rgb_mean_difference": second_restart_delta,
                                                              "before": before_restart, "after": after_restart,
                                                              "after_second": after_second_restart}
            run.command("r_tasks 0")
            summary["checks"]["terrain_sequential_restart"] = capture(run, "terrain_sequential", VIEWS[0][1])
            run.command("r_tasks 1\nr_fastsky 0\nr_atmosphere 1\nr_clouds 1\nr_clouds_speed 0\nr_octree_lod_distance 65536")
            summary["checks"]["atmosphere"] = capture(run, "atmosphere")
        run.command("echo MATERIAL_SMOKE_COMPLETE\nbind F12 quit")
        os.write(run.write_fd, b"k")
        run.process.wait(timeout=10)
        if run.process.returncode != 0:
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
