#!/usr/bin/env python3
"""Collect separate receiver stage timings and optional estimated reuse counters.

These are diagnostic workloads. Stage ranges overlap, and enabling coverage
counters changes the workload; neither replaces the uninstrumented benchmark.
"""
import argparse
import json
from pathlib import Path
import re
import time
from types import SimpleNamespace

import bench_radiance as rc
from bench_radiance_receiver_matrix import CAMERAS

HEADER = re.compile(r'estimated receiver coverage frame=(\d+) viewport=(\d+)x(\d+) divisor=(\S+) output=(\w+)')
BLOCKS = re.compile(r'receiver blocks total=(\d+) empty=(\d+) reusable=(\d+) mixed=(\d+) raw_GI_valid=(\d+) selected_GI_valid=(\d+)')
PIXELS = re.compile(r'receiver pixels covered=(\d+) GI_reuse=(\d+) GI_fallback=(\d+) direct_reuse=(\d+) direct_fallback=(\d+) invalid_geometry=(\d+)')


def coverage(output, filtering, divisor, maximum_width, maximum_height, output_kind=None):
    header, blocks, pixels = HEADER.search(output), BLOCKS.search(output), PIXELS.search(output)
    if not header or not blocks or not pixels: raise RuntimeError('receiver coverage is unavailable')
    frame, width, height = map(int, header.groups()[:3])
    actual_divisor = float(header[4])
    total, empty, reusable, mixed, raw, selected = map(int, blocks.groups())
    covered, gi, gi_fallback, direct, direct_fallback, invalid = map(int, pixels.groups())
    expected_output = output_kind or ('filtered' if filtering == 'texture' else 'raw')
    if not 0 < width <= maximum_width or not 0 < height <= maximum_height or actual_divisor != divisor or header[5] != expected_output:
        raise RuntimeError(f'unexpected coverage viewport/filter: {header[0]}')
    expected_blocks = rc.receiver_extent(width, 1/divisor) * rc.receiver_extent(height, 1/divisor)
    if total != expected_blocks or empty+reusable+mixed != total:
        raise RuntimeError('receiver block counters do not partition the viewport')
    if max(raw, selected) > reusable or covered > width*height or invalid > covered or gi+gi_fallback != covered or direct+direct_fallback != covered:
        raise RuntimeError('receiver pixel/sample counters do not partition their domains')
    return {'frame': frame, 'viewport': [width, height], 'divisor': divisor, 'output': header[5],
        'blocks': {'total': total, 'empty': empty, 'reusable': reusable, 'mixed': mixed, 'raw_GI_valid': raw, 'selected_GI_valid': selected},
        'pixels': {'covered': covered, 'GI_reuse': gi, 'GI_fallback': gi_fallback, 'direct_reuse': direct, 'direct_fallback': direct_fallback, 'invalid_geometry': invalid},
        'GI_reuse_fraction': gi/covered if covered else None, 'direct_reuse_fraction': direct/covered if covered else None}


def main():
    import bench_meshlets as bench
    from bench_ecs_ripple import FrameCounter
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--basedir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cameras', nargs='+', choices=tuple(CAMERAS), default=['shaft', 'start'])
    parser.add_argument('--backend', choices=('hardware', 'software'), default='hardware')
    parser.add_argument('--quality', type=int, choices=(0, 1, 2), default=1)
    parser.add_argument('--probe-gaussian', type=rc.probe_gaussian_strength,
                        help='override the per-camera Gaussian strengths (shaft2, others1)')
    parser.add_argument('--receiver-scales', nargs='+', type=rc.resolution_scale, default=[.25])
    parser.add_argument('--receiver-filters', nargs='+', choices=('probe', 'texture'), default=['probe', 'texture'])
    parser.add_argument('--checkerboard', type=int, choices=(0, 1), default=1)
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--seconds', type=float, default=2)
    parser.add_argument('--warmup', type=float, default=1)
    parser.add_argument('--initial-warmup', type=float, default=10)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    if 1 in args.receiver_scales or len(set(args.receiver_scales)) != len(args.receiver_scales):
        parser.error('diagnostic receiver scales must be distinct reduced scales')
    modes = [('full', 'full', 1, 0)]
    for filtering in args.receiver_filters:
        for scale in args.receiver_scales:
            label = filtering if len(args.receiver_scales) == 1 else f'{filtering}_d{rc.receiver_divisor(scale):g}_c{args.checkerboard}'
            modes.append((label, filtering, scale, args.checkerboard))
    binary, root = args.binary.resolve(), args.output.resolve()
    source = args.basedir.resolve()
    if (source/'id1').is_dir(): source /= 'id1'
    paks = sorted(source.glob('pak*.pak'))
    if not binary.is_file() or not paks: parser.error('existing executable and Quake PAKs are required')
    rc.ensure_no_game(False)
    root.mkdir(parents=True, exist_ok=False)
    report = {'binary': {'path': str(binary), 'sha256': rc.sha256(binary)}, 'authoritative_timing': False,
              'quality': args.quality, 'complete': False, 'cases': {}, 'note': 'Stages sampled with coverage disabled; coverage collected afterward. Estimated counters and overlapping stage ranges are diagnostic.'}
    output = root/'results.json'
    try:
        for camera in args.cameras:
            scene, pose = CAMERAS[camera]
            gaussian = args.probe_gaussian if args.probe_gaussian is not None else (2 if camera == 'shaft' else 1)
            work = root/camera
            (work/'id1').mkdir(parents=True)
            (work/'user').mkdir()
            for pak in paks: (work/'id1'/pak.name).symlink_to(pak)
            startup = (f'host_timescale .000001\nscr_centertime 0\nr_rtshadows 0\nr_radiance 1\nr_radiance_backend {2 if args.backend == "hardware" else 1}\n'
                       f'r_radiance_quality {args.quality}\nr_radiance_grid 0\nr_radiance_point_gain 1\nr_radiance_emissive_gain 1\nr_radiance_bounce_gain 1\n'
                       f'r_radiance_probe_gaussian {gaussian}\nr_radiance_sky_fallback .15\nr_radiance_receiver_diagnostics 0\n'
                       f'r_radiance_receiver_filter 0\nr_radiance_resolution_scale 1\nr_radiance_checkerboard 0\nmap {scene}')
            (work/'id1/boot.cfg').write_text(bench.BOOT.replace('r_indirect 1', 'r_indirect 0').replace('map start', startup))
            config = SimpleNamespace(binary=binary, width=args.width, height=args.height, timeout=args.timeout, validation=False, game=None)
            engine, counter = bench.Engine(config, work), None
            try:
                engine.wait(lambda text: 'CL_SignonReply: 4' in text)
                engine.cmd('setpos ' + ' '.join(map(str, pose)))
                engine.cmd('host_maxfps 0\nvid_vsync 0')
                counter = FrameCounter(engine.process.pid, binary)
                clock = rc.SimulationClock(counter, binary)
                rows, sources = {}, None
                for label, filtering, scale, checker in modes:
                    requested = 'probe' if filtering == 'full' else filtering
                    engine.cmd(f'r_indirect 0\nr_radiance_resolution_scale {scale}\nr_radiance_checkerboard {checker}\n'
                               f'r_radiance_receiver_filter {int(requested == "texture")}\nr_radiance_receiver_diagnostics 0')
                    deadline = time.monotonic()+args.timeout
                    while True:
                        state = engine.cmd('radiance_stats')
                        try:
                            rc.verify_state(state, 'radiance', args.backend, args.quality)
                            rc.verify_receiver_state(state, rc.receiver_name(scale, bool(checker)), scale, requested, True)
                            if not rc.radiance_ready(state): raise RuntimeError('prepass is not ready')
                            break
                        except RuntimeError:
                            if time.monotonic() >= deadline: raise
                            time.sleep(.1)
                    time.sleep(args.initial_warmup+args.warmup)
                    rc.verify_pose(engine.cmd('viewpos'), pose)
                    rc.verify_hold(engine.cmd('host_timescale'))
                    rc.verify_probe_filter(engine.cmd('r_radiance_probe_gaussian'), gaussian)
                    rc.verify_receiver_diagnostics_off(engine.cmd('r_radiance_receiver_diagnostics'))
                    before_state = engine.cmd('radiance_stats')
                    actual_sources = rc.verify_sources(before_state, 1, 1, 1)
                    if sources is not None and sources != actual_sources: raise RuntimeError('diagnostic source counts changed')
                    sources = actual_sources
                    start_clock = clock.sample()
                    engine.cmd('gpu_frame_time start\ngpu_stage_time start')
                    time.sleep(args.seconds)
                    timers = engine.cmd('gpu_frame_time stop\ngpu_stage_time stop')
                    end_clock = clock.sample()
                    if end_clock-start_clock > .001 or end_clock < start_clock: raise RuntimeError('held diagnostic simulation advanced')
                    after_state = engine.cmd('radiance_stats')
                    rc.verify_state(after_state, 'radiance', args.backend, args.quality)
                    rc.verify_receiver_state(after_state, rc.receiver_name(scale, bool(checker)), scale, requested, True)
                    frame_counts = rc.verify_receiver_frames(rc.receiver_frames(before_state), rc.receiver_frames(after_state), scale != 1)
                    row = {'whole_frame_gpu_diagnostic': rc.parse_gpu(timers), 'stages': rc.parse_gpu_stages(timers),
                           'filter': filtering, 'resolution_scale': scale, 'checkerboard': bool(checker),
                           'receiver_frames': frame_counts,
                           'simulation_start': start_clock, 'simulation_end': end_clock, 'state': state, 'timers': timers}
                    (work/f'{label}-timers.txt').write_text(timers)
                    if filtering == 'texture':
                        engine.cmd('r_radiance_receiver_diagnostics 1')
                        deadline, previous = time.monotonic()+args.timeout, None
                        while True:
                            state = engine.cmd('radiance_stats')
                            try:
                                output_kind = 'filtered' if filtering == 'texture' and (gaussian > 0 or checker) else 'raw'
                                value = coverage(state, filtering, rc.receiver_divisor(scale), args.width, args.height, output_kind)
                                if previous is not None and value['frame'] > previous:
                                    row['estimated_coverage'] = value
                                    break
                                previous = value['frame']
                            except RuntimeError:
                                pass
                            if time.monotonic() >= deadline: raise RuntimeError(f'fresh receiver counters unavailable: {state}')
                            time.sleep(.05)
                        (work/f'{label}-coverage.txt').write_text(state)
                        engine.cmd('r_radiance_receiver_diagnostics 0')
                        rc.verify_receiver_diagnostics_off(engine.cmd('r_radiance_receiver_diagnostics'))
                    elif filtering == 'probe':
                        row['coverage_unavailable'] = 'Estimated coverage is implemented only for texture reconstruction.'
                    row['capture'] = str(engine.photo(label))
                    rows[label] = row
                    report['cases'][camera] = {'scene': scene, 'pose': pose, 'gaussian': gaussian, 'modes': rows}
                    output.write_text(json.dumps(report, indent=2)+'\n')
                    print(f'Diagnostic {camera} {label}: {row["whole_frame_gpu_diagnostic"]["median_ms"]}ms; coverage {row.get("estimated_coverage", {}).get("GI_reuse_fraction")}', flush=True)
            finally:
                if counter: counter.close()
                engine.close()
        report['complete'] = True
    finally:
        output.write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__': main()
