#!/usr/bin/env python3
"""Compare single-pass clouds, isolated distance bands, and two-cascade layouts.

Uses the established horizon cameras and isolated graphical harness. Host-frame
off/on pairs are recorded with profiling disabled; GPU pass diagnostics are a
separate measurement. Near-only/far-only runs are diagnostic workloads, not an
additive decomposition: removing near clouds can remove early termination of
the far ray. All executables, configurations, captures and measurements remain
in the printed temporary directory; saved player settings are untouched.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from run_cloud_horizon_smoke import (VIEWS, capture, difference, full_image_difference, movement_checks, paired_cloud_timings,
                                    sky_stats, summarize_pairs, viewport_checks)
from run_terrain_lod_smoke import Run, WRAPPER
from run_terrain_smoothing_smoke import camera


def cases(args):
    result = [{"name": "single_before", "cascades": 0, "band": 0, "split": 4096, "near": 2, "far": 2}]
    for split in args.splits:
        if args.diagnostics:
            for band, label in ((1, "near"), (2, "far")):
                result.append({"name": f"single_{label}_{split}", "cascades": 0, "band": band,
                               "split": split, "near": 2, "far": 2})
        for layout in args.layouts:
            if args.candidates and f"{layout}@{split}" not in args.candidates:
                continue
            near, far = map(int, layout.split("/"))
            result.append({"name": f"cascade_{near}_{far}_{split}", "cascades": 1, "band": 0,
                           "split": split, "near": near, "far": far})
    result.append({"name": "single_after", "cascades": 0, "band": 0, "split": 4096, "near": 2, "far": 2})
    return result


def configure(run, case):
    run.command(f"r_sky_profile 0\nr_clouds 1\nr_clouds_resolution 2\nr_clouds_filter 0\nr_atmosphere_lut 1\n"
                f"r_clouds_cascades {case['cascades']}\nr_clouds_band {case['band']}\n"
                f"r_clouds_split {case['split']}\nr_clouds_near_resolution {case['near']}\n"
                f"r_clouds_far_resolution {case['far']}")
    run.delay(.3)
    stats = sky_stats(run)
    bands = stats.get("bands")
    if bands is None or "gpu_bands" not in stats:
        raise RuntimeError("candidate binary does not report the cascade/band diagnostics")
    if (bands["cascades"] != case["cascades"] or bands["diagnostic"] != case["band"] or
            abs(bands["split"] - case["split"]) > .1 or bands["near_divisor"] != case["near"] or
            bands["far_divisor"] != case["far"]):
        raise RuntimeError(f"cloud mode did not apply: {case}: {bands}")
    return stats


def compare(current, reference):
    old = reference["incremental_cloud_ms"]
    return {"reference": reference, "current": current,
            "incremental_reduction_percent": 100 * (1 - current["incremental_cloud_ms"] / old) if old > 0 else None,
            "clouds_off_change_percent": 100 * (current["clouds_off_ms"] / reference["clouds_off_ms"] - 1)}


def preserve_check_images(work, checks, directory):
    """Keep shared helper filenames from overwriting a different layout's images."""
    destination = work / directory
    destination.mkdir()
    relocated = {}

    def visit(value):
        if isinstance(value, dict):
            for key, item in value.items():
                value[key] = visit(item)
        elif isinstance(value, list):
            for index, item in enumerate(value):
                value[index] = visit(item)
        elif isinstance(value, str) and value.startswith(str(work) + "/") and value.endswith(".png"):
            if value not in relocated:
                target = destination / Path(value).name
                Path(value).rename(target)
                relocated[value] = str(target)
            return relocated[value]
        return value

    visit(checks)


def scene_checks(run, checks):
    run.command("fog 0\nr_clouds_height 8\nr_clouds_thickness 8\nr_clouds_density 1\nr_clouds_coverage 1")
    cloudy = capture(run, "opaque_ground_clouds", "956 956 190 65 45 0")
    run.command("r_clouds 0")
    clear = capture(run, "opaque_ground_clear")
    delta = difference(cloudy["screenshot"], clear["screenshot"])
    checks["opaque_foreground"] = {"clouds": cloudy, "clear": clear, "rgb_mean_difference": delta}
    if delta > .01:
        raise RuntimeError(f"cascade clouds leaked through foreground terrain: {delta}")
    run.command("r_clouds 1\nr_clouds_height 2000\nr_clouds_thickness 1500\nr_clouds_density 1\nr_clouds_coverage 1")
    cloudy = capture(run, "above_dense_ground_clouds", "128 128 5000 65 45 0")
    run.command("r_clouds 0")
    clear = capture(run, "above_dense_ground_clear")
    delta = difference(cloudy["screenshot"], clear["screenshot"])
    checks["clouds_in_front_of_ground"] = {"clouds": cloudy, "clear": clear, "rgb_mean_difference": delta}
    if delta < 1:
        raise RuntimeError(f"near cascade clouds did not obscure ground: {delta}")
    run.command("r_clouds 1\nr_clouds_density .5\nr_clouds_coverage .4")
    run.map_command("map e1m1")
    run.command("setpos 640 960 300 -30 90 0\nr_drawviewmodel 1\nviewsize 120")
    checks["bsp"] = capture(run, "bsp", terrain=False)
    run.command("vid_fsaa 4\nvid_restart")
    checks["bsp_msaa"] = capture(run, "bsp_msaa", terrain=False)
    run.command("r_clouds_height 280\nr_clouds_thickness 350\nr_clouds_density .08\nr_clouds_coverage .5")
    checks["bsp_roof_intersection"] = capture(run, "bsp_roof_intersection", terrain=False)
    run.command("setpos -64 1120 -240 -25 0 0\nfov 120\nr_novis 1\nr_wateralpha .35\nfog 0\n"
                "r_clouds_height 2000\nr_clouds_thickness 1500\nr_clouds_density .5\nr_clouds_coverage .4")
    checks["bsp_transparency"] = capture(run, "bsp_transparency", terrain=False)
    run.command("r_wateralpha 1")
    checks["bsp_opaque_water"] = capture(run, "bsp_opaque_water", terrain=False)
    delta = difference(checks["bsp_transparency"]["screenshot"], checks["bsp_opaque_water"]["screenshot"])
    checks["bsp_water_alpha_difference"] = delta
    if delta < 1:
        raise RuntimeError(f"BSP water check did not exercise visible transparent water: {delta}")


def prepare(args):
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        raise ValueError("basedir contains no pak*.pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-cloud-cascades-"))
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
r_atmosphere_lut 1
r_clouds 1
r_clouds_resolution 2
r_clouds_filter 0
r_clouds_speed 0
r_sky_profile 0
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
world_create cascade_smoke terrain 4242
""")
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--reference", type=Path, help="successful preserved single-pass horizon summary at the same resolution")
    parser.add_argument("--views", nargs="+", choices=[name for name, _ in VIEWS], default=[name for name, _ in VIEWS])
    parser.add_argument("--layouts", nargs="+", choices=("2/2", "2/4", "4/2"), default=["2/2", "2/4", "4/2"])
    parser.add_argument("--splits", nargs="+", type=int, default=[4096, 8192, 16384])
    parser.add_argument("--candidates", nargs="+", help="selected layout@split pairs, such as 2/2@4096 2/4@4096 4/2@16384; replaces the cross product")
    parser.add_argument("--diagnostics", action="store_true", help="also measure isolated near/far single-pass rays at each split")
    parser.add_argument("--pairs", type=int, default=1)
    parser.add_argument("--sample-seconds", type=float, default=1)
    parser.add_argument("--validation", action="store_true", help="debug Vulkan checks; omit host-frame performance measurements")
    parser.add_argument("--lifecycle", action="store_true", help="also exercise every requested cascade layout under odd sizes, MSAA, scales and restarts")
    parser.add_argument("--motion", action="store_true", help="also capture wind, rotation, travel and phase wraps per layout at the first split")
    parser.add_argument("--linear", action="store_true", help="also capture linear-filtered clouds per requested layout")
    parser.add_argument("--scene-checks", action="store_true", help="for one selected cascade case, check terrain depth and BSP sky/roof/water/viewmodels")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--stage-timeout", type=float, default=60)
    args = parser.parse_args()
    if args.candidates:
        if any(not re.fullmatch(r"(?:2/2|2/4|4/2)@[0-9]+", candidate) for candidate in args.candidates):
            parser.error("candidates must have the form 2/2@4096, 2/4@4096 or 4/2@16384")
        args.candidates = [f"{candidate.split('@')[0]}@{int(candidate.split('@')[1])}" for candidate in args.candidates]
        if len(set(args.candidates)) != len(args.candidates):
            parser.error("duplicate candidates are not useful")
        args.layouts = list(dict.fromkeys(candidate.split("@")[0] for candidate in args.candidates))
        args.splits = list(dict.fromkeys(int(candidate.split("@")[1]) for candidate in args.candidates))
    if not (1 <= args.pairs <= 5 and .25 <= args.sample_seconds <= 15 and 0 < args.timeout <= 3600 and 0 < args.stage_timeout <= 60):
        parser.error("pairs must be 1..5; sample/timeouts must be positive and bounded (15/3600/60 seconds)")
    if not args.splits or any(split < 256 or split > 131072 for split in args.splits):
        parser.error("splits must be 256..131072 world units")
    if not (320 <= args.width <= 3840 and 240 <= args.height <= 2160):
        parser.error("resolution must be 320x240..3840x2160")
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        parser.error("a graphical session is required")
    if len(set(args.splits)) != len(args.splits) or len(set(args.layouts)) != len(args.layouts):
        parser.error("duplicate splits/layouts are not useful")
    check_cases = [case for case in cases(args) if case["cascades"] and (args.candidates or case["split"] == args.splits[0])]
    if args.scene_checks and len(check_cases) != 1:
        parser.error("--scene-checks needs one selected layout/split or candidate")
    reference = json.loads(args.reference.read_text()) if args.reference else None
    if reference and (not reference.get("success") or reference.get("resolution") != [args.width, args.height]):
        parser.error("reference must be successful at the same resolution")
    if reference and any(not reference.get("views", {}).get(name, {}).get("paired_summary") for name in args.views):
        parser.error("reference lacks paired timing summaries for requested views")
    work = prepare(args)
    print(f"work: {work}", flush=True)
    summary = {"success": False, "work": str(work), "binary": str(args.binary.resolve()),
               "preserved_binary": str(work / "vkquake"), "binary_sha256": hashlib.sha256((work / "vkquake").read_bytes()).hexdigest(),
               "resolution": [args.width, args.height], "validation": args.validation, "pairs": args.pairs,
               "sample_seconds": args.sample_seconds, "reference": str(args.reference.resolve()) if args.reference else None,
               "method": ("graphical correctness checks; host-frame timings disabled under Vulkan validation" if args.validation else
                          "profiling-off paired host-frame intervals; separate completed-frame GPU pass diagnostics"),
               "visual_review_required": True, "views": {}, "lifecycle": {}, "motion": {}, "linear": {}, "scenes": {},
               "cascade_diagnostics": {}, "warnings": []}
    run = None
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "initial terrain signon")
        device = re.search(r"^Device: (.+)$", run.text(), re.MULTILINE)
        if device:
            summary["gpu"] = device[1]
        for view, viewpoint in VIEWS:
            if view not in args.views:
                continue
            camera(run, viewpoint)
            results = summary["views"][view] = {}
            for case in cases(args):
                configure(run, case)
                label = f"{view}_{case['name']}"
                result = capture(run, label)
                result["case"] = case
                if reference and Path(reference["views"][view]["screenshot"]).is_file():
                    result["rgb_difference_from_preserved"] = full_image_difference(
                        reference["views"][view]["screenshot"], result["screenshot"])
                if not args.validation:
                    result["paired_cloud_timings"] = paired_cloud_timings(run, label)
                    result["paired_summary"] = summarize_pairs(result["paired_cloud_timings"])
                    if reference:
                        result["against_preserved"] = compare(result["paired_summary"], reference["views"][view]["paired_summary"])
                    if results:
                        result["against_single_before"] = compare(result["paired_summary"], results["single_before"]["paired_summary"])
                result["sky_gpu_diagnostics"] = sky_stats(run, profiling=True)
                if results:
                    result["rgb_difference_from_single"] = full_image_difference(
                        results["single_before"]["screenshot"], result["screenshot"])
                results[case["name"]] = result
                (work / "summary.partial.json").write_text(json.dumps(summary, indent=2) + "\n")
                print(f"{view}: {case['name']} complete", flush=True)
            if not args.validation:
                drift = results["single_after"]["against_single_before"]
                if abs(drift["incremental_reduction_percent"]) > 5 or abs(drift["clouds_off_change_percent"]) > 5:
                    summary["warnings"].append(f"{view}: single-pass before/after drift exceeds 5%; repeat before attributing small differences")
            if results["single_after"]["rgb_difference_from_single"] > .01:
                raise RuntimeError(f"{view}: returning to single-pass changed stopped-wind pixels")
        if args.lifecycle or args.motion or args.linear:
            for case in check_cases:
                near, far, split = case["near"], case["far"], case["split"]
                layout = f"{near}/{far}@{split}"
                camera(run, VIEWS[3][1])
                configure(run, case)
                if args.validation:
                    checks = summary["cascade_diagnostics"][layout] = {}
                    for band, name in ((1, "near"), (2, "far")):
                        configure(run, {**case, "band": band})
                        checks[name] = capture(run, f"cascade_only_{name}")
                        checks[name]["sky"] = sky_stats(run)
                    preserve_check_images(work, checks, f"bands_{near}_{far}_{split}")
                    configure(run, case)
                if args.lifecycle:
                    checks = summary["lifecycle"][layout] = {}
                    viewport_checks(run, checks)
                    for check in checks.values():
                        stats = check.get("sky", check.get("after")) if isinstance(check, dict) else None
                        if stats:
                            bands = stats.get("bands", {})
                            if bands.get("cascades") != 1 or bands.get("near_divisor") != near or bands.get("far_divisor") != far:
                                raise RuntimeError(f"cascade layout was lost across a viewport/restart: {layout}: {stats}")
                    preserve_check_images(work, checks, f"lifecycle_{near}_{far}_{split}")
                if args.linear:
                    checks = summary["linear"][layout] = {}
                    checks["nearest"] = capture(run, "linear_reference_nearest")
                    run.command("r_clouds_filter 1")
                    checks["linear"] = capture(run, "linear_filter")
                    checks["sky"] = sky_stats(run)
                    delta = full_image_difference(checks["nearest"]["screenshot"], checks["linear"]["screenshot"])
                    checks["rgb_mean_difference"] = delta
                    if delta <= .01 or checks["sky"]["clouds"]["filter"] != "linear":
                        raise RuntimeError(f"linear cascade filtering was not applied: {layout}")
                    run.command("r_clouds_filter 0")
                    preserve_check_images(work, checks, f"linear_{near}_{far}_{split}")
                if args.motion:
                    checks = summary["motion"][layout] = {}
                    movement_checks(run, checks)
                    # Approach a fixed point in the cloud layer across the
                    # split radius. The split is camera-relative, so nearby
                    # world features cross it as the camera travels forward.
                    for name, offset in (("far", 256), ("at", 0), ("near", -256)):
                        distance = case["split"] + offset
                        coordinate = 8192 - distance / math.sqrt(2)
                        label = f"split_approach_{name}"
                        checks[label] = capture(run, label, f"{coordinate:.6f} {coordinate:.6f} 2600 0 45 0")
                    preserve_check_images(work, checks, f"motion_{near}_{far}_{split}")
        if args.scene_checks:
            configure(run, check_cases[0])
            scene_checks(run, summary["scenes"])
            preserve_check_images(work, summary["scenes"], "scenes")
        run.command("echo CLOUD_CASCADE_BENCHMARK_COMPLETE\nbind F12 quit")
        os.write(run.write_fd, b"k")
        run.process.wait(timeout=10)
        if run.process.returncode:
            raise RuntimeError(f"client exited with status {run.process.returncode}")
        config = (work / "id1/vkQuake.cfg").read_text()
        controls = ("r_clouds_cascades", "r_clouds_near_resolution", "r_clouds_far_resolution", "r_clouds_split", "r_clouds_band")
        if any(re.search(r'^' + name + ' ', config, re.MULTILINE) for name in controls):
            raise RuntimeError("experimental cascade controls were archived")
        logs = run.text() + (work / "process.log").read_text(errors="replace")
        summary["validation_messages"] = sorted(set(re.findall(r"[^\n]*(?:VUID-|Validation Error)[^\n]*", logs)))
        if args.validation and "Using VK_LAYER_KHRONOS_validation" not in logs:
            raise RuntimeError("requested Vulkan validation layer was not enabled")
        if summary["validation_messages"]:
            raise RuntimeError("Vulkan validation errors; inspect process.log")
        summary["success"] = True
    except Exception as error:
        summary["error"] = str(error)
    finally:
        if run:
            run.close()
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({key: value for key, value in summary.items()
                          if key not in ("views", "lifecycle", "motion", "linear", "scenes", "cascade_diagnostics")}, indent=2), flush=True)
    if not summary["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
