#!/usr/bin/env python3
"""Capture fixed planet surfaces with and without distant LOD.

The binary, configuration, world, logs and captures are isolated in /tmp. Existing
worlds are copied with SQLite backup, and only pak assets are linked. Captures
need visual review; this harness deliberately makes no visual quality assertion.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import tempfile
import time

from run_terrain_lod_smoke import Run, WRAPPER, parse_stats

VIEWS = (("near", (512, 128, 200), (15, 45, 0)),
         ("slope", (1100, 128, 280), (15, 45, 0)),
         ("far_slope", (1800, 128, 360), (15, 45, 0)),
         ("overhead", (1100, 128, 500), (75, 45, 0)))


def settle(run, lod):
    deadline = min(run.deadline, time.monotonic() + run.args.stage_timeout)
    previous, stable = None, 0
    while time.monotonic() < deadline:
        stats = parse_stats(run.command("octree_render_stats"))
        ready = stats["near_triangles"] > 0 and not stats["pending"] and not stats["uploaded_kib"] and not stats["retired_mib"]
        ready &= (stats["selected"] > 0 and stats["cached"] >= stats["selected"]) if lod else not stats["far_triangles"] and not stats["selected"]
        key = tuple(stats[name] for name in ("near_triangles", "far_triangles", "selected", "pending"))
        stable = stable + 1 if ready and key == previous else 0
        if stable >= 3:
            return stats
        previous = key
        run.delay(.25)
    raise RuntimeError(f"surface geometry did not settle: {stats}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--global-cache", type=Path)
    parser.add_argument("--source-world", type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--stage-timeout", type=float, default=60)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--view", nargs=7, action="append", metavar=("NAME", "X", "Y", "Z", "PITCH", "YAW", "ROLL"),
                        help="replace default views with this named camera; may be repeated")
    args = parser.parse_args()
    if not 0 < args.timeout <= 900 or not 0 < args.stage_timeout <= 180:
        parser.error("timeout bounds are 900 and 180 seconds")
    views = VIEWS
    if args.view:
        views = []
        for name, *values in args.view:
            if not name.replace("_", "").replace("-", "").isalnum():
                parser.error("view names must contain only letters, numbers, underscores or hyphens")
            try:
                values = tuple(map(float, values))
            except ValueError:
                parser.error("view coordinates and angles must be numeric")
            if not all(math.isfinite(value) for value in values):
                parser.error("view coordinates and angles must be finite")
            views.append((name, values[:3], values[3:]))
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-planet-surface-"))
    (work / "id1/worlds").mkdir(parents=True)
    (work / "user").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset)
    shutil.copy2(args.binary.resolve(), work / "vkquake")
    if args.global_cache:
        shutil.copy2(args.global_cache, work / "id1/worlds/surface.sqlite3.planetcache")
    if args.source_world:
        with sqlite3.connect(f"file:{args.source_world.resolve()}?mode=ro", uri=True) as src:
            with sqlite3.connect(work / "id1/worlds/surface.sqlite3") as dest:
                src.backup(dest)
    (work / "autokey.c").write_text(WRAPPER)
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", str(work / "autokey.c"), "-o", str(work / "autokey.so"), "-ldl"], check=True, timeout=30)
    settings = '''vid_fullscreen 0
vid_vsync 0
vid_fsaa 0
host_maxfps 60
r_tasks 1
r_fastsky 1
r_clouds 0
r_drawviewmodel 0
viewsize 130
cl_nocsqc 1
cl_bob 0
cl_rollangle 0
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
r_sun_angle 45
r_sun_azimuth 0
r_sun_intensity 1
bind F12 "exec smoke_command.cfg"
'''
    settings += "world_load surface\n" if args.source_world else "world_create surface planet 1\n"
    (work / "id1/startup.cfg").write_text(settings)
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "work": str(work), "binary_sha256": hashlib.sha256((work / "vkquake").read_bytes()).hexdigest(), "captures": {}, "validation_requested": args.validation}
    run = None
    print(work, flush=True)
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "world signon")
        run.command("noclip 1\ngod 1")
        for label, point, angles in views:
            chunks = tuple(int(v // 1024) for v in point)
            local = tuple(v - chunk * 1024 for v, chunk in zip(point, chunks))
            run.command("world_teleport " + " ".join(map(str, chunks + local)))
            run.command("setpos " + " ".join(map(str, local + angles)))
            for lod in (1, 0):
                run.command(f"r_octree_lod {lod}")
                stats = settle(run, lod)
                name = f"{label}_lod{lod}"
                summary["captures"][name] = {"position": point, "angles": angles, "stats": stats, "screenshot": run.photograph(name)}
                print(name, flush=True)
        summary["world_stats"] = run.command("world_stats")
        run.command("echo PLANET_SURFACE_COMPLETE\nbind F12 quit")
        os.write(run.write_fd, b"k")
        run.process.wait(timeout=10)
        if run.process.returncode:
            raise RuntimeError(f"engine exit {run.process.returncode}")
        summary["success"] = True
    except Exception as error:
        summary["error"] = str(error)
    finally:
        if run:
            run.close()
        if args.validation:
            process_log = work / "process.log"
            output = process_log.read_text(errors="replace") if process_log.exists() else ""
            failures = re.findall(r"[^\n]*(?:VUID-|Validation Error)[^\n]*", output)
            summary["validation_layer_enabled"] = "Using VK_LAYER_KHRONOS_validation" in output
            summary["validation_error_count"] = len(failures)
            summary["validation_error_lines"] = failures[:16]
            if failures or not summary["validation_layer_enabled"]:
                summary["success"] = False
                summary.setdefault("error", "Vulkan validation errors or requested layer was not active; inspect process.log")
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({key: summary[key] for key in ("success", "work", "error") if key in summary}), flush=True)
    if not summary["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
