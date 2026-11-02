#!/usr/bin/env python3
"""Capture curved-cloud horizons and exercise cloud configuration/lifetimes.

Run --baseline against the prior binary, then --reference its summary.json.
Uses the normal graphical task path and isolated /tmp game data, following
run_terrain_lod_smoke.py. Wind is stopped and current ray-march jitter is fixed;
older baseline builds may dither over time. Screenshots require visual review.
Host-frame intervals include scheduling/presentation. Optional sky_stats GPU
queries are recorded separately, with profiling disabled during host timings.
--require-speedup enforces the project target on the same machine and settings;
it is not a universal FPS threshold. Saved player configurations are untouched.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile
import time

from PIL import Image, ImageChops, ImageStat

from run_terrain_lighting_smoke import difference, photograph
from run_terrain_lod_smoke import Run, WRAPPER, parse_stats
from run_terrain_smoothing_smoke import camera, idle_world, world_status


VIEWS = (("horizontal", "128 128 256 0 45 0"),
         ("slightly_down", "128 128 768 5 45 0"),
         ("inside_layer", "128 128 2600 0 45 0"),
         ("above_layer", "128 128 5000 15 45 0"))


def paired_cloud_timings(run, label):
    """Alternate warm off/on measurements to expose baseline or clock drift."""
    pairs = []
    for index in range(run.args.pairs):
        pair = {}
        for enabled, name in ((0, "off"), (1, "on")):
            run.command(f"r_clouds {enabled}")
            run.delay(.3)
            pair[name] = run.measure(f"{label}_{index}_{name}")
        pairs.append(pair)
    return pairs


def summarize_pairs(pairs):
    off = statistics.median(pair["off"]["mean_ms"] for pair in pairs)
    on = statistics.median(pair["on"]["mean_ms"] for pair in pairs)
    incremental = statistics.median(pair["on"]["mean_ms"] - pair["off"]["mean_ms"] for pair in pairs)
    return {"clouds_off_ms": off, "clouds_on_ms": on, "incremental_cloud_ms": incremental,
            "statistic": "median of paired mean host-frame intervals"}


def compare_pairs(current, baseline):
    old, new = summarize_pairs(baseline), summarize_pairs(current)
    if old["incremental_cloud_ms"] <= 0:
        raise ValueError("baseline has no measurable incremental cloud cost")
    return {"baseline": old, "current": new,
            "incremental_reduction_percent": 100 * (1 - new["incremental_cloud_ms"] / old["incremental_cloud_ms"]),
            "clouds_off_change_percent": 100 * (new["clouds_off_ms"] / old["clouds_off_ms"] - 1)}


def sky_stats(run, profiling=False):
    if profiling:
        run.command("r_sky_profile 1")
        # Edge diagnostics may be read back only once per second. Keep that
        # sampling cost outside the unprofiled host-frame measurements.
        run.delay(1.1)
    try:
        output = run.command("sky_stats")
        if 'Unknown command "sky_stats"' in output:
            raise RuntimeError("sky_stats is missing in the candidate binary")
        # Preserve unavailable/unsupported diagnostics explicitly; zero alone
        # is not evidence that a GPU operation had no elapsed cost.
        result = {"profiling_enabled": profiling, "raw": output.strip()}
        clouds = re.search(r"Sky clouds: path (\w+) resolution (\d+) (\d+) divisor (\d+) filter (\w+) allocated_bytes (\d+)", output)
        atmosphere = re.search(r"Sky atmosphere: path (\w+) available (\d+) rebuilds (\d+) allocated_bytes (\d+)", output)
        gpu = re.search(r"Sky GPU: available (\d+) profiling (\d+) samples (\d+) atmosphere_ms ([\d.]+) lut_ms ([\d.]+) "
                        r"cloud_trace_ms ([\d.]+) cloud_composite_ms ([\d.]+)", output)
        edges = re.search(r"Sky edges: available (\d+) fallback_pixels (\d+) total_pixels (\d+) percent ([\d.]+)", output)
        if not (clouds and atmosphere and gpu and edges):
            raise RuntimeError("incomplete sky_stats output: " + output)
        result["clouds"] = {"path": clouds[1], "width": int(clouds[2]), "height": int(clouds[3]), "divisor": int(clouds[4]),
                            "filter": clouds[5], "allocated_bytes": int(clouds[6])}
        result["atmosphere"] = {"path": atmosphere[1], "available": bool(int(atmosphere[2])), "rebuilds": int(atmosphere[3]),
                                "allocated_bytes": int(atmosphere[4])}
        result["gpu"] = dict(zip(("available", "profiling", "samples"), map(int, gpu.groups()[:3])))
        result["gpu"].update(zip(("atmosphere_ms", "lut_ms", "cloud_trace_ms", "cloud_composite_ms"), map(float, gpu.groups()[3:])))
        result["edges"] = {"available": bool(int(edges[1])), "fallback_pixels": int(edges[2]), "total_pixels": int(edges[3]), "percent": float(edges[4])}
        bands = re.search(r"Sky bands: cascades (\d+) diagnostic (\d+) split ([\d.]+) near_divisor (\d+) far_divisor (\d+) "
                          r"near_resolution (\d+) (\d+) far_resolution (\d+) (\d+)", output)
        band_gpu = re.search(r"Sky GPU bands: near_ms ([\d.]+) far_ms ([\d.]+)", output)
        if bands:
            result["bands"] = {"cascades": int(bands[1]), "diagnostic": int(bands[2]), "split": float(bands[3]),
                               "near_divisor": int(bands[4]), "far_divisor": int(bands[5]),
                               "near_resolution": [int(bands[6]), int(bands[7])],
                               "far_resolution": [int(bands[8]), int(bands[9])]}
        if band_gpu:
            result["gpu_bands"] = {"near_ms": float(band_gpu[1]), "far_ms": float(band_gpu[2])}
        return result
    finally:
        if profiling:
            run.command("r_sky_profile 0")


def check_defaults(run):
    expected = {"r_clouds_resolution": "2", "r_clouds_filter": "0", "r_atmosphere_lut": "1", "r_sky_profile": "0"}
    result = {}
    for name, value in expected.items():
        output = run.command(name)
        match = re.search(r'"' + name + r'" is "([^"]+)"', output)
        if not match or float(match[1]) != float(value):
            raise RuntimeError(f"unexpected fresh-config default for {name}: {output}")
        result[name] = match[1]
    return result


def full_image_difference(first, second):
    with Image.open(first) as a, Image.open(second) as b:
        if a.size != b.size:
            raise ValueError("cloud captures have different dimensions")
        return sum(ImageStat.Stat(ImageChops.difference(a.convert("RGB"), b.convert("RGB"))).mean) / 3


def highlight_fraction(path):
    # The lower 60% of above-cloud views contains the cloud deck, while the
    # upper sky has little useful highlight information. Record a descriptive
    # measure for tone-map comparisons; a white pixel alone is not an error.
    with Image.open(path) as image:
        area = image.convert("RGB").crop((0, image.height * 2 // 5, image.width, image.height))
        pixels = area.tobytes()
        near_white, clipped = 0, 0
        for rgb in zip(pixels[0::3], pixels[1::3], pixels[2::3]):
            minimum = min(rgb)
            near_white += minimum >= 250
            clipped += minimum == 255
        count = area.width * area.height
        return {"near_white_percent": 100 * near_white / count, "clipped_white_percent": 100 * clipped / count}


def sky_rows(path):
    # The five-degree downward view places the geometric horizon above the
    # midpoint. Report full-width row means instead of relying on a particular
    # cloud pattern, and leave interpretation to visual review.
    with Image.open(path) as image:
        image = image.convert("RGB")
        return [{"y": y, "rgb": ImageStat.Stat(image.crop((0, y, image.width, y + 1))).mean}
                for y in range(image.height // 3, image.height // 2 + 1)]


def settled_terrain(run):
    # Steep downward views cull all far draws, and high cameras cull all near
    # draws. Require settled caches rather than requiring either draw class.
    deadline = min(run.deadline, time.monotonic() + run.args.stage_timeout)
    previous, stable, last = None, 0, None
    while time.monotonic() < deadline:
        last = parse_stats(run.command("octree_render_stats"))
        if last["gpu_mib"] > 128.1 or last["cpu_mib"] > 32.01 or last["selected"] > 512 or last["cached"] > 512 or last["pending"] > 8 or last["failed"]:
            raise RuntimeError(f"terrain cache bounds/build failure: {last}")
        ready = (last["selected"] > 0 and last["cached"] >= last["selected"] and not last["pending"] and
                 not last["uploaded_kib"] and not last["retired_mib"])
        key = tuple(last[name] for name in ("selected", "cached", "pending", "near_triangles", "far_triangles"))
        stable = stable + 1 if ready and key == previous else 0
        if stable >= 2:
            return last
        previous = key
        run.delay(.35)
    raise RuntimeError(f"cloud view terrain did not settle: {last}")


def capture(run, label, viewpoint=None, terrain=True, timing=False):
    if viewpoint:
        camera(run, viewpoint)
    if terrain:
        stats = settled_terrain(run)
        if run.args.validation:
            # Debug marching-tetrahedra builds can exceed the release helper's
            # six-second budget while validation also instruments the renderer.
            deadline = min(run.deadline, time.monotonic() + run.args.stage_timeout)
            samples = []
            while time.monotonic() < deadline:
                samples.append(world_status(run))
                if len(samples) >= 3 and all(not entry["pending_chunks"] and not entry["mesh_jobs"] for entry in samples[-3:]):
                    break
                run.delay(.5)
            else:
                raise RuntimeError("debug world mesh workers did not become idle: " + json.dumps(samples[-3:]))
        else:
            idle_world(run)
    else:
        stats = run.settle(False, idle_uploads=True)
    run.delay(.15)
    result = {"camera": viewpoint, "stats": stats, "screenshot": photograph(run, label)}
    result["horizon_rows"] = sky_rows(result["screenshot"])
    result["highlights"] = highlight_fraction(result["screenshot"])
    if timing and not run.args.validation:
        result["timings"] = run.measure(label)
    return result


def quality_checks(run, checks):
    camera(run, VIEWS[1][1])
    for divisor in (1, 2, 4):
        for filtering in (0, 1):
            label = f"resolution_{divisor}_filter_{filtering}"
            run.command(f"r_clouds_resolution {divisor}\nr_clouds_filter {filtering}")
            result = capture(run, label, timing=True)
            result["sky"] = sky_stats(run, profiling=True)
            if result["sky"]["clouds"]["divisor"] != divisor or result["sky"]["clouds"]["filter"] != ("linear" if filtering else "nearest"):
                raise RuntimeError("cloud quality controls were not applied: " + result["sky"]["raw"])
            checks[label] = result
        delta = full_image_difference(checks[f"resolution_{divisor}_filter_0"]["screenshot"],
                                      checks[f"resolution_{divisor}_filter_1"]["screenshot"])
        if delta <= .01:
            raise RuntimeError(f"nearest/linear filtering did not change clouds at divisor {divisor}")
        checks[f"filter_difference_{divisor}"] = delta
    run.command("r_clouds_resolution 2\nr_clouds_filter 0")
    for enabled in (0, 1):
        run.command(f"r_atmosphere_lut {enabled}")
        checks[f"atmosphere_lut_{enabled}"] = capture(run, f"atmosphere_lut_{enabled}", timing=True)
        checks[f"atmosphere_lut_{enabled}"]["sky"] = sky_stats(run, profiling=True)
    checks["atmosphere_lut_difference"] = full_image_difference(
        checks["atmosphere_lut_0"]["screenshot"], checks["atmosphere_lut_1"]["screenshot"])
    previous = sky_stats(run)["atmosphere"]
    camera(run, VIEWS[0][1])
    after_motion = sky_stats(run)["atmosphere"]
    if previous["available"] and after_motion["rebuilds"] != previous["rebuilds"]:
        raise RuntimeError("camera motion rebuilt the fixed-altitude atmosphere LUT")
    run.command("r_sun_intensity 4")
    run.delay(.25)
    after_intensity = sky_stats(run)["atmosphere"]
    if previous["available"] and after_intensity["rebuilds"] != after_motion["rebuilds"]:
        raise RuntimeError("sun intensity rebuilt the direction-only atmosphere LUT")
    run.command("r_sun_intensity 1")
    run.command("r_sun_azimuth 45")
    run.delay(.25)
    after_sun = sky_stats(run)["atmosphere"]
    if previous["available"] and after_sun["rebuilds"] <= after_motion["rebuilds"]:
        raise RuntimeError("sun direction change did not invalidate the atmosphere LUT")
    checks["atmosphere_invalidation"] = {"before": previous, "after_camera": after_motion,
                                          "after_intensity": after_intensity, "after_sun": after_sun}
    run.command("r_sun_azimuth 0")
    run.command("r_clouds 0")
    for angle in (0, 45, -6):
        run.command(f"r_sun_angle {angle}")
        images = []
        for enabled in (0, 1):
            label = f"clear_atmosphere_sun{angle}_lut{enabled}"
            run.command(f"r_atmosphere_lut {enabled}")
            checks[label] = capture(run, label)
            images.append(checks[label]["screenshot"])
        checks[f"clear_atmosphere_sun{angle}_difference"] = full_image_difference(*images)
    run.command("r_sun_angle 35\nr_clouds 1")


def movement_checks(run, checks):
    camera(run, "128 128 2200 -5 45 0")
    run.command("r_clouds_speed 1")
    first = photograph(run, "wind_first")
    run.delay(1)
    second = photograph(run, "wind_second")
    delta = full_image_difference(first, second)
    if delta <= .01:
        raise RuntimeError("cloud wind did not change the rendered image")
    checks["animated_wind"] = {"first": first, "second": second, "rgb_mean_difference": delta}
    run.command("r_clouds_speed 0")
    # Exact image comparisons prove deterministic stationary rendering;
    # rotation/travel sequences remain captures for human visual review.
    for label, viewpoint in (("rotation_left", "128 128 2200 -5 0 0"),
                             ("rotation_right", "128 128 2200 -5 90 0"),
                             ("travel_below", "128 128 1900 0 45 0"),
                             ("travel_inside", "256 256 2750 0 45 0"),
                             ("travel_above", "384 384 3600 15 45 0")):
        checks[label] = capture(run, label, viewpoint)
    anchor = "128 128 2200 -5 45 0"
    before = capture(run, "stochastic_roundtrip_before", anchor)
    # Move beyond the roughly 13,333-unit source-noise repeat, then cross the
    # 133,333-unit canonical sampling-phase wrap with a small camera move.
    for label, viewpoint in (("stochastic_neighbor_cell", "14224 13210 2200 -5 45 0"),
                             ("noise_phase_before", "133331 128 2200 -5 45 0"),
                             ("noise_phase_after", "133335 128 2200 -5 45 0")):
        checks[label] = capture(run, label, viewpoint)
    checks["noise_phase_boundary_difference"] = full_image_difference(
        checks["noise_phase_before"]["screenshot"], checks["noise_phase_after"]["screenshot"])
    # Keep equivalent full-resolution captures to distinguish source-noise
    # discontinuities from reduced-resolution classification/upscaling edges.
    run.command("r_clouds_resolution 1")
    for suffix, viewpoint in (("before", "133331 128 2200 -5 45 0"),
                              ("after", "133335 128 2200 -5 45 0")):
        label = f"noise_phase_full_{suffix}"
        checks[label] = capture(run, label, viewpoint)
    checks["noise_phase_full_boundary_difference"] = full_image_difference(
        checks["noise_phase_full_before"]["screenshot"], checks["noise_phase_full_after"]["screenshot"])
    run.command("r_clouds_resolution 2")
    after = capture(run, "stochastic_roundtrip_after", anchor)
    delta = full_image_difference(before["screenshot"], after["screenshot"])
    if delta > .01:
        raise RuntimeError(f"cloud pattern changed after returning to the same canonical camera: {delta}")
    checks["stochastic_camera_roundtrip"] = {"before": before, "after": after, "rgb_mean_difference": delta}


def viewport_checks(run, checks):
    # Odd target dimensions, a partial scene viewport, and resolution changes
    # exercise different block extents; restore the baseline for later checks.
    original = sky_stats(run)
    odd_width = run.args.width if run.args.width % 2 else run.args.width + 1
    odd_height = run.args.height if run.args.height % 2 else run.args.height + 1
    run.command(f"vid_width {odd_width}\nvid_height {odd_height}\nvid_restart")
    for samples in (0, 4):
        run.command(f"vid_fsaa {samples}\nvid_restart")
        for scale in (1, 2):
            run.command(f"r_scale {scale}")
            for viewsize in (130, 80):
                label = f"odd_msaa{samples}_scale{scale}_view{viewsize}"
                run.command(f"viewsize {viewsize}")
                checks[label] = capture(run, label)
                checks[label]["sky"] = sky_stats(run)
                with Image.open(checks[label]["screenshot"]) as image:
                    checks[label]["actual_resolution"] = list(image.size)
                    if image.size != (odd_width, odd_height):
                        raise RuntimeError(f"video restart did not apply odd dimensions: {image.size}")
    # FSAA changes recreate resources immediately and resync size cvars to the
    # current window. Set it before requesting the restored window dimensions.
    run.command(f"vid_fsaa 0\nvid_width {run.args.width}\nvid_height {run.args.height}\nr_scale 1\nviewsize 130\nvid_restart")
    run.delay(.25)
    restored_image = photograph(run, "viewport_restored")
    with Image.open(restored_image) as image:
        if image.size != (run.args.width, run.args.height):
            raise RuntimeError(f"video mode did not return to its original dimensions: {image.size}")
    restored = sky_stats(run)
    for component in ("clouds", "atmosphere"):
        if restored[component]["allocated_bytes"] != original[component]["allocated_bytes"]:
            raise RuntimeError(f"sky allocation changed after restoring the original video mode: {original} -> {restored}")
    checks["viewport_restore"] = {"before": original, "after": restored, "screenshot": restored_image}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--visual-only", action="store_true", help="capture fixed views, stability, wind, travel and noise-phase boundaries without the quality/lifecycle matrix")
    parser.add_argument("--benchmark-only", action="store_true", help="only measure the four fixed views and collect GPU diagnostics")
    parser.add_argument("--compatibility-only", action="store_true", help="exercise depth, viewport, restart and map lifetimes without repeating quality/motion checks")
    parser.add_argument("--views", nargs="+", choices=[label for label, _ in VIEWS],
                        help="benchmark-only subset for quick iterations; final acceptance always requires all four views")
    parser.add_argument("--require-speedup", action="store_true", help="fail unless every view improves incremental cloud cost by at least 50%% against --reference")
    parser.add_argument("--pairs", type=int, default=3, help="alternating clouds-off/on measurements per view (1..5)")
    parser.add_argument("--validation", action="store_true", help="enable Vulkan validation and skip host-frame performance measurements")
    parser.add_argument("--lighting-checks", action="store_true", help="also capture strong sun and sunset for tone-map visual review")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--stage-timeout", type=float, default=30)
    parser.add_argument("--sample-seconds", type=float, default=2)
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
    if args.baseline and args.validation:
        parser.error("performance baselines must be measured without --validation")
    if not 1 <= args.pairs <= 5:
        parser.error("--pairs must be 1..5")
    if args.require_speedup and (not args.reference or args.validation or args.visual_only or args.compatibility_only):
        parser.error("--require-speedup needs --reference and timings without --validation/--visual-only/--compatibility-only")
    if sum((args.benchmark_only, args.visual_only, args.compatibility_only)) > 1:
        parser.error("--benchmark-only, --visual-only and --compatibility-only are mutually exclusive")
    if args.baseline and args.compatibility_only:
        parser.error("--baseline cannot be combined with --compatibility-only")
    if args.views and not args.benchmark_only:
        parser.error("--views is only supported with --benchmark-only")
    if args.require_speedup and args.views and set(args.views) != {label for label, _ in VIEWS}:
        parser.error("--require-speedup requires all four baseline views")
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
    if reference and any(not reference.get("views", {}).get(label, {}).get("paired_cloud_timings") for label, _ in VIEWS):
        parser.error("reference must contain alternating off/on pairs for every fixed view")
    work = Path(tempfile.mkdtemp(prefix="quiver-cloud-horizon-"))
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
gamma 0.9
contrast 1.4
host_maxfps 0
host_speeds 0
r_speeds 0
r_tasks 1
r_parallelmark 1
r_indirect 1
r_gpulightmapupdate 1
r_scale 1
r_fastsky 0
r_atmosphere 1
r_clouds 1
r_clouds_speed 0
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
r_octree_lod_distance 65536
r_octree_smooth 1
r_sun_azimuth 0
r_sun_angle 35
r_sun_intensity 1
bind F12 "exec smoke_command.cfg"
world_create horizon_smoke terrain 4242
""")
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "baseline": args.baseline, "visual_only": args.visual_only, "benchmark_only": args.benchmark_only,
               "compatibility_only": args.compatibility_only,
               "validation": args.validation, "work": str(work), "binary": str(args.binary.resolve()), "preserved_binary": str(work / "vkquake"),
               "resolution": [args.width, args.height], "worker_count": "engine default; no override",
               "method": ("normal graphical correctness checks; host-frame timings disabled under validation" if args.validation else
                          "normal host-frame input-batch intervals; warm stationary camera; stopped cloud wind"),
               "visual_review_required": True, "reference": str(args.reference.resolve()) if args.reference else None,
               "views": {}, "checks": {}, "performance_acceptance": {}, "warnings": []}
    run = None
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "initial terrain signon")
        device = re.search(r"^Device: (.+)$", run.text(), re.MULTILINE)
        if device:
            summary["gpu"] = device[1]
        if not args.baseline:
            summary["checks"]["defaults"] = check_defaults(run)
        for label, viewpoint in VIEWS:
            if args.compatibility_only or (args.views and label not in args.views):
                continue
            result = capture(run, label, viewpoint, timing=True)
            if not args.visual_only and not args.validation:
                result["paired_cloud_timings"] = paired_cloud_timings(run, label)
                result["paired_summary"] = summarize_pairs(result["paired_cloud_timings"])
            if not args.baseline:
                result["sky_gpu_diagnostics"] = sky_stats(run, profiling=True)
            if reference:
                result["baseline_timings"] = reference["views"][label]["timings"]
                if not args.visual_only and not args.validation:
                    comparison = compare_pairs(result["paired_cloud_timings"], reference["views"][label]["paired_cloud_timings"])
                    summary["performance_acceptance"][label] = comparison
                    if comparison["clouds_off_change_percent"] > 5:
                        summary["warnings"].append(f"{label}: clouds-off host frame cost rose more than 5%; repeat/investigate before attribution")
            summary["views"][label] = result
        if not args.baseline and not args.benchmark_only:
            if not args.compatibility_only:
                near_clouds = capture(run, "near_clouds", "128 128 2200 -5 45 0", timing=True)
                run.delay(.5)
                repeated = photograph(run, "near_clouds_stationary")
                stationary_delta = full_image_difference(near_clouds["screenshot"], repeated)
                if stationary_delta > .01:
                    raise RuntimeError(f"stopped-wind cloud pixels shimmer at a fixed camera: {stationary_delta}")
                summary["checks"]["near_clouds"] = near_clouds
                summary["checks"]["stationary_clouds"] = {"rgb_mean_difference": stationary_delta, "screenshot": repeated}
                run.command("r_clouds_density 1\nr_clouds_coverage .75")
                summary["checks"]["near_dense_clouds"] = capture(run, "near_dense_clouds")
                run.command("r_clouds_density .5\nr_clouds_coverage .4")
            if not args.compatibility_only:
                if not args.visual_only:
                    quality_checks(run, summary["checks"])
                movement_checks(run, summary["checks"])
            if args.lighting_checks and not args.compatibility_only:
                run.command("r_sun_intensity 4")
                summary["checks"]["above_strong_sun"] = capture(run, "above_strong_sun", "128 128 5000 15 45 0")
                run.command("contrast 2")
                summary["checks"]["above_strong_sun_contrast2"] = capture(run, "above_strong_sun_contrast2")
                run.command("contrast 1.4\nr_sun_angle 2")
                summary["checks"]["above_strong_sunset"] = capture(run, "above_strong_sunset")
                run.command("r_sun_intensity 1")
                summary["checks"]["above_sunset"] = capture(run, "above_sunset")
                run.command("r_sun_angle 35")
            if not args.visual_only:
                camera(run, VIEWS[1][1])
                for label, command in (("density_zero", "r_clouds_density 0"),
                                       ("coverage_zero", "r_clouds_density .5\nr_clouds_coverage 0"),
                                       ("thin_low_clouds", "r_clouds_coverage .4\nr_clouds_height 500\nr_clouds_thickness 100"),
                                       ("sunset", "r_clouds_height 2000\nr_clouds_thickness 1500\nr_sun_angle 2"),
                                       ("night", "r_sun_angle -15"),
                                       ("fog", "r_sun_angle 35\nfog .2 .3 .4 .5")):
                    run.command(command)
                    summary["checks"][label] = capture(run, label)
                # Terrain is at least 64 units high. A dense 8..16-unit cloud
                # layer is wholly behind opaque terrain in this downward view.
                # Compare only the lower half to exclude sky and temporal dither.
                run.command("fog 0\nr_clouds_height 8\nr_clouds_thickness 8\nr_clouds_density 1\nr_clouds_coverage 1")
                opaque_clouds = capture(run, "opaque_ground_clouds", "956 956 190 65 45 0")
                run.command("r_clouds 0")
                opaque_clear = capture(run, "opaque_ground_clear")
                occlusion_delta = difference(opaque_clouds["screenshot"], opaque_clear["screenshot"])
                if occlusion_delta > .01:
                    raise RuntimeError(f"clouds leaked through opaque foreground terrain: {occlusion_delta}")
                summary["checks"]["opaque_foreground"] = {"rgb_mean_difference": occlusion_delta,
                                                             "clouds": opaque_clouds, "clear": opaque_clear}
                run.command("r_clouds 1\nr_clouds_height 2000\nr_clouds_thickness 1500\nr_clouds_density 1\nr_clouds_coverage 1")
                above_clouds = capture(run, "above_dense_ground_clouds", "128 128 5000 65 45 0")
                run.command("r_clouds 0")
                above_clear = capture(run, "above_dense_ground_clear")
                above_delta = difference(above_clouds["screenshot"], above_clear["screenshot"])
                if above_delta < 1:
                    raise RuntimeError(f"near clouds failed to obscure ground below the layer: {above_delta}")
                summary["checks"]["clouds_in_front_of_ground"] = {"rgb_mean_difference": above_delta,
                                                                  "clouds": above_clouds, "clear": above_clear}
                run.command("r_clouds 1\nr_clouds_height 2000\nr_clouds_thickness 1500\nr_clouds_density .5\nr_clouds_coverage .4")
                camera(run, VIEWS[3][1])
                run.command("vid_fsaa 4\nvid_restart")
                summary["checks"]["msaa_restart"] = capture(run, "msaa_restart")
                run.command("vid_fsaa 0\nvid_restart")
                summary["checks"]["second_restart"] = capture(run, "second_restart")
                viewport_checks(run, summary["checks"])
                run.command("r_fastsky 1")
                summary["checks"]["fastsky"] = capture(run, "fastsky")
                run.command("r_fastsky 0")
                for label, coordinates in (("negative_coordinates", "-1 -1"),
                                           ("huge_coordinates", "9007199254740991 -9007199254740991")):
                    run.command(f"world_teleport {coordinates} 0 128 128 768")
                    summary["checks"][label] = capture(run, label)
                run.map_command("world_create horizon_empty empty 4242")
                summary["checks"]["empty_world"] = capture(run, "empty_world", "128 128 256 5 45 0", terrain=False)
                run.map_command("map __empty")
                run.command("setpos 128 128 256 5 45 0")
                summary["checks"]["empty_map"] = capture(run, "empty_map", terrain=False)
                run.map_command("map e1m1")
                # Coordinates are below the outdoor sky ceiling in stock e1m1.
                run.command("setpos 640 960 300 -30 90 0\nr_drawviewmodel 1\nviewsize 120")
                summary["checks"]["bsp"] = capture(run, "bsp", terrain=False)
                run.command("r_clouds 0")
                summary["checks"]["bsp_clouds_off"] = capture(run, "bsp_clouds_off", terrain=False)
                run.command("r_clouds 1\nvid_fsaa 4\nvid_restart")
                summary["checks"]["bsp_msaa"] = capture(run, "bsp_msaa", terrain=False)
                # Lower clouds intersect the outdoor roof and courtyard, so
                # the boundary path is exercised in an ordinary BSP scene.
                run.command("r_clouds_height 280\nr_clouds_thickness 350\nr_clouds_density .08\nr_clouds_coverage .5")
                summary["checks"]["bsp_roof_intersection"] = capture(run, "bsp_roof_intersection", terrain=False)
                # The open water beneath the courtyard bridge is at z=-296.
                # A wide view includes water, the bridge, and the high sky
                # opening together. Disable PVS for this check because stock
                # e1m1 lacks underwater visibility from every air leaf.
                run.command("setpos -64 1120 -240 -25 0 0\nfov 120\nr_novis 1\nr_wateralpha .35\nfog 0\nr_clouds_height 2000\nr_clouds_thickness 1500\nr_clouds_density .5\nr_clouds_coverage .4")
                summary["checks"]["bsp_transparency"] = capture(run, "bsp_transparency", terrain=False)
                run.command("r_wateralpha 1")
                summary["checks"]["bsp_opaque_water_reference"] = capture(run, "bsp_opaque_water_reference", terrain=False)
                summary["checks"]["bsp_water_alpha_difference"] = difference(
                    summary["checks"]["bsp_transparency"]["screenshot"], summary["checks"]["bsp_opaque_water_reference"]["screenshot"])
                run.command("fov 90\nr_novis 0")
        run.command("echo CLOUD_HORIZON_SMOKE_COMPLETE\nbind F12 quit")
        os.write(run.write_fd, b"k")
        run.process.wait(timeout=10)
        if run.process.returncode != 0:
            raise RuntimeError(f"client exited with status {run.process.returncode}")
        if not args.baseline:
            config = (work / "id1/vkQuake.cfg").read_text()
            archived = {}
            for name in ("r_clouds_resolution", "r_clouds_filter", "r_atmosphere_lut"):
                value = re.search(r'^' + name + r' "([^"]+)"$', config, re.MULTILINE)
                if not value:
                    raise RuntimeError(f"rendering control was not archived: {name}")
                archived[name] = value[1]
            if re.search(r'^r_sky_profile ', config, re.MULTILINE):
                raise RuntimeError("temporary GPU profiling control was archived")
            summary["checks"]["archived_controls"] = archived
        logs = run.text() + (work / "process.log").read_text(errors="replace")
        validation = sorted(set(re.findall(r"[^\n]*(?:VUID-|Validation Error)[^\n]*", logs)))
        summary["validation_messages"] = validation
        if args.validation and "Using VK_LAYER_KHRONOS_validation" not in logs:
            raise RuntimeError("requested Vulkan validation layer was not enabled")
        if validation:
            raise RuntimeError(f"Vulkan validation reported {len(validation)} distinct errors; inspect process.log")
        if args.require_speedup:
            missed = {name: result["incremental_reduction_percent"] for name, result in summary["performance_acceptance"].items()
                      if result["incremental_reduction_percent"] < 50}
            if missed:
                raise RuntimeError(f"incremental cloud reduction fell below the 50% target: {missed}")
        summary["success"] = True
    except Exception as error:
        summary["error"] = str(error)
    finally:
        if run is not None:
            run.close()
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({key: value for key, value in summary.items() if key not in ("views", "checks")}, indent=2), flush=True)
        print("views:", list(summary["views"]), "checks:", list(summary["checks"]), flush=True)
    if not summary["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
