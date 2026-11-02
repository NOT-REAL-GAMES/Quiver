#!/usr/bin/env python3
"""Capture isolated terrain lighting views and check dynamic sun updates.

Use --baseline before changing the renderer, then pass its summary.json with
--reference when testing the new renderer. Requires the same Linux graphical
session, compiler, SDL2 headers, and game assets as run_terrain_lod_smoke.py,
plus Pillow for pixel comparisons. All generated data stays in a new /tmp
directory. The sky is held flat and clouds are disabled, so image differences
in the terrain region come from terrain lighting. Normal worker counts and
normal host-frame intervals are used; timings are descriptive, not thresholds.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageStat

from run_terrain_lod_smoke import Run, WRAPPER, parse_stats


def difference(first, second):
    with Image.open(first) as a, Image.open(second) as b:
        if a.size != b.size:
            raise ValueError("lighting captures have different dimensions")
        # The lower half contains nearby terrain and avoids the console overlay
        # and sky in both fixed views. Do not write modified screenshots.
        area = (0, a.height // 2, a.width, a.height)
        delta = ImageChops.difference(a.convert("RGB").crop(area), b.convert("RGB").crop(area))
        return sum(ImageStat.Stat(delta).mean) / 3


def photograph(run, label):
    # At uncapped rates an earlier asynchronous frame can clear a screenshot
    # request before the next frame reads it. Allow that frame to finish for
    # capture only; timing samples remain uncapped and workers stay enabled.
    run.command("host_maxfps 60")
    run.delay(.1)
    try:
        return run.photograph(label)
    finally:
        run.command("host_maxfps 0")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--baseline", action="store_true", help="record old renderer without requiring sun response")
    parser.add_argument("--reference", type=Path, help="summary.json from the baseline run")
    parser.add_argument("--compatibility-only", action="store_true", help="check BSP, video restart, MSAA and sequential terrain without repeating lighting/timing")
    parser.add_argument("--timeout", type=float, default=150)
    parser.add_argument("--stage-timeout", type=float, default=30)
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
    if args.baseline and (args.reference or args.compatibility_only):
        parser.error("--baseline cannot be combined with --reference or --compatibility-only")
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak assets")
    reference = json.loads(args.reference.read_text()) if args.reference else None
    if reference and (not reference.get("success") or not reference.get("baseline") or reference.get("resolution") != [args.width, args.height]):
        parser.error("reference must be a successful baseline run at the same resolution")
    work = Path(tempfile.mkdtemp(prefix="quiver-terrain-lighting-"))
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
r_sun_azimuth 0
r_sun_angle 35
r_sun_intensity 1
bind F12 "exec smoke_command.cfg"
world_create lighting_smoke terrain 4242
""")
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "baseline": args.baseline, "work": str(work), "binary": str(args.binary.resolve()),
               "resolution": [args.width, args.height], "worker_count": "engine default; no override",
               "method": "normal host-frame input-batch intervals; warm stationary camera; clouds disabled",
               "capture_only_fps_cap": 60, "compatibility_only": args.compatibility_only,
               "reference": str(args.reference.resolve()) if args.reference else None, "views": {}, "checks": {}}
    run = None
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "initial terrain signon")
        device = re.search(r"^Device: (.+)$", run.text(), re.MULTILINE)
        if device:
            summary["gpu"] = device[1]
        views = () if args.compatibility_only else (("high", "128 128 768 15 45 0"), ("low", "128 128 256 8 45 0"))
        for view, camera in views:
            run.command("setpos " + camera)
            run.settle(True, True, idle_uploads=True)
            original = None
            for light, azimuth, elevation, intensity in (("sun0", 0, 35, 1), ("sun180", 180, 35, 1),
                                                         ("sun75", 0, 75, 1), ("sun_off", 0, 35, 0)):
                label = view + "_" + light
                # `wait` advances exactly one normal host frame. Inspect the
                # first eight rendered frames after the cvar change, rather
                # than only observing an eventual zero-upload settled state.
                transition = run.command(f"r_sun_azimuth {azimuth}\nr_sun_angle {elevation}\nr_sun_intensity {intensity}" +
                                         "\nwait\noctree_render_stats" * 8)
                transition_stats = [parse_stats(part) for part in re.split(r"(?=Octree render:)", transition)[1:]]
                if len(transition_stats) != 8 or any(item["uploaded_kib"] or item["retired_mib"] for item in transition_stats):
                    raise RuntimeError(f"sun change uploaded or retired terrain meshes in {label}")
                stats = run.settle(True, True, idle_uploads=True)
                geometry = tuple(stats[name] for name in ("near_draws", "near_triangles", "far_draws", "far_triangles", "gpu_mib"))
                if original is not None and geometry != original:
                    raise RuntimeError(f"sun change altered geometry or GPU allocation in {label}")
                original = geometry
                entry = {"camera": camera, "azimuth": azimuth, "elevation": elevation, "intensity": intensity, "stats": stats,
                         "sun_change_frames_checked": len(transition_stats),
                         "screenshot": photograph(run, label)}
                if light == "sun0":
                    entry["timings"] = run.measure(label)
                if reference:
                    # Older baselines may lack the sun-off capture; their
                    # terrain was independent of all sun controls, so sun0 at
                    # the same camera is the corresponding reference image.
                    reference_label = label if label in reference["views"] else view + "_sun0"
                    prior = reference["views"][reference_label]
                    entry["baseline_reference_view"] = reference_label
                    for name in ("near_draws", "near_triangles", "far_draws", "far_triangles"):
                        if stats[name] != prior["stats"][name]:
                            raise RuntimeError(f"lighting changed {name} from baseline for {label}")
                    entry["baseline_rgb_mean_difference"] = difference(prior["screenshot"], entry["screenshot"])
                summary["views"][label] = entry
            for light in ("sun180", "sun75", "sun_off"):
                value = difference(summary["views"][view + "_sun0"]["screenshot"],
                                   summary["views"][view + "_" + light]["screenshot"])
                summary["checks"][view + "_" + light + "_rgb_mean_difference"] = value
                if not args.baseline and value < .1:
                    raise RuntimeError(f"sun change had no visible terrain response in {view}/{light}: {value}")
        if reference and any(entry["baseline_rgb_mean_difference"] < .1 for entry in summary["views"].values()):
            raise RuntimeError("new lighting did not visibly change every reference view")
        if not args.baseline:
            run.map_command("map e1m1")
            summary["checks"]["bsp_e1m1"] = {"stats": run.settle(False, idle_uploads=True),
                                               "screenshot": photograph(run, "bsp_e1m1")}
            run.command("vid_fsaa 4\nvid_restart")
            run.delay(.5)
            summary["checks"]["bsp_msaa_restart"] = {"stats": run.settle(False, idle_uploads=True),
                                                      "screenshot": photograph(run, "bsp_e1m1_msaa4")}
            run.map_command("world_load lighting_smoke")
            run.command("setpos 128 128 768 15 45 0\nr_sun_azimuth 0\nr_sun_angle 35\nr_sun_intensity 1")
            summary["checks"]["terrain_msaa_restart"] = {"stats": run.settle(True, True, idle_uploads=True),
                                                          "screenshot": photograph(run, "terrain_msaa4")}
            run.command("vid_fsaa 0\nvid_restart")
            summary["checks"]["terrain_restart_no_msaa"] = run.settle(True, True, idle_uploads=True)
            run.command("r_tasks 0")
            summary["checks"]["terrain_sequential"] = {"stats": run.settle(True, True, idle_uploads=True),
                                                        "screenshot": photograph(run, "terrain_sequential")}
        run.command("echo LIGHTING_SMOKE_COMPLETE\nbind F12 quit")
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
