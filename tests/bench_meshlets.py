#!/usr/bin/env python3
"""Compare Quake's stock indirect BSP path with meshlets on the same executable.

Linux + SDL2/3 development headers, a Vulkan display, Pillow, and local Quake
assets are required. Link only pak files into a disposable directory. The SDL
shim injects an F12 press into this process to run commands; it does not modify
the renderer. Stock id1 ships demo1, demo2, demo3. Timedemos are whole-engine
throughput, not isolated GPU or culling timings.
"""
import argparse
import hashlib
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

from PIL import Image, ImageChops, ImageStat


SHIM = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef USE_SDL3
#include <SDL3/SDL.h>
typedef bool poll_result;
#define DOWN SDL_EVENT_KEY_DOWN
#define UP SDL_EVENT_KEY_UP
#else
#include <SDL2/SDL.h>
typedef int poll_result;
#define DOWN SDL_KEYDOWN
#define UP SDL_KEYUP
#endif
poll_result SDL_PollEvent(SDL_Event *event) {
    static poll_result (*real_poll)(SDL_Event *);
    static int fd = -1, release;
    if (!real_poll) {
        real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent");
        fd = atoi(getenv("BENCH_KEY_FD"));
    }
    char signal;
    if (event && (release || read(fd, &signal, 1) == 1)) {
        SDL_zero(*event);
        event->type = release ? UP : DOWN;
#ifdef USE_SDL3
        event->key.scancode = SDL_SCANCODE_F12;
        event->key.key = SDLK_F12;
        event->key.down = !release;
#else
        event->key.keysym.scancode = SDL_SCANCODE_F12;
        event->key.keysym.sym = SDLK_F12;
        event->key.state = release ? SDL_RELEASED : SDL_PRESSED;
#endif
        release = !release;
        return 1;
    }
    return real_poll(event);
}
"""

RESULT = re.compile(r"(\d+) frames +([\d.]+) seconds +([\d.]+) fps")
STATE = re.compile(r"Meshlet renderer: active (\d+) requested (\d+) indirect (\d+)")
VISIBILITY = re.compile(r"Meshlet visibility: (stock|hybrid|independent) occlusion (\d+) occluders (\d+)")
MESH_SHADER = re.compile(r"Mesh shader: active (\d+) eligible (\d+)")
TASK_SHADER = re.compile(r"Task shader: active (\d+)")
FRUSTUM = 'frustum'  # Benchmark labels, not separate engine modes.
MESH_FRUSTUM = 'mesh_frustum'
TASK_MESH = 'task_mesh'
TASK_FRUSTUM = 'task_frustum'


def configure_backend(mode):
    if mode == FRUSTUM:
        return 'r_meshlets 2\nr_meshlet_occlusion 0'
    if mode in (MESH_FRUSTUM, TASK_FRUSTUM):
        return f'r_meshlets 3\nr_meshlet_occlusion 0\nr_meshlet_task {int(mode == TASK_FRUSTUM)}'
    if mode == 2:
        return 'r_meshlets 2\nr_meshlet_occlusion 1'
    if mode in (3, TASK_MESH):
        return f'r_meshlets 3\nr_meshlet_occlusion 1\nr_meshlet_task {int(mode == TASK_MESH)}'
    return f'r_meshlets {mode}'


def verify_backend(text, mode):
    actual = 2 if mode == FRUSTUM else (3 if mode in (MESH_FRUSTUM, TASK_MESH, TASK_FRUSTUM) else mode)
    state = STATE.search(text)
    if not state or int(state[3]) != 1 or bool(int(state[1])) != bool(actual) or int(state[2]) != actual:
        raise RuntimeError(f"wrong renderer for mode {mode}: {text}")
    visibility = VISIBILITY.search(text)
    if actual in (2, 3) or visibility:
        if not visibility or visibility[1] != ('stock', 'hybrid', 'independent', 'independent')[actual]:
            raise RuntimeError(f"wrong visibility path for mode {mode}: {text}")
        if actual in (2, 3) and int(visibility[2]) != int(mode not in (FRUSTUM, MESH_FRUSTUM, TASK_FRUSTUM)):
            raise RuntimeError(f"unexpected Hi-Z activation for {mode}: {text}")
        if actual == 3:
            mesh = MESH_SHADER.search(text)
            if not mesh or int(mesh[1]) != 1 or int(mesh[2]) == 0:
                raise RuntimeError(f"mesh shader path inactive: {text}")
            task = TASK_SHADER.search(text)
            expected = int(mode in (TASK_MESH, TASK_FRUSTUM))
            if mode in (TASK_MESH, TASK_FRUSTUM) and (not task or int(task[1]) != expected):
                raise RuntimeError(f"task shader path inactive: {text}")
            if task and int(task[1]) != expected:
                raise RuntimeError(f"unexpected task shader path: {text}")


def balanced_order(modes, index):
    if len(modes) <= 2:
        return list(reversed(modes)) if index % 2 else list(modes)
    if len(modes) == 4:
        # Williams design: each ordering position and predecessor is balanced.
        return [modes[(index + off) % 4] for off in (0, 1, 3, 2)]
    # Williams order balances every immediate predecessor over 2*n rounds.
    n = len(modes)
    base = [0]
    for i in range(1, n):
        base.append((i + 1) // 2 if i % 2 else n - i // 2)
    row = [modes[(item + index // 2) % n] for item in base]
    return row[::-1] if index % 2 else row


def paired_changes(pairs, modes, metric, reference=0):
    out = {}
    for mode in modes:
        if mode == reference:
            continue
        values = [100 * (p[mode][metric] / p[reference][metric] - 1) for p in pairs]
        out[mode] = dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
    return out
VIEWS = (("start", "544 288 32 0 90 0"), ("e1m1", "480 -352 88 0 90 0"),
         ("e1m1", "1312 880 -248 0 90 0"), ("e1m2", "-416 -144 320 0 90 0"))
BOOT = """vid_fullscreen 0
vid_vsync 0
vid_fsaa 0
host_maxfps 0
r_tasks 1
r_parallelmark 1
r_indirect 1
r_gpulightmapupdate 1
scr_speeds 0
cl_startdemos 0
cl_startmenu 0
cl_nocsqc 1
developer 1
con_notifytime -1
scr_conspeed 99999
bind F12 "exec control.cfg"
map start
"""


class Engine:
    def __init__(self, args, work):
        self.work, self.args = work, args
        flags = subprocess.check_output(["ldd", str(args.binary)], text=True)
        sdl3 = "libSDL3" in flags
        cflags = subprocess.check_output(["pkg-config", "--cflags", "sdl3" if sdl3 else "sdl2"], text=True).split()
        (work / "key.c").write_text(SHIM)
        subprocess.run(["cc", "-O2", "-shared", "-fPIC", *cflags, *(["-DUSE_SDL3"] if sdl3 else []),
                        str(work / "key.c"), "-o", str(work / "key.so"), "-ldl"], check=True)
        self.read_fd, self.key = os.pipe()
        os.set_blocking(self.read_fd, False)
        self.env = dict(os.environ, LD_PRELOAD=str(work / "key.so"), BENCH_KEY_FD=str(self.read_fd),
                        XDG_DATA_HOME=str(work / "xdg"), XDG_CONFIG_HOME=str(work / "config"))
        self.log = (work / "output.log").open("wb")
        self.serial = 0
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_socket:
            port_socket.bind(("127.0.0.1", 0))
            port = port_socket.getsockname()[1]
        # vkQuake caps the reconstructed command line used by stuffcmds.
        # Use paths relative to the isolated cwd so +exec boot.cfg isn't cut.
        self.process = subprocess.Popen([str(args.binary), "-basedir", ".", "-userdir", "user",
                                         "-window", "-width", str(args.width), "-height", str(args.height),
                                         "-nosound", "-nojoy", "-nomouse", "-listen", "1", "-port", str(port), "-condebug",
                                         *(["-game", args.game] if getattr(args, "game", None) else []),
                                         *(["-validation", "2"] if args.validation else []), "+exec", "boot.cfg"],
                                        cwd=work, env=self.env, stdout=self.log, stderr=subprocess.STDOUT,
                                        pass_fds=(self.read_fd,), stdin=subprocess.DEVNULL)
        # SDL_GetPrefPath(vkQuake) owns the log in this vkQuake revision.
        self.path = work / "xdg/vkQuake/qconsole.log"

    def text(self):
        return self.path.read_text(errors="replace") if self.path.exists() else ""

    def wait(self, predicate, start=0):
        deadline = time.monotonic() + self.args.timeout
        while time.monotonic() < deadline:
            text = self.text()[start:]
            value = predicate(text)
            if value:
                return value
            if self.process.poll() is not None:
                raise RuntimeError(f"engine exited ({self.process.returncode}); inspect {self.work}")
            time.sleep(.025)
        raise RuntimeError("engine timeout; last log lines: " + self.text()[-500:])

    def cmd(self, body):
        self.serial += 1
        marker = f"MESHLET_CMD_{self.serial}_DONE"
        start = len(self.text())
        tmp = self.work / "id1/control.tmp"
        tmp.write_text(body + "\necho " + marker + "\n")
        tmp.replace(self.work / "id1/control.cfg")
        os.write(self.key, b"k")
        return self.wait(lambda t: t[:t.find(marker)] or " " if marker in t else None, start)

    def backend(self, mode, verify=True):
        if self.args.stock_only:
            return "unmodified stock executable"
        result = self.cmd(configure_backend(mode) + "\nwait\nwait\nmeshlet_stats")
        if verify:
            verify_backend(result, mode)
        return result.strip()

    def demo(self, name, mode, warmup=False):
        # Confirm the live renderer during untimed warmup. Do not synchronize
        # or print renderer stats in the measured playback runs.
        start = len(self.text())
        command = ("" if self.args.stock_only else configure_backend(mode) + "\n") + f"timedemo {name}"
        if warmup and not self.args.stock_only:
            command += "\nwait" * 4 + "\nmeshlet_stats"
        stats = self.cmd(command)
        if warmup and not self.args.stock_only:
            verify_backend(stats, mode)
        result = self.wait(lambda t: RESULT.search(t), start)
        frames, seconds, fps = result.groups()
        print(f"{name} mode {mode}: {frames} frames, {fps} fps", flush=True)
        data = dict(frames=int(frames), seconds=float(seconds), fps=float(fps), frame_ms=1000 / float(fps))
        if warmup and not self.args.stock_only:
            data['renderer'] = [line for line in stats.splitlines() if line.startswith('Meshlet ')]
        if mode and 'using stock renderer' in self.text()[start:]:
            raise RuntimeError(f'{name} fell back to stock')
        return data

    def view(self, name, pose):
        start = len(self.text())
        self.cmd(f"host_framerate 0\nmap {name}")
        self.wait(lambda t: "CL_SignonReply: 4" in t, start)
        self.cmd(f"host_maxfps 60\nviewsize 130\nr_drawviewmodel 0\ncrosshair 0\ncl_bob 0\ncl_rollangle 0\nsetpos {pose}")
        time.sleep(.4)
        self.cmd("pause")
        time.sleep(.2)

    def photo(self, label):
        start = len(self.text())
        for _ in range(3):
            self.cmd("screenshot png")
            for _ in range(80):
                match = re.search(r"Wrote [\x01\x02]?([^\n]+\.png)", self.text()[start:])
                if match:
                    p = next(self.work.rglob(match[1]), None)
                    if p:
                        target = self.work / (label + ".png")
                        shutil.copy2(p, target)
                        return target
                time.sleep(.03)
        raise RuntimeError("screenshot did not finish")

    def record_route(self, index, name, poses):
        # Record ordinary network demo frames once and replay that exact file
        # for both paths. Turn and walk in a small circle at each supplied pose.
        route = f"meshlet_route{index}"
        self.view(name, poses[0])
        self.cmd(f"pause\nhost_maxfps 72\nr_meshlets 0\ncl_forwardspeed 100\ncl_yawspeed 60\nrecord {route}")
        for pose in poses:
            self.cmd(f"setpos {pose}\n+left\n+forward")
            time.sleep(6)
            self.cmd("-left\n-forward")
        self.cmd("stop\nhost_maxfps 0")
        file = next(self.work.rglob(route + ".dem"), None)
        if not file or file.stat().st_size < 4096:
            raise RuntimeError(f"could not record route for {name}")
        return route, hashlib.sha256(file.read_bytes()).hexdigest()

    def close(self):
        if self.process.poll() is None:
            try:
                self.cmd("bind F12 quit")
                os.write(self.key, b"k")
                self.process.wait(timeout=10)
            except Exception:
                self.process.kill()
                self.process.wait()
        os.close(self.key)
        os.close(self.read_fd)
        self.log.close()


def delta(a, b):
    with Image.open(a) as first, Image.open(b) as second:
        diff = ImageChops.difference(first.convert("RGB"), second.convert("RGB"))
        channels = diff.split()
        mx = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
        histogram = mx.histogram()
        return dict(mean_rgb=sum(ImageStat.Stat(diff).mean) / 3,
                    percent_over_4=100 * sum(histogram[5:]) / sum(histogram),
                    max_channel=max(mx.getextrema()))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--basedir", required=True, type=Path, help="game folder or id1 directory (case insensitive pak filenames)")
    ap.add_argument("--demos", nargs="+", default=["demo1", "demo2", "demo3"])
    ap.add_argument("--modes", nargs="+", type=int, choices=(0, 1, 2, 3), default=[0, 1],
                    help="compare selected paths; 0 stock BSP, 1 hybrid meshlets, 2 independent GPU occlusion, 3 mesh shaders")
    ap.add_argument("--frustum-control", action="store_true",
                    help="also time mode 2 with r_meshlet_occlusion 0 and compare it directly against Hi-Z")
    ap.add_argument("--mesh-frustum-control", action="store_true",
                    help="also time mode 3 with r_meshlet_occlusion 0 and compare it directly against Hi-Z")
    ap.add_argument("--task-control", action="store_true", help="also benchmark mode 3 with task shader culling")
    ap.add_argument("--task-frustum-control", action="store_true", help="also benchmark task shaders without depth occlusion")
    ap.add_argument("--game", help="optional sibling mod folder (e.g. mg1 under the rerelease basedir)")
    ap.add_argument("--view", nargs=7, action="append", metavar=("MAP", "X", "Y", "Z", "PITCH", "YAW", "ROLL"),
                    help="replace default fixed views; may be repeated")
    ap.add_argument("--record-views", action="store_true", help="record a moving camera demo on each --view map instead of using bundled demos")
    ap.add_argument("--demo-files", type=Path, nargs="+", help="replay these local .dem files instead of bundled demos")
    ap.add_argument("--pairs", type=int, default=6)
    ap.add_argument("--stock-only", action="store_true", help="baseline/preflight with an unmodified stock executable")
    ap.add_argument("--visual-only", action="store_true")
    ap.add_argument("--gpu-seconds", type=float, default=0, help="separate GPU timestamp sample duration per window (normal unpaused rendering)")
    ap.add_argument("--gpu-pairs", type=int, default=4, help="alternating stock/meshlet window pairs per fixed camera")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--validation", action="store_true", help="visual verification only (requires a debug build and layers)")
    args = ap.parse_args()
    if 0 not in args.modes or len(set(args.modes)) != len(args.modes):
        ap.error("--modes must include stock 0 and cannot contain duplicates")
    if args.frustum_control and (2 not in args.modes or args.stock_only):
        ap.error("--frustum-control needs mode 2")
    if args.mesh_frustum_control and (3 not in args.modes or args.stock_only):
        ap.error("--mesh-frustum-control needs mode 3")
    if (args.task_control or args.task_frustum_control) and (3 not in args.modes or args.stock_only):
        ap.error("task controls need mode 3")
    if args.validation and not args.visual_only:
        ap.error("do not mix validation instrumentation with throughput measurements")
    if args.pairs < 1 or args.gpu_pairs < 1 or args.gpu_seconds < 0 or args.gpu_seconds and (args.stock_only or args.validation):
        ap.error("invalid measurement parameters")
    if any(not re.fullmatch(r"[A-Za-z0-9_.-]+", n) for n in args.demos):
        ap.error("invalid demo name")
    views = VIEWS
    if args.view:
        views = []
        for name, *coords in args.view:
            if not re.fullmatch(r"[A-Za-z0-9_]+", name):
                ap.error("invalid map name")
            try:
                if not all(math.isfinite(float(c)) for c in coords):
                    raise ValueError
            except ValueError:
                ap.error("invalid view coordinates")
            views.append((name, " ".join(coords)))
    if args.game and not re.fullmatch(r"[A-Za-z0-9_-]+", args.game):
        ap.error("invalid game directory")
    if args.demo_files and args.record_views:
        ap.error("choose prerecorded demos or recording new demos")
    args.binary = args.binary.resolve()
    basedir = args.basedir.resolve()
    source = next((p for p in basedir.iterdir() if p.is_dir() and p.name.lower() == "id1"), basedir)
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r"pak\d+\.pak", p.name.lower()))
    if not paks:
        ap.error(f"no pak files at {source}")
    work = Path(tempfile.mkdtemp(prefix="quiver-meshlet-bench-"))
    print(work, flush=True)
    (work / "id1").mkdir()
    (work / "user").mkdir()
    for pak in paks:
        (work / "id1" / pak.name.lower()).symlink_to(pak)
    if args.game:
        mod = source.parent / args.game
        gamepaks = sorted(p for p in mod.iterdir() if re.fullmatch(r"pak\d+\.pak", p.name.lower())) if mod.is_dir() else []
        if not gamepaks:
            ap.error(f"no mod paks at {mod}")
        (work / args.game).mkdir()
        for p in gamepaks:
            (work / args.game / p.name.lower()).symlink_to(p)
        paks += gamepaks
    (work / "id1/boot.cfg").write_text(BOOT.replace("map start", f"map {views[0][0]}"))
    result = dict(success=False, work=str(work), binary=str(args.binary), binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  assets=[str(p) for p in paks], resolution=[args.width,args.height], stock_only=args.stock_only,
                  timedemos={}, visual={}, gpu={})
    result["settings"] = dict(r_indirect=1, r_gpulightmapupdate=1, r_tasks=1, scr_speeds=0,
                              host_pairs=args.pairs, gpu_pairs=args.gpu_pairs, gpu_window_seconds=args.gpu_seconds,
                              gpu_scope="whole rendered frames, completed timestamps; camera fixed, game unpaused")
    if args.demo_files:
        args.demos, result["imported"] = [], {}
        for i, file in enumerate(args.demo_files):
            demo = f"replay{i}"
            if not file.is_file() or file.suffix.lower() != ".dem":
                ap.error(f"not a demo: {file}")
            (work / (args.game or "id1") / (demo + ".dem")).symlink_to(file.resolve())
            args.demos.append(demo)
            result["imported"][demo] = dict(file=str(file.resolve()), sha256=hashlib.sha256(file.read_bytes()).hexdigest())
    engine = None
    try:
        engine = Engine(args, work)
        engine.wait(lambda t: "CL_SignonReply: 4" in t)
        result["device"] = next((line for line in engine.text().splitlines() if line.startswith("Device:")), None)
        modes = [0] if args.stock_only else sorted(args.modes)
        if args.frustum_control:
            modes.append(FRUSTUM)
        if args.mesh_frustum_control:
            modes.append(MESH_FRUSTUM)
        if args.task_control:
            modes.append(TASK_MESH)
        if args.task_frustum_control:
            modes.append(TASK_FRUSTUM)
        result["settings"]["modes"] = modes
        if not args.visual_only:
            demos = args.demos
            if args.record_views:
                demos, result["recorded"] = [], {}
                for idx, name in enumerate(dict.fromkeys(n for n, _ in views)):
                    demo, sha = engine.record_route(idx, name, [pose for n, pose in views if n == name])
                    demos.append(demo)
                    result["recorded"][demo] = dict(map=name, sha256=sha)
            for demo in demos:
                data = dict(warmups={}, pairs=[])
                result["timedemos"][demo] = data
                for mode in modes:
                    data["warmups"][mode] = engine.demo(demo, mode, warmup=True)
                for index in range(args.pairs):
                    pair = {}
                    for mode in balanced_order(modes, index):
                        pair[mode] = engine.demo(demo, mode)
                    data["pairs"].append(pair)
                data["median_frame_ms"] = {mode: statistics.median(p[mode]["frame_ms"] for p in data["pairs"]) for mode in modes}
                if len({p[m]["frames"] for p in data["pairs"] for m in modes}) != 1:
                    raise RuntimeError("timedemo frame counts do not match")
                comparisons = paired_changes(data["pairs"], modes, "frame_ms")
                data["frame_ms_change_vs_stock_percent"] = comparisons
                if FRUSTUM in modes:
                    data["hiz_vs_frustum_frame_ms_change_percent"] = paired_changes(data["pairs"], [2], "frame_ms", FRUSTUM)[2]
                if MESH_FRUSTUM in modes:
                    data["mesh_hiz_vs_frustum_frame_ms_change_percent"] = paired_changes(data["pairs"], [3], "frame_ms", MESH_FRUSTUM)[3]
                if TASK_MESH in modes:
                    data["task_vs_mesh_frame_ms_change_percent"] = paired_changes(data["pairs"], [TASK_MESH], "frame_ms", 3)[TASK_MESH]
                if TASK_FRUSTUM in modes and MESH_FRUSTUM in modes:
                    data["task_vs_mesh_frustum_frame_ms_change_percent"] = paired_changes(data["pairs"], [TASK_FRUSTUM], "frame_ms", MESH_FRUSTUM)[TASK_FRUSTUM]
                if 1 in comparisons:
                    data["paired_frame_ms_change_percent"] = comparisons[1]
        for idx, (name, pose) in enumerate(views):
            engine.view(name, pose)
            label = f"{idx}_{name}"
            entry = dict(map=name, pose=pose, backend={})
            result["visual"][label] = entry
            entry["backend"][0] = engine.backend(0)
            a = engine.photo(label+"_stock")
            control = engine.photo(label+"_stock_repeat")
            entry["stock_repeat"] = delta(a, control)
            images = {}
            for mode in modes[1:]:
                entry["backend"][mode] = engine.backend(mode)
                b = images[mode] = engine.photo(label + '_' + {1: 'meshlets', 2: 'occlusion', 3: 'mesh_shader', FRUSTUM: 'frustum', MESH_FRUSTUM: 'mesh_frustum', TASK_MESH: 'task_mesh', TASK_FRUSTUM: 'task_frustum'}[mode])
                change = delta(a, b)
                entry.setdefault("vs_stock", {})[mode] = change
                if mode == 1:
                    entry["candidate"] = change
                # Compare against temporal noise in the stock repeat as well
                # as absolute error. Save all images for inspection.
                if (mode == 1 or FRUSTUM not in modes) and (change["mean_rgb"] > entry["stock_repeat"]["mean_rgb"] + .10 or
                    change["percent_over_4"] > entry["stock_repeat"]["percent_over_4"] + .20):
                    raise RuntimeError(f"visible difference in {label}: {entry}")
            if 2 in images and 3 in images:
                change = entry['mesh_shader_vs_compute'] = delta(images[2], images[3])
                if (change['mean_rgb'] > entry['stock_repeat']['mean_rgb'] + .10 or
                        change['percent_over_4'] > entry['stock_repeat']['percent_over_4'] + .20):
                    raise RuntimeError(f"mesh shader changed visible world geometry in {label}: {change}")
            if FRUSTUM in modes:
                change = entry['hiz_vs_frustum'] = delta(images[2], images[FRUSTUM])
                if (change["mean_rgb"] > entry["stock_repeat"]["mean_rgb"] + .10 or
                    change["percent_over_4"] > entry["stock_repeat"]["percent_over_4"] + .20):
                    raise RuntimeError(f"occlusion changed visible world geometry in {label}: {change}")
            if MESH_FRUSTUM in images:
                change = entry['mesh_hiz_vs_frustum'] = delta(images[3], images[MESH_FRUSTUM])
                if (change['mean_rgb'] > entry['stock_repeat']['mean_rgb'] + .10 or
                        change['percent_over_4'] > entry['stock_repeat']['percent_over_4'] + .20):
                    raise RuntimeError(f"mesh shader occlusion changed visible world geometry in {label}: {change}")
                if FRUSTUM in images:
                    change = entry['mesh_vs_compute_frustum'] = delta(images[FRUSTUM], images[MESH_FRUSTUM])
                    if (change['mean_rgb'] > entry['stock_repeat']['mean_rgb'] + .10 or
                            change['percent_over_4'] > entry['stock_repeat']['percent_over_4'] + .20):
                        raise RuntimeError(f"mesh shader frustum changed visible world geometry in {label}: {change}")
            for task_mode, reference in ((TASK_MESH, 3), (TASK_FRUSTUM, MESH_FRUSTUM)):
                if task_mode not in images:
                    continue
                # A frustum-only task capture can also be checked against the
                # ordinary mesh path when its frustum control was not selected.
                if reference not in images:
                    reference = 3
                change = entry[f'{task_mode}_vs_mesh'] = delta(images[reference], images[task_mode])
                if (change['mean_rgb'] > entry['stock_repeat']['mean_rgb'] + .10 or
                        change['percent_over_4'] > entry['stock_repeat']['percent_over_4'] + .20):
                    raise RuntimeError(f"task shader changed visible world geometry in {label}: {change}")
            if args.gpu_seconds:
                pairs = []
                gpu = result["gpu"][label] = dict(pairs=pairs)
                # Pausing triggers a 16ms sleep in vkQuake's main loop and
                # can downclock the GPU. Unpause, keep the camera fixed and
                # advance simulation only one microsecond per rendered frame.
                engine.cmd("pause\nhost_maxfps 0\nhost_framerate 0.000001")
                time.sleep(.7)
                for i in range(args.gpu_pairs):
                    pair = {}
                    for mode in balanced_order(modes, i + idx):
                        engine.backend(mode)
                        time.sleep(.35)
                        engine.cmd("gpu_frame_time start")
                        time.sleep(args.gpu_seconds)
                        raw = engine.cmd("gpu_frame_time stop")
                        match = re.search(r"GPU frame time: (\d+) samples, ([\d.]+) ms mean", raw)
                        if not match or int(match[1]) < 1:
                            raise RuntimeError(f"GPU timestamps unavailable: {raw}")
                        pair[mode] = dict(samples=int(match[1]), mean_ms=float(match[2]))
                    pairs.append(pair)
                gpu["median_mean_ms"] = {m: statistics.median(p[m]["mean_ms"] for p in pairs) for m in modes}
                comparisons = paired_changes(pairs, modes, "mean_ms")
                gpu["gpu_ms_change_vs_stock_percent"] = comparisons
                if FRUSTUM in modes:
                    gpu["hiz_vs_frustum_gpu_ms_change_percent"] = paired_changes(pairs, [2], "mean_ms", FRUSTUM)[2]
                if MESH_FRUSTUM in modes:
                    gpu["mesh_hiz_vs_frustum_gpu_ms_change_percent"] = paired_changes(pairs, [3], "mean_ms", MESH_FRUSTUM)[3]
                if TASK_MESH in modes:
                    gpu["task_vs_mesh_gpu_ms_change_percent"] = paired_changes(pairs, [TASK_MESH], "mean_ms", 3)[TASK_MESH]
                if TASK_FRUSTUM in modes and MESH_FRUSTUM in modes:
                    gpu["task_vs_mesh_frustum_gpu_ms_change_percent"] = paired_changes(pairs, [TASK_FRUSTUM], "mean_ms", MESH_FRUSTUM)[TASK_FRUSTUM]
                if 1 in comparisons:
                    gpu["paired_change_percent"] = comparisons[1]
                engine.cmd("host_maxfps 60\nhost_framerate 0")
        result["success"] = True
    except Exception as exc:
        result["error"] = str(exc)
    finally:
        if engine:
            engine.close()
        if args.validation:
            log = (work / "output.log").read_text(errors="replace")
            errors = [line for line in log.splitlines() if re.search(r"VUID-|Validation Error|SYNC-HAZARD|hazard detected", line, re.I)]
            result["validation"] = dict(errors=errors[:20], active="VK_LAYER_KHRONOS_validation" in log)
            if errors or not result["validation"]["active"]:
                result["success"] = False
        (work / "summary.json").write_text(json.dumps(result, indent=2)+"\n")
        print({k:result[k] for k in ("success","work","error") if k in result}, flush=True)
    if not result["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
