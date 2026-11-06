#!/usr/bin/env python3
"""Capture held full-resolution Gaussian source controls in an isolated game.

Functional visual evidence only; existing games may be left running explicitly.
No user configuration, assets or live game commands are changed.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import time
from types import SimpleNamespace

import bench_meshlets as bench
import bench_radiance as rc
from bench_ecs_ripple import FrameCounter
from smoke_radiance_receiver_gaussian import POSES
from smoke_radiance_sky_grazing import verify_cvar

QUERY = ('radiance_stats\nviewpos\nr_lightmap\nr_indirect\nvid_fsaa\nfov\n'
         'r_radiance_highlight\nr_radiance_quality\nr_radiance_grid\n'
         'r_radiance_probe_gaussian\nr_radiance_receiver_diagnostics\nhost_timescale')
ERRORS = re.compile(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', re.I)


def verify(output, gains, grid):
    rc.verify_state(output, 'radiance', 'hardware', 0)
    rc.verify_receiver_state(output, 'full', 1, 'texture', True)
    rc.verify_pose(output, POSES['doorway'])
    rc.verify_probe_filter(output, 2)
    rc.verify_hold(output)
    rc.verify_receiver_diagnostics_off(output)
    for name, value in [('r_lightmap', 1), ('r_indirect', 0), ('vid_fsaa', 1), ('fov', 90),
                        ('r_radiance_highlight', 0), ('r_radiance_quality', 0), ('r_radiance_grid', grid)]:
        verify_cvar(output, name, value)
    return rc.verify_sources(output, *gains)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-existing', action='store_true')
    parser.add_argument('--all-sources', action='store_true', help='also capture point0, emissive0 and grid0 controls')
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    binary, root = args.binary.resolve(), args.output.resolve()
    source = args.basedir.resolve()
    if (source/'id1').is_dir(): source /= 'id1'
    paks = sorted(source.glob('pak*.pak'))
    if not binary.is_file() or not paks: parser.error('existing executable and Quake PAKs are required')
    rc.ensure_no_game(args.allow_existing)
    root.mkdir(parents=True, exist_ok=False)
    frozen = root/'binary/vkquake'; frozen.parent.mkdir(); shutil.copy2(binary, frozen)
    for directory in ('harness', 'id1', 'user'): (root/directory).mkdir()
    for name in (Path(__file__).name, 'bench_radiance.py', 'bench_meshlets.py', 'bench_ecs_ripple.py',
                 'smoke_radiance_receiver_gaussian.py', 'smoke_radiance_sky_grazing.py', 'smoke_radiance_start_spots.py'):
        shutil.copy2(Path(__file__).with_name(name), root/'harness'/name)
    for pak in paks: (root/'id1'/pak.name).symlink_to(pak)
    startup = ('host_timescale .000001\nscr_centertime 0\nr_rtshadows 0\nr_radiance 1\n'
               'r_radiance_backend 2\nr_radiance_quality 0\nr_radiance_grid 1\n'
               'r_radiance_point_gain 7\nr_radiance_emissive_gain 1\nr_radiance_bounce_gain 8\n'
               'r_radiance_sky_fallback .15\nr_radiance_receiver_filter 1\nr_radiance_receiver_diagnostics 0\n'
               'r_radiance_resolution_scale 1\nr_radiance_probe_gaussian 2\nr_radiance_checkerboard 0\n'
               'r_lightmap 1\nr_radiance_highlight 0\nfov 90\nviewsize 130\nr_drawviewmodel 0\n'
               'crosshair 0\ncl_bob 0\ncl_rollangle 0\nmap e1m1')
    (root/'id1/boot.cfg').write_text(bench.BOOT.replace('vid_fsaa 0', 'vid_fsaa 1').replace('r_indirect 1', 'r_indirect 0').replace('map start', startup))
    config = SimpleNamespace(binary=frozen, width=1920, height=1080, timeout=180, validation=args.validation, game=None)
    report = {'binary': {'source_path': str(binary), 'path': str(frozen), 'sha256': rc.sha256(frozen)},
              'assets': {str(p): rc.sha256(p) for p in paks}, 'pose': POSES['doorway'], 'size': [1920,1080],
              'authoritative_timing': False, 'validation_requested': args.validation, 'complete': False, 'captures': {}}
    engine, counter = bench.Engine(config, root), None
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        counter = FrameCounter(engine.process.pid, frozen); clock = rc.SimulationClock(counter, frozen)
        engine.cmd('setpos '+' '.join(map(str, POSES['doorway'])))
        cases = [('baseline', (7,1,8), 1), ('bounce0', (7,1,0), 1)]
        if args.all_sources:
            cases += [('point0', (0,1,8), 1), ('emissive0', (7,0,8), 1), ('grid0', (7,1,8), 0)]
        sources = None
        for index, (label,gains,grid) in enumerate(cases):
            engine.cmd(f'r_radiance_point_gain {gains[0]}\nr_radiance_emissive_gain {gains[1]}\nr_radiance_bounce_gain {gains[2]}\nr_radiance_grid {grid}')
            time.sleep(12 if index==0 else 2)
            before = engine.cmd(QUERY); current = verify(before,gains,grid)
            if sources is not None and current != sources: raise RuntimeError('source counts changed')
            sources = current
            start_clock = clock.sample(); capture = engine.photo(label); end_clock = clock.sample()
            after = engine.cmd(QUERY); verify(after,gains,grid)
            if not 0 <= end_clock-start_clock < .001: raise RuntimeError('simulation advanced during capture')
            frames = rc.verify_receiver_frames(rc.receiver_frames(before), rc.receiver_frames(after), False)
            report['captures'][label] = {'capture': str(capture), 'gains': gains, 'grid': grid, 'sources': sources,
                'simulation_start': start_clock, 'simulation_end': end_clock, 'receiver_frames': frames,
                'before': before, 'after': after}
            (root/'results.json').write_text(json.dumps(report,indent=2)+'\n')
            print(f'Captured {label}: {capture}',flush=True)
        report['complete'] = True
    finally:
        if counter: counter.close()
        engine.close()
        log = (root/'output.log').read_text(errors='replace')
        report['exit_code'] = engine.process.returncode
        report['validation_errors'] = ERRORS.findall(log)
        report['binary_unchanged'] = rc.sha256(frozen)==report['binary']['sha256']
        (root/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['complete'] or report['exit_code'] != 0 or report['validation_errors'] or not report['binary_unchanged']:
        raise RuntimeError('capture or teardown failed; inspect results.json/output.log')


if __name__ == '__main__': main()
