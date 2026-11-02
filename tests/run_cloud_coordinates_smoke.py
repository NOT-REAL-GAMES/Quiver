#!/usr/bin/env python3
"""Check frozen cloud images across local rebases and both wrapped planet seams.

Uses isolated assets/configuration. --baseline records the old discontinuity;
normal runs require small pixel differences across two-unit planet seams.
The periodic-wrap controls use four-unit displacements with the same limit.
No frame timing is collected, so these checks can run alongside CPU regressions.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageStat
from run_terrain_lod_smoke import Run, WRAPPER


def difference(first, second):
    with Image.open(first) as a, Image.open(second) as b:
        delta = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
        return sum(ImageStat.Stat(delta).mean) / 3


def camera(run, x, y):
    # World positions wrap but setpos addresses the current execution frame.
    # Move the camera and cloud layer together above the planet's relief. The
    # world switch now hides the sky too, so leave it enabled for this test.
    chunks = [math.floor(x / 1024), math.floor(y / 1024), 64]
    local = [x - chunks[0] * 1024, y - chunks[1] * 1024, 256]
    run.command("world_teleport " + " ".join(map(str, chunks + local)) +
                "\nwait\nsetpos " + " ".join(map(str, local)) + " -40 45 0\nnoclip 1")
    run.delay(.3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--global-cache", type=Path)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--timeout", type=float, default=360)
    parser.add_argument("--stage-timeout", type=float, default=120)
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    args = parser.parse_args()
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("no pak*.pak assets found")
    work = Path(tempfile.mkdtemp(prefix="quiver-cloud-coordinates-"))
    (work / "id1/worlds").mkdir(parents=True)
    (work / "user").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset)
    if args.global_cache:
        shutil.copy2(args.global_cache, work / "id1/worlds/cloud_coordinates.sqlite3.planetcache")
    shutil.copy2(args.binary.resolve(), work / "vkquake")
    (work / "autokey.c").write_text(WRAPPER)
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", str(work / "autokey.c"), "-o", str(work / "autokey.so"), "-ldl"], check=True)
    (work / "id1/startup.cfg").write_text("""vid_fullscreen 0
vid_vsync 0
vid_fsaa 0
host_maxfps 60
r_tasks 1
r_fastsky 0
r_atmosphere 1
r_clouds 1
r_clouds_speed 0
r_clouds_scale 0.000075
r_drawviewmodel 0
r_drawworld 1
r_clouds_height 67536
viewsize 130
cl_nocsqc 1
cl_bob 0
cl_rollangle 0
developer 1
scr_conspeed 100000
con_notifytime -1
world_view_chunks 1
r_octree_distance 1024
r_octree_lod_distance 4096
r_sun_angle 35
r_sun_azimuth 0
bind F12 "exec smoke_command.cfg"
world_create cloud_coordinates planet 1
""")
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\n")
    summary = {"success": False, "work": str(work), "baseline": args.baseline, "checks": {}}
    run = None
    print(work, flush=True)
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "planet signon")
        # Isolate the sky from terrain at the distant control locations.
        # The default descriptor wraps at this many chunks; only two world
        # units separate each seam pair.
        length = 1250048 * 1024
        pairs = {"local_rebase": ((1023, 128), (1025, 128)),
                 "x_seam": ((length - 1, 128), (1, 128)),
                 "y_seam": ((128, length - 1), (128, 1)),
                 "phase_wrap": ((66665, 128), (66669, 128)),
                 "full_period": ((133331, 128), (133335, 128))}
        for divisor, filtering, cascades in ((2, 0, 0), (1, 0, 0), (2, 1, 0), (2, 0, 1)):
            run.command(f"r_clouds_resolution {divisor}\nr_clouds_filter {filtering}\nr_clouds_cascades {cascades}")
            for name, positions in pairs.items():
                label = f"{name}_r{divisor}_f{filtering}_c{cascades}"
                images = []
                for index, position in enumerate(positions):
                    camera(run, *position)
                    images.append(run.photograph(f"{label}_{index}"))
                delta = difference(*images)
                limit = 3
                summary["checks"][label] = {"images": images, "mean_rgb_difference": delta, "limit": limit}
                if not args.baseline and delta > limit:
                    raise RuntimeError(f"clouds shifted at {label}: mean RGB difference {delta:.3f}")
        camera(run, 128, 128)
        first = run.photograph("stationary_first")
        run.delay(.3)
        second = run.photograph("stationary_second")
        summary["stationary_difference"] = difference(first, second)
        if summary["stationary_difference"] > .01:
            raise RuntimeError("stationary cloud field is unstable")
        run.command("r_clouds 0\nwait\nwait")
        summary["visible_cloud_difference"] = difference(second, run.photograph("clouds_off"))
        if summary["visible_cloud_difference"] < 1.0:
            raise RuntimeError("cloud-coordinate captures do not contain visible clouds")
        run.command("echo CLOUD_COORDINATES_COMPLETE\nbind F12 quit")
        os.write(run.write_fd, b"k")
        run.process.wait(timeout=10)
        if run.process.returncode:
            raise RuntimeError(f"engine exited {run.process.returncode}")
        summary["success"] = True
    except Exception as error:
        summary["error"] = str(error)
    finally:
        if run:
            run.close()
        if args.validation:
            text = (work / "process.log").read_text(errors="replace")
            summary["validation_active"] = "Using VK_LAYER_KHRONOS_validation" in text
            summary["validation_errors"] = text.count("VUID-") + text.count("Validation Error")
            if not summary["validation_active"] or summary["validation_errors"]:
                summary["success"] = False
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({key: value for key, value in summary.items() if key != "checks"}), flush=True)
    if not summary["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
