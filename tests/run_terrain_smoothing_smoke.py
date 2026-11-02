#!/usr/bin/env python3
"""Exercise marching-tetrahedra rendering in an isolated graphical world.

Captures the same camera with r_octree_smooth disabled/enabled, checks bounded
stationary GPU allocation, dynamic lighting, chunk-corner excavation, distant
coordinates, empty worlds and renderer restart. The images require visual
review; pixel differences alone do not prove a smooth or watertight surface.
Normal host-frame intervals are descriptive measurements, not GPU timings or
hardware-independent acceptance thresholds. Uses the helpers and requirements
from run_terrain_lod_smoke.py. All files are created in a fresh /tmp directory.
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

from run_terrain_lighting_smoke import difference, photograph
from run_terrain_lod_smoke import Run, WRAPPER, parse_stats


def geometry(stats):
    return tuple(stats[name] for name in ("near_draws", "near_triangles", "far_draws", "far_triangles", "gpu_mib"))


def world_status(run):
    output = run.command("world_stats")
    chunks = re.search(r"World chunks (\d+)/(\d+), pending (\d+), mesh jobs (\d+)", output)
    memory = re.search(r"World CPU memory: trees ([\d.]+) MiB, meshes ([\d.]+) MiB", output)
    if not chunks or not memory:
        raise RuntimeError("world statistics were not available")
    result = dict(zip(("resident_chunks", "chunk_limit", "pending_chunks", "mesh_jobs"), map(int, chunks.groups())))
    result.update(tree_mib=float(memory[1]), mesh_mib=float(memory[2]))
    return result


def idle_world(run):
    # Stable GPU output can conceal CPU meshes being constantly evicted and
    # rebuilt. Require the worker queues to drain before timing the view.
    samples = []
    for _ in range(12):
        samples.append(world_status(run))
        if len(samples) >= 3 and all(not entry["pending_chunks"] and not entry["mesh_jobs"] for entry in samples[-3:]):
            return samples
        run.delay(.5)
    raise RuntimeError("stationary world mesh workers did not become idle: " + json.dumps(samples[-3:]))


def camera(run, specification):
    values = list(map(float, specification.split()))
    chunks = [math.floor(value / 1024) for value in values[:3]]
    local = [value - chunk * 1024 for value, chunk in zip(values[:3], chunks)]
    # setpos addresses the current floating frame, so teleport canonically
    # before setting angles. Reusing absolute-looking setpos coordinates after
    # crossing a chunk boundary would move the camera to a different location.
    run.command("world_teleport " + " ".join(map(str, chunks + local)) + "\nwait\nsetpos " +
                " ".join(map(str, local + values[3:])))
    run.delay(.15)
    output = run.command("world_stats")
    match = re.search(r"World position: chunk (-?\d+) (-?\d+) (-?\d+) local ([\d.-]+) ([\d.-]+) ([\d.-]+)", output)
    if not match or list(map(int, match.groups()[:3])) != chunks:
        raise RuntimeError("camera did not reach its canonical chunk: " + output)
    actual = list(map(float, match.groups()[3:]))
    # Render position includes the player's eye height. Horizontal coordinates
    # are unaffected; network position encoding adds at most a small rounding.
    if any(abs(a - b) > .1 for a, b in zip(actual[:2], local[:2])):
        raise RuntimeError("camera did not reach its canonical local position: " + output)


def edit(run, command, result):
    count = run.text().count(result)
    output = run.command(command)
    if any(message in output for message in ("Edit failed:", "No loaded terrain within reach", "Placement overlaps")):
        raise RuntimeError("edit rejected: " + output)
    run.wait(lambda text: text.count(result) > count, result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
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
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-terrain-smoothing-"))
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
r_octree_smooth 0
r_sun_azimuth 0
r_sun_angle 35
r_sun_intensity 1
bind F12 "exec smoke_command.cfg"
world_create smoothing_smoke terrain 4242
""")
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "work": str(work), "binary": str(args.binary.resolve()),
               "resolution": [args.width, args.height], "worker_count": "engine default; no override",
               "method": "normal host-frame input-batch intervals; warm stationary camera; clouds disabled",
               "capture_only_fps_cap": 60, "visual_review_required": True, "comparisons": {}, "checks": {}}
    run = None
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "initial terrain signon")
        if 'Unknown command "r_octree_smooth"' in run.text():
            raise RuntimeError("binary does not provide r_octree_smooth")
        device = re.search(r"^Device: (.+)$", run.text(), re.MULTILINE)
        if device:
            summary["gpu"] = device[1]
        for view, viewpoint in (("high", "128 128 768 15 45 0"), ("low", "128 128 256 8 45 0")):
            camera(run, viewpoint)
            for enabled in (0, 1):
                label = f"{view}_smooth{enabled}"
                run.command(f"r_octree_smooth {enabled}")
                stats = run.settle(True, True, idle_uploads=True)
                world_samples = idle_world(run)
                summary["comparisons"][label] = {"camera": viewpoint, "stats": stats, "world_idle_samples": world_samples,
                                                  "timings": run.measure(label),
                                                  "screenshot": photograph(run, label)}
            first = summary["comparisons"][view + "_smooth0"]
            second = summary["comparisons"][view + "_smooth1"]
            change = difference(first["screenshot"], second["screenshot"])
            summary["checks"][view + "_smoothing_rgb_mean_difference"] = change
            if change < .1:
                raise RuntimeError(f"smoothing has no visible effect at {view} camera: {change}")
        original = run.settle(True, True, idle_uploads=True)
        output = run.command("r_sun_azimuth 180" + "\nwait\noctree_render_stats" * 8)
        transitions = [parse_stats(part) for part in re.split(r"(?=Octree render:)", output)[1:]]
        if len(transitions) != 8 or any(item["uploaded_kib"] or item["retired_mib"] for item in transitions):
            raise RuntimeError("sun movement rebuilt terrain geometry")
        light_stats = run.settle(True, True, idle_uploads=True)
        if geometry(original) != geometry(light_stats):
            raise RuntimeError("sun movement changed stable terrain geometry")
        lit_image = photograph(run, "low_smooth1_sun180")
        sun_difference = difference(summary["comparisons"]["low_smooth1"]["screenshot"], lit_image)
        if sun_difference < .1:
            raise RuntimeError("smoothed terrain does not respond to sun movement")
        summary["checks"]["sun_response"] = {"stats": light_stats, "frames_checked": len(transitions),
                                                "rgb_mean_difference": sun_difference, "screenshot": lit_image}
        run.command("r_sun_azimuth 0")
        camera(run, "956 956 190 45 45 0")
        before = run.settle(True, True, idle_uploads=True)
        before_image = photograph(run, "chunk_corner_before")
        # Four adjacent voxel columns belong to four different chunks. Remove
        # five cells per column, then add one back through the supported edit
        # commands. Each edit waits for its durable completion acknowledgment.
        for x, y in ((1020, 1020), (1028, 1020), (1020, 1028), (1028, 1028)):
            camera(run, f"{x} {y} 200 90 0 0")
            for _ in range(5):
                edit(run, "world_remove", "Voxel removed")
        edit(run, "world_place 1", "Voxel placed")
        camera(run, "956 956 190 45 45 0")
        after = run.settle(True, True, idle_uploads=True)
        if geometry(before) == geometry(after):
            raise RuntimeError("chunk-corner edits did not change geometry")
        after_image = photograph(run, "chunk_corner_excavation")
        summary["checks"]["chunk_corner_excavation"] = {"removed": 20, "placed": 1, "before": before, "after": after,
                                                         "before_screenshot": before_image, "screenshot": after_image,
                                                         "rgb_mean_difference": difference(before_image, after_image)}
        for label, coordinates in (("negative_boundary", "-1 -1"), ("huge_coordinates", "9007199254740991 -9007199254740991"),
                                   ("return_origin", "0 0")):
            run.command(f"world_teleport {coordinates} 0 128 128 768")
            stats = run.settle(True, True, idle_uploads=True)
            summary["checks"][label] = {"stats": stats, "screenshot": photograph(run, label)}
        run.command("r_octree_mesh_mb 16\nr_octree_upload_mb 1")
        for x, y in ((6, 0), (-2, -1), (7, 2), (-1, 1), (0, 0)):
            run.command(f"world_teleport {x} {y} 0 128 128 768")
            run.delay(.1)
        stats = run.settle(True, idle_uploads=True)
        if stats["gpu_mib"] > 16.1:
            raise RuntimeError(f"low-memory terrain did not converge under 16 MiB: {stats}")
        run.delay(1)
        repeat = run.settle(True, idle_uploads=True)
        if geometry(stats) != geometry(repeat):
            raise RuntimeError("stationary low-memory terrain kept changing")
        summary["checks"]["low_memory_churn"] = {"stats": stats, "repeat": repeat,
                                                  "world_idle_samples": idle_world(run),
                                                  "screenshot": photograph(run, "low_memory_churn")}
        run.command("r_octree_mesh_mb 128\nr_octree_upload_mb 8")
        run.map_command("world_create smoothing_empty empty 4242")
        stats = run.settle(False, idle_uploads=True)
        if stats["near_triangles"]:
            raise RuntimeError("empty generator produced terrain geometry")
        summary["checks"]["empty_generator"] = stats
        run.map_command("map __empty")
        summary["checks"]["empty_map"] = run.settle(False, idle_uploads=True)
        run.map_command("map e1m1")
        summary["checks"]["bsp"] = {"stats": run.settle(False, idle_uploads=True),
                                      "screenshot": photograph(run, "bsp_e1m1")}
        run.command("vid_fsaa 4\nvid_restart")
        summary["checks"]["bsp_video_restart"] = run.settle(False, idle_uploads=True)
        run.map_command("world_load smoothing_smoke")
        camera(run, "956 956 190 45 45 0")
        summary["checks"]["persisted_excavation"] = {"stats": run.settle(True, True, idle_uploads=True),
                                                      "screenshot": photograph(run, "persisted_excavation")}
        run.command("vid_fsaa 0\nvid_restart\nr_tasks 0")
        camera(run, "128 128 768 15 45 0")
        summary["checks"]["terrain_sequential_restart"] = run.settle(True, True, idle_uploads=True)
        run.command("r_tasks 1\nr_fastsky 0\nr_atmosphere 1\nr_clouds 1\nr_clouds_speed 0\nr_octree_lod_distance 65536")
        run.delay(1)
        summary["checks"]["atmosphere_65536"] = {"stats": run.settle(True, True, idle_uploads=True),
                                                  "screenshot": photograph(run, "atmosphere_65536")}
        run.command("echo SMOOTHING_SMOKE_COMPLETE\nbind F12 quit")
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
