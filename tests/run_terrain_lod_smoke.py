#!/usr/bin/env python3
"""Run isolated graphical terrain LOD checks and normal-frame timing comparisons.

Requires Linux, a graphical Vulkan session, a C compiler, SDL2 development
headers, and existing pak*.pak assets. Only the assets are linked into a fresh
temporary directory; all configurations, databases, screenshots, and logs are
written there. No worker-count override is used. A one-player listen server
avoids the engine's 16 ms background-window idle throttle during measurements.

A small SDL event wrapper injects F12 into this process to execute staged config
files. It also records intervals between input-poll batches, once per normal
host frame. These are application frame intervals, including scheduling and
presentation waits, not isolated GPU timings. Unlike timerefresh, the measured
frames use the normal renderer task path. Results are descriptive measurements,
not a hardware-independent FPS acceptance threshold.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import statistics
import subprocess
import tempfile
import time

from run_world_network_smoke import copied_engine_environment


WRAPPER = r'''
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int SDL_PollEvent(SDL_Event *event)
{
    static int (*real_poll)(SDL_Event *);
    static int controls = -1, timings = -1, keyup, batch_start = 1;
    static unsigned capture, sequence;
    static long long previous;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (!real_poll) {
        real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent");
        const char *c = getenv("QUIVER_LOD_CONTROL_FD");
        const char *t = getenv("QUIVER_LOD_TIMING_FD");
        if (!real_poll || !c || !t) _exit(125);
        controls = atoi(c); timings = atoi(t);
    }
    if (batch_start) {
        if (capture && previous) {
            char line[80];
            int n = snprintf(line, sizeof line, "%u,%lld\n", capture, now - previous);
            if (write(timings, line, n) != n) _exit(126);
        }
        previous = now;
        batch_start = 0;
    }
    if (keyup && event) {
        SDL_memset(event, 0, sizeof *event);
        event->type = SDL_KEYUP;
        event->key.state = SDL_RELEASED;
        event->key.keysym.scancode = SDL_SCANCODE_F12;
        event->key.keysym.sym = SDLK_F12;
        keyup = 0;
        return 1;
    }
    char control;
    if (read(controls, &control, 1) == 1) {
        if (control == 'b') { capture = ++sequence; previous = now; }
        if (control == 'e') capture = 0;
        if (control == 'k' && event) {
            SDL_memset(event, 0, sizeof *event);
            event->type = SDL_KEYDOWN;
            event->key.state = SDL_PRESSED;
            event->key.keysym.scancode = SDL_SCANCODE_F12;
            event->key.keysym.sym = SDLK_F12;
            keyup = 1;
            return 1;
        }
    }
    int result = real_poll(event);
    if (!result) batch_start = 1;
    return result;
}
'''


def parse_stats(text):
    render = re.search(r"Octree render: near (\d+) draws (\d+) triangles, far (\d+) draws (\d+) triangles", text)
    gpu = re.search(r"Octree GPU: ([\d.]+) MiB live\+retired, ([\d.]+) MiB retired, uploaded ([\d.]+) KiB this frame", text)
    lod = re.search(r"Terrain LOD: (\d+) selected, (\d+) cached, (\d+) pending, ([\d.]+) MiB CPU, (\d+) failed, ([\d.]+) ms total builds", text)
    if not (render and gpu and lod):
        raise ValueError("missing renderer/LOD statistics: " + text[-1500:])
    result = dict(zip(("near_draws", "near_triangles", "far_draws", "far_triangles"), map(int, render.groups())))
    result.update(gpu_mib=float(gpu[1]), retired_mib=float(gpu[2]), uploaded_kib=float(gpu[3]), selected=int(lod[1]),
                  cached=int(lod[2]), pending=int(lod[3]), cpu_mib=float(lod[4]), failed=int(lod[5]), build_ms=float(lod[6]))
    return result


class Run:
    def __init__(self, args, work):
        self.args, self.work = args, work
        self.serial = 0
        self.validation_log_offset = 0
        self.started = time.monotonic()
        self.deadline = self.started + args.timeout
        self.read_fd, self.write_fd = os.pipe()
        os.set_blocking(self.read_fd, False)
        self.timing_fd = os.open(work / "frame_intervals.csv", os.O_CREAT | os.O_WRONLY, 0o600)
        self.log = (work / "process.log").open("wb")
        environment = copied_engine_environment(args.binary)
        environment.update(LD_PRELOAD=str(work / "autokey.so"), QUIVER_LOD_CONTROL_FD=str(self.read_fd),
                           QUIVER_LOD_TIMING_FD=str(self.timing_fd))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_socket:
            port_socket.bind(("127.0.0.1", 0))
            port = port_socket.getsockname()[1]
        command = ["./vkquake", "-basedir", ".", "-userdir", "user", "-window", "-width", str(args.width),
                   "-height", str(args.height), "-listen", "1", "-port", str(port), "-nosound", "-nojoy", "-nomouse",
                   "-condebug", "+exec", "startup.cfg"]
        if getattr(args, "validation", False):
            command.insert(1, "-validation")
        self.process = subprocess.Popen(command, cwd=work, env=environment, stdin=subprocess.DEVNULL,
                                        stdout=self.log, stderr=subprocess.STDOUT,
                                        pass_fds=(self.read_fd, self.timing_fd))
        self.measurements = 0

    def text(self):
        log = self.work / "qconsole.log"
        return log.read_text(errors="replace") if log.exists() else ""

    def check(self):
        if self.process.poll() is not None:
            raise RuntimeError(f"graphical client exited unexpectedly ({self.process.returncode})")
        if time.monotonic() >= self.deadline:
            raise RuntimeError("overall graphical smoke timeout")
        text = self.text()
        if any(error in text for error in ("Host_Error:", "Sys_Error:", "runaway loop error", "Vulkan Error")):
            raise RuntimeError("engine error; inspect qconsole.log")
        if getattr(self.args, "validation", False):
            with (self.work / "process.log").open("r", errors="replace") as log:
                log.seek(self.validation_log_offset)
                recent = log.read()
                self.validation_log_offset = log.tell()
            failure = re.search(r"[^\n]*(?:VUID-|Validation Error)[^\n]*", recent)
            if failure:
                raise RuntimeError("Vulkan validation: " + failure[0][:1200])

    def wait(self, predicate, description, timeout=None):
        deadline = min(self.deadline, time.monotonic() + (timeout or self.args.stage_timeout))
        while time.monotonic() < deadline:
            self.check()
            value = predicate(self.text())
            if value:
                return value
            time.sleep(0.05)
        raise RuntimeError("timeout waiting for " + description)

    def delay(self, duration):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            self.check()
            time.sleep(min(0.05, max(0, end - time.monotonic())))

    def command(self, text):
        self.serial += 1
        begin, end = f"LOD_BEGIN_{self.serial}", f"LOD_END_{self.serial}"
        temporary = self.work / "id1/smoke_command.tmp"
        temporary.write_text(f"echo {begin}\n{text}\necho {end}\n")
        temporary.replace(self.work / "id1/smoke_command.cfg")
        os.write(self.write_fd, b"k")
        result = self.wait(lambda output: output.split(begin, 1)[1].split(end, 1)[0]
                           if begin in output and end in output else None, end)
        return result

    def map_command(self, text):
        count = self.text().count("CL_SignonReply: 4")
        self.command(text)
        self.wait(lambda output: output.count("CL_SignonReply: 4") > count, "complete map signon")

    def settle(self, expected_far, expected_near=False, idle_uploads=False):
        deadline = min(self.deadline, time.monotonic() + self.args.stage_timeout)
        stable = 0
        previous = None
        last = None
        while time.monotonic() < deadline:
            last = parse_stats(self.command("octree_render_stats"))
            if not math.isfinite(last["gpu_mib"]) or last["gpu_mib"] > 128.1:
                raise RuntimeError(f"terrain GPU budget exceeded: {last}")
            if last["cpu_mib"] > 32.01 or last["selected"] > 512 or last["cached"] > 512 or last["pending"] > 8 or last["failed"]:
                raise RuntimeError(f"terrain cache bounds/build failure: {last}")
            ready = last["pending"] == 0
            if expected_far:
                ready &= last["selected"] > 0 and last["cached"] >= last["selected"] and last["far_triangles"] > 0
            else:
                ready &= last["selected"] == 0 and last["far_draws"] == 0 and last["far_triangles"] == 0
            if expected_near:
                ready &= last["near_triangles"] > 0
            if idle_uploads:
                ready &= last["uploaded_kib"] == 0 and last["retired_mib"] == 0
            key = tuple(last[name] for name in ("selected", "cached", "pending", "near_triangles", "far_triangles"))
            stable = stable + 1 if ready and key == previous else 0
            if stable >= 2:
                return last
            previous = key
            self.delay(0.35)
        raise RuntimeError(f"terrain did not settle: {last}")

    def photograph(self, label):
        before = set((self.work / "id1").glob("*.png"))
        # Screenshot requests are asynchronous. Retry a missing request, and
        # wait for the completion message instead of copying a partial PNG.
        for _ in range(3):
            self.command("screenshot")
            deadline = min(self.deadline, time.monotonic() + 3)
            while time.monotonic() < deadline:
                self.check()
                for path in set((self.work / "id1").glob("*.png")) - before:
                    if "Wrote " + path.name in self.text():
                        target = self.work / (label + ".png")
                        shutil.copy2(path, target)
                        return str(target)
                time.sleep(.05)
        raise RuntimeError("screenshot did not complete after three requests")

    def measure(self, label):
        self.measurements += 1
        os.write(self.write_fd, b"b")
        self.delay(self.args.sample_seconds)
        os.write(self.write_fd, b"e")
        self.delay(0.15)
        rows = (self.work / "frame_intervals.csv").read_text().splitlines()
        values = [int(row.split(",")[1]) / 1_000_000 for row in rows if row.startswith(str(self.measurements) + ",")]
        if len(values) < 20 or any(not math.isfinite(value) or value <= 0 for value in values):
            raise RuntimeError(f"insufficient or invalid frame samples for {label}: {len(values)}")
        ordered = sorted(values)
        return {"frames": len(values), "mean_ms": statistics.mean(values), "median_ms": statistics.median(values),
                "p95_ms": ordered[math.ceil(len(ordered) * .95) - 1], "p99_ms": ordered[math.ceil(len(ordered) * .99) - 1],
                "aggregate_fps": 1000 / statistics.mean(values), "sample_id": self.measurements}

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        self.log.close()
        for descriptor in (self.read_fd, self.write_fd, self.timing_fd):
            os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--stage-timeout", type=float, default=30)
    parser.add_argument("--sample-seconds", type=float, default=3)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    args = parser.parse_args()
    if not (0 < args.timeout <= 600 and 0 < args.stage_timeout <= 60 and 0.25 <= args.sample_seconds <= 15):
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
    work = Path(tempfile.mkdtemp(prefix="quiver-terrain-lod-"))
    (work / "id1").mkdir()
    (work / "user").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    shutil.copy2(args.binary.resolve(), work / "vkquake")
    (work / "autokey.c").write_text(WRAPPER)
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", str(work / "autokey.c"),
                    "-o", str(work / "autokey.so"), "-ldl"], check=True, timeout=30)
    config = """vid_fullscreen 0
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
con_notifytime 0
world_view_chunks 2
r_octree_distance 4096
r_octree_mesh_mb 128
r_octree_upload_mb 8
gl_farclip 16384
r_octree_lod 0
r_octree_lod_distance 32768
bind F12 "exec smoke_command.cfg"
world_create lod_smoke terrain 4242
"""
    (work / "id1/startup.cfg").write_text(config)
    (work / "id1/vkQuake.cfg").write_text(f"vid_fullscreen 0\nvid_width {args.width}\nvid_height {args.height}\nvid_vsync 0\nvid_fsaa 0\n")
    summary = {"success": False, "work": str(work), "binary": str(args.binary.resolve()),
               "resolution": [args.width, args.height], "worker_count": "engine default; no override",
               "method": "normal host-frame input-batch intervals; warm stationary camera; clouds disabled",
               "background_throttle": "disabled by standard one-player listen mode",
               "far_geometry": "procedural backdrop; edits/caves exact only in detailed nearby chunks",
               "comparisons": {}, "checks": {}}
    run = None
    try:
        run = Run(args, work)
        run.wait(lambda text: "CL_SignonReply: 4" in text, "initial terrain signon")
        device = re.search(r"^Device: (.+)$", run.text(), re.MULTILINE)
        if device:
            summary["gpu"] = device[1]
        run.command("setpos 128 128 768 15 45 0")
        run.delay(.5)
        for label, enabled, distance in (("lod_off", 0, 32768), ("lod_32768", 1, 32768), ("lod_65536", 1, 65536)):
            run.command(f"r_octree_lod {enabled}\nr_octree_lod_distance {distance}")
            stats = run.settle(bool(enabled), True)
            measurement = run.measure(label)
            summary["comparisons"][label] = {"stats": stats, "timings": measurement, "screenshot": run.photograph(label)}
        for label, coordinates in (("negative_boundary", "-1 -1"), ("huge_coordinates", "9007199254740991 -9007199254740991"),
                                   ("return_origin", "0 0")):
            run.command(f"world_teleport {coordinates} 0 128 128 768")
            stats = run.settle(True, True)
            summary["checks"][label] = {"stats": stats, "screenshot": run.photograph(label)}
        run.command("r_octree_mesh_mb 16\nr_octree_upload_mb 1")
        # Reverse across nearby quadtree boundaries while uploads are pending,
        # then demand eventual visible progress under the smaller memory cap.
        for x, y in ((6, 0), (-2, -1), (7, 2), (-1, 1), (0, 0)):
            run.command(f"world_teleport {x} {y} 0 128 128 768")
            run.delay(.1)
        stats = run.settle(True, idle_uploads=True)
        if stats["gpu_mib"] > 16.1:
            raise RuntimeError(f"low-memory terrain did not converge under 16 MiB: {stats}")
        summary["checks"]["low_memory_churn"] = {"stats": stats, "screenshot": run.photograph("low_memory_churn")}
        run.command("r_octree_mesh_mb 128\nr_octree_upload_mb 8")
        run.map_command("world_create lod_empty empty 4242")
        summary["checks"]["empty_generator"] = run.settle(False)
        run.map_command("map __empty")
        summary["checks"]["empty_map"] = run.settle(False)
        run.map_command("world_load lod_smoke")
        run.command("setpos 128 128 768 15 45 0")
        summary["checks"]["world_reset"] = run.settle(True, True)
        run.command("r_fastsky 0\nr_atmosphere 1\nr_clouds 1\nr_clouds_speed 0")
        run.delay(2)
        summary["checks"]["atmosphere_65536"] = {"stats": run.settle(True, True),
                                                 "screenshot": run.photograph("atmosphere_65536")}
        run.command("echo LOD_SMOKE_COMPLETE\nbind F12 quit")
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
