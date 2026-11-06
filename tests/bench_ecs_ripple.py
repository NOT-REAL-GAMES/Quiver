#!/usr/bin/env python3
"""Alternate release binaries on the live 1,024- or 32,768-entity ripple scene.

Linux only: reads the existing host_framecount symbol from each child process
through /proc, avoiding a console print or other instrumentation every frame.
The executable must retain its normal symbol table (as Meson release builds do).
Requires an SDL development package, Pillow, Vulkan display, and Quake PAKs.
All game directories, configs, screenshots and output logs are isolated.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess
import tempfile
import time
from types import SimpleNamespace

from PIL import Image, ImageChops

import bench_meshlets as bench


COUNT = 32768
STRESS = re.compile(r'ECS stress: active=(\d+) entities=(\d+) ticks=(\d+) updates=(\d+) failures=(\d+) mean_ms=([\d.]+)')
RIPPLE = re.compile(r'ECS ripple: visuals=(\d+) script_mean_ms=([\d.]+) time=([-\d.]+)')
VISUAL = re.compile(r'ECS visuals: local=(\d+)')
POSITION = re.compile(r'ECS visual (\d+):\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


class FrameCounter:
    """Read the same counter used by Quake's on-screen FPS display."""
    def __init__(self, pid, binary):
        symbols = subprocess.run(['nm', '-an', str(binary)], check=True, text=True,
                                 stdout=subprocess.PIPE).stdout
        matches = re.findall(r'^([0-9a-fA-F]+)\s+[bBdD]\s+host_framecount$', symbols, re.M)
        if len(matches) != 1:
            raise RuntimeError(f'{binary} must contain the host_framecount symbol; use an unstripped build')
        address = int(matches[0], 16)
        header = subprocess.run(['readelf', '-h', str(binary)], check=True, text=True,
                                stdout=subprocess.PIPE).stdout
        kind = re.search(r'^\s*Type:\s+(\w+)', header, re.M)
        if not kind or kind.group(1) not in ('DYN', 'EXEC'):
            raise RuntimeError(f'unsupported ELF type: {binary}')
        if kind.group(1) == 'DYN':
            executable = Path(f'/proc/{pid}/exe').resolve()
            base = None
            for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
                fields = line.split(maxsplit=5)
                if len(fields) == 6 and fields[2] == '00000000' and Path(fields[5]).resolve() == executable:
                    base = int(fields[0].split('-')[0], 16)
                    break
            if base is None:
                raise RuntimeError(f'cannot determine PIE load address of process {pid}')
            address += base
        self.address = address
        self.fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)

    def sample(self):
        before = time.monotonic()
        data = os.pread(self.fd, 4, self.address)
        after = time.monotonic()
        if len(data) != 4:
            raise RuntimeError('could not read host frame counter')
        return ((before + after) * .5, struct.unpack('=I', data)[0])

    def close(self):
        os.close(self.fd)


def check_state(engine, count=COUNT, indices=None):
    if indices is None:
        indices = (0, 31, 512, 1023) if count == 1024 else (0, 8191, 16384, 32767)
    results = []
    for index in indices:
        output = engine.cmd(f'ecs_stress_stats\necs_visual_stats {index}')
        stress, ripple, visuals, pos = (STRESS.search(output), RIPPLE.search(output),
                                        VISUAL.search(output), POSITION.search(output))
        if not all((stress, ripple, visuals, pos)):
            raise RuntimeError(f'missing stress/visual statistics: {output}')
        active, entities, ticks, updates, failures = map(int, stress.groups()[:5])
        if (active, entities, failures, int(ripple.group(1)), int(visuals.group(1))) != (1, count, 0, count, count):
            raise RuntimeError(f'incomplete or failing workload: {output}')
        if not ticks or updates != ticks * count or int(pos.group(1)) != index:
            raise RuntimeError(f'incorrect entity/tick counts: {output}')
        x, y, z = map(float, pos.groups()[1:])
        if count == 1024:
            expected_x = (index % 32 - 15.5) * 32
            expected_y = (index // 32 - 15.5) * 16
        else:
            expected_x = (index % 256 - 127.5) * 4
            expected_y = (index // 256 - 63.5) * 4
        clock = float(ripple.group(3))
        expected_z = 160 + 24 * math.sin(clock * 2 * math.pi / 3 - math.hypot(x, y) * 2 * math.pi / 256)
        if abs(x - expected_x) > .01 or abs(y - expected_y) > .01 or abs(z - expected_z) > .02:
            raise RuntimeError(f'wrong position for entity {index}: {(x, y, z)} expected {(expected_x, expected_y, expected_z)}')
        results.append(dict(index=index, x=x, y=y, z=z, simulation_time=clock,
                            ecs_mean_ms=float(stress.group(6)), script_mean_ms=float(ripple.group(2)), ticks=ticks))
    return results


def check_pixels(engine, width, height, compare_renderer=False):
    # Pausing holds the wave at the same time for both screenshots. The upper
    # 70% excludes the weapon and HUD; toggling entity drawing must change a
    # substantial part of the image, not merely report a synthetic count.
    engine.cmd('pause\nr_drawentities 0')
    hidden = engine.photo('entities-off')
    engine.cmd('r_drawentities 1')
    visible = engine.photo('entities-on')
    with Image.open(hidden) as a, Image.open(visible) as b:
        if a.size != b.size or a.size != (width, height):
            raise RuntimeError(f'unexpected screenshot sizes: {a.size}, {b.size}')
        crop = (0, 0, width, int(height * .70))
        delta = ImageChops.difference(a.convert('RGB').crop(crop), b.convert('RGB').crop(crop))
        pixels = delta.get_flattened_data() if hasattr(delta, 'get_flattened_data') else delta.getdata()
        changed = sum(1 for pixel in pixels if max(pixel) >= 24)
        fraction = changed / (crop[2] * crop[3])
    if fraction < .005:
        engine.cmd('pause')
        raise RuntimeError(f'ripple markers did not visibly render (changed pixel fraction {fraction:.5f})')
    result = dict(changed_fraction=fraction, visible_screenshot=str(visible), hidden_screenshot=str(hidden))
    if compare_renderer:
        # The batch switch is a candidate-only debug control. Compare both
        # paths on the same paused state, with the same scene and camera.
        setting = engine.cmd('r_ecs_batch 0\nr_ecs_batch')
        if 'Unknown command' in setting or not re.search(r'"r_ecs_batch"\s+is\s+"?0', setting):
            raise RuntimeError(f'candidate lacks the r_ecs_batch switch: {setting}')
        time.sleep(.1)
        state = engine.cmd('ecs_visual_stats')
        if not re.search(r'\bbatched=0\b', state):
            raise RuntimeError(f'legacy renderer did not take effect before capture: {state}')
        legacy = engine.photo('renderer-legacy')
        engine.cmd('r_ecs_batch 1')
        time.sleep(.1)
        state = engine.cmd('ecs_visual_stats')
        if not re.search(r'\bbatched=1\b', state):
            raise RuntimeError(f'batched renderer did not take effect before capture: {state}')
        batched = engine.photo('renderer-batched')
        with Image.open(legacy) as a, Image.open(batched) as b:
            difference = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
            data = difference.get_flattened_data() if hasattr(difference, 'get_flattened_data') else difference.getdata()
            changed_5 = changed_24 = total = 0
            for pixel in data:
                total += sum(pixel)
                changed_5 += max(pixel) >= 5
                changed_24 += max(pixel) >= 24
            count = width * height
        result['renderer_comparison'] = dict(legacy_screenshot=str(legacy), batched_screenshot=str(batched),
                                              mean_absolute_rgb_error=total / (3 * count),
                                              fraction_changed_5=changed_5 / count,
                                              fraction_changed_24=changed_24 / count)
    engine.cmd('pause')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, help='original unstripped engine binary')
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True, help='directory containing id1 or its PAKs')
    parser.add_argument('--progs', type=Path, required=True, help='compiled ripple gamecode shared by both binaries')
    parser.add_argument('--pairs', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--warmup', type=float, default=2)
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--entities', type=int, choices=(1024, 32768), default=32768,
                        help='which playable ripple map to exercise')
    parser.add_argument('--baseline-map', help='map name for an older baseline fixture; defaults to the selected map')
    parser.add_argument('--target-fps', type=float, default=500, help='acceptance threshold (default: 500)')
    parser.add_argument('--max-edicts', type=int, default=32800)
    parser.add_argument('--rtshadows', type=int, choices=(0, 1, 2), default=0,
                        help='ray-traced shadow setting (default 0 matches the existing ripple baseline)')
    parser.add_argument('--compare-renderer', action='store_true', help='compare paused candidate legacy and batched renderer screenshots')
    parser.add_argument('--timed-no-entities', action='store_true', help='diagnostic only: disable entity drawing during timed samples')
    parser.add_argument('--renderer', type=int, choices=(0, 1), help='select r_ecs_batch when profiling the candidate')
    parser.add_argument('--host-profile-seconds', type=float, default=0, help='collect separate host_speeds diagnostic after timing')
    parser.add_argument('--gpu-profile-seconds', type=float, default=0, help='collect separate completed-frame GPU timing after timing')
    parser.add_argument('--setup', action='append', default=[], help='additional console command before loading the map; repeatable')
    parser.add_argument('--baseline-setup', action='append', default=[], help='baseline-only startup console command; repeatable')
    parser.add_argument('--candidate-setup', action='append', default=[], help='candidate-only startup console command; repeatable')
    parser.add_argument('--require-batched', type=int, choices=(0, 1), help='require candidate ecs_visual_stats to report the selected path')
    parser.add_argument('--require-gpu-cull', action='store_true', help='require the candidate GPU instance culling path to dispatch')
    parser.add_argument('--validation', action='store_true', help='enable and check the Vulkan validation layer')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--output', type=Path, help='new empty output directory; default is a temporary directory')
    args = parser.parse_args()
    if args.pairs < 1 or args.seconds < 1 or args.warmup < 0 or args.max_edicts < args.entities + 2 or args.target_fps <= 0:
        parser.error('invalid measurement duration, pair count, or edict capacity')
    if args.width < 320 or args.height < 200:
        parser.error('resolution too small')
    binaries = {'candidate': args.candidate.resolve()}
    if args.baseline:
        binaries = {'baseline': args.baseline.resolve(), **binaries}
    progs = args.progs.resolve()
    for path in [*binaries.values(), progs]:
        if not path.is_file(): parser.error(f'file not found: {path}')
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs in {source}')
    if args.output:
        root = args.output.resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        root = Path(tempfile.mkdtemp(prefix='quiver-ripple-bench-')).resolve()
    print(f'results: {root}', flush=True)
    report = dict(resolution=[args.width, args.height], entities=args.entities, target_fps=args.target_fps,
                  baseline_map=args.baseline_map, max_edicts=args.max_edicts, rtshadows=args.rtshadows,
                  timed_no_entities=args.timed_no_entities, renderer=args.renderer, setup=args.setup,
                  baseline_setup=args.baseline_setup, candidate_setup=args.candidate_setup, validation=args.validation,
                  seconds=args.seconds, warmup=args.warmup, pairs=[],
                  binaries={name: dict(path=str(path), sha256=sha256(path)) for name, path in binaries.items()},
                  progs=dict(path=str(progs), sha256=sha256(progs)), paks=[str(x) for x in paks])

    def run(label, pair):
        work = root / f'{pair:02d}-{label}'
        (work / 'id1').mkdir(parents=True)
        (work / 'quiver_ripple').mkdir()
        (work / 'user').mkdir()
        for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
        (work / 'quiver_ripple/progs.dat').symlink_to(progs)
        map_name = args.baseline_map if label == 'baseline' and args.baseline_map else f'ecs{args.entities}'
        extra_setup = args.baseline_setup if label == 'baseline' else args.candidate_setup
        startup = (f'r_rtshadows {args.rtshadows}\nmax_edicts {args.max_edicts}\n'
                   'host_maxfps 0\nhost_speeds 0\nsv_speeds 0\nscr_showfps 0\n' +
                   ''.join(command + '\n' for command in args.setup + extra_setup) + f'map {map_name}')
        (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', startup))
        config = SimpleNamespace(binary=binaries[label], width=args.width, height=args.height,
                                 timeout=args.timeout, validation=args.validation, game='quiver_ripple', stock_only=True)
        engine = bench.Engine(config, work)
        counter = None
        try:
            engine.wait(lambda text: 'CL_SignonReply: 4' in text)
            engine.cmd('host_maxfps 0\nhost_speeds 0\nsv_speeds 0\nscr_showfps 0\nsetpos -480 0 32 0 0 0')
            if label == 'candidate' and args.renderer is not None:
                setting = engine.cmd(f'r_ecs_batch {args.renderer}\nr_ecs_batch')
                if 'Unknown command' in setting:
                    raise RuntimeError(f'candidate lacks r_ecs_batch: {setting}')
            if label == 'candidate' and args.require_batched is not None:
                state = engine.cmd('ecs_visual_stats')
                if not re.search(rf'\bbatched={args.require_batched}\b', state):
                    raise RuntimeError(f'candidate render path not as requested: {state}')
            if label == 'candidate' and args.require_gpu_cull:
                state = engine.cmd('ecs_gpu_stats')
                if not re.search(r'enabled=1\s+available=1\s+active=1\s+dispatches=[1-9]\d*', state):
                    raise RuntimeError(f'GPU culling was not active: {state}')
            time.sleep(.1)
            player = engine.cmd('edict 1')
            origin = re.search(r"origin\s+'\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s*'", player)
            if not origin or any(abs(a - b) > .1 for a, b in zip(map(float, origin.groups()), (-480, 0, 32))):
                raise RuntimeError(f'benchmark camera failed to reach its fixed position: {player}')
            before = check_state(engine, args.entities)
            pixels = check_pixels(engine, args.width, args.height, args.compare_renderer and label == 'candidate')
            if label == 'candidate' and args.renderer is not None:
                engine.cmd(f'r_ecs_batch {args.renderer}')
            if args.timed_no_entities: engine.cmd('r_drawentities 0')
            time.sleep(args.warmup)
            counter = FrameCounter(engine.process.pid, binaries[label])
            first = previous = counter.sample()
            windows = []
            deadline = first[0] + args.seconds
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0: break
                time.sleep(min(1.0, remaining))
                current = counter.sample()
                elapsed = current[0] - previous[0]
                frames = (current[1] - previous[1]) & 0xffffffff
                if elapsed <= 0 or not frames: raise RuntimeError('engine stopped producing frames')
                windows.append(dict(seconds=elapsed, frames=frames, fps=frames / elapsed))
                previous = current
            elapsed = previous[0] - first[0]
            frames = (previous[1] - first[1]) & 0xffffffff
            if args.timed_no_entities: engine.cmd('r_drawentities 1')
            after = check_state(engine, args.entities)
            if after[0]['ticks'] <= before[0]['ticks']:
                raise RuntimeError('ripple ticks did not advance during measurement')
            # A sample exactly one or more three-second periods later can
            # legitimately return to its starting Z. Check a second phase.
            if after[0]['z'] == before[0]['z']:
                time.sleep(.25)
                shifted = check_state(engine, args.entities, indices=(0,))[0]
                if shifted['z'] == after[0]['z']:
                    raise RuntimeError('ripple position did not animate')
            status = Path(f'/proc/{engine.process.pid}/status').read_text()
            rss = re.search(r'^VmRSS:\s+(\d+)\s+kB', status, re.M)
            host_profile = None
            if args.host_profile_seconds > 0:
                if args.timed_no_entities: engine.cmd('r_drawentities 0')
                engine.cmd('host_speeds 1')
                time.sleep(.3)
                start = len(engine.text())
                time.sleep(args.host_profile_seconds)
                log = engine.text()[start:]
                engine.cmd('host_speeds 0\nr_drawentities 1')
                samples = [tuple(map(float, row)) for row in re.findall(
                    r'([\d.]+) tot\s+([\d.]+) server\s+([\d.]+) gfx\s+([\d.]+) snd', log)]
                if not samples: raise RuntimeError('no host_speeds samples')
                host_profile = dict(samples=len(samples), total_median_ms=statistics.median(x[0] for x in samples),
                                    server_median_ms=statistics.median(x[1] for x in samples),
                                    gfx_median_ms=statistics.median(x[2] for x in samples))
            gpu_profile = None
            if args.gpu_profile_seconds > 0:
                if args.timed_no_entities: engine.cmd('r_drawentities 0')
                engine.cmd('gpu_frame_time start')
                time.sleep(args.gpu_profile_seconds)
                output = engine.cmd('gpu_frame_time stop\nr_drawentities 1')
                match = re.search(r'GPU frame time: (\d+) samples, ([\d.]+) ms mean', output)
                if not match: raise RuntimeError(f'no GPU frame timing samples: {output}')
                gpu_profile = dict(samples=int(match.group(1)), mean_ms=float(match.group(2)))
            if args.validation:
                # stdout may still be buffered while the child is running;
                # the engine flushes its debug console log after each message.
                log = (work / 'output.log').read_text(errors='replace') + engine.text()
                if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                    raise RuntimeError(f'Vulkan validation unavailable or reported an error; see {work}/output.log')
            return dict(fps=frames / elapsed, frame_ms=1000 * elapsed / frames,
                        measured_seconds=elapsed, frames=frames, windows=windows,
                        rss_kib=int(rss.group(1)) if rss else None,
                        state_before=before, state_after=after, image=pixels, host_profile=host_profile, gpu_profile=gpu_profile)
        finally:
            if counter: counter.close()
            if engine.process.poll() is None:
                engine.process.terminate()
                engine.process.wait(timeout=10)
            engine.close()

    try:
        for pair in range(args.pairs):
            row = {}
            for label in bench.balanced_order(list(binaries), pair):
                row[label] = run(label, pair)
                print(f'{label} pair {pair}: {row[label]["fps"]:.2f} FPS / {row[label]["frame_ms"]:.3f} ms', flush=True)
            report['pairs'].append(row)
            (root / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        report['median_fps'] = {label: statistics.median(row[label]['fps'] for row in report['pairs']) for label in binaries}
        report['minimum_candidate_window_fps'] = min(window['fps'] for row in report['pairs'] for window in row['candidate']['windows'])
        report['candidate_all_runs_500fps'] = not args.timed_no_entities and all(row['candidate']['fps'] >= 500 for row in report['pairs'])
        report['candidate_all_windows_500fps'] = not args.timed_no_entities and report['minimum_candidate_window_fps'] >= 500
        report['candidate_all_runs_target'] = not args.timed_no_entities and all(row['candidate']['fps'] >= args.target_fps for row in report['pairs'])
        report['candidate_all_windows_target'] = not args.timed_no_entities and report['minimum_candidate_window_fps'] >= args.target_fps
        if 'baseline' in binaries:
            report['median_paired_frame_time_change_percent'] = statistics.median(
                100 * (row['candidate']['frame_ms'] / row['baseline']['frame_ms'] - 1) for row in report['pairs'])
        report['target_met'] = report['candidate_all_runs_target'] and report['candidate_all_windows_target']
        report['benchmark_completed'] = True
    finally:
        (root / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['median_fps'], indent=2), flush=True)


if __name__ == '__main__': main()
