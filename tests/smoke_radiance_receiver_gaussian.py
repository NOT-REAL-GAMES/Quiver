#!/usr/bin/env python3
"""Capture held E1M1 Gaussian surface comparisons on one frozen executable."""
import argparse
import json
from pathlib import Path
import shutil
import time
from types import SimpleNamespace

import bench_meshlets as bench
import bench_radiance as rc
from bench_ecs_ripple import FrameCounter

POSES = {
    'doorway': (607, 143, 40, 3, 91, 0),
    'spawn': (480, -352, 88, 0, 90, 0),
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--poses', nargs='+', choices=tuple(POSES), default=list(POSES))
    p.add_argument('--scales', nargs='+', type=rc.resolution_scale, default=[1, .5, .25])
    p.add_argument('--width', type=int, default=1920)
    p.add_argument('--height', type=int, default=1080)
    p.add_argument('--point-gain', type=float, default=1)
    p.add_argument('--bounce-gain', type=float, default=8)
    p.add_argument('--initial-warmup', type=float, default=10)
    p.add_argument('--warmup', type=float, default=2)
    p.add_argument('--timeout', type=float, default=180)
    p.add_argument('--normal-textures', action='store_true')
    p.add_argument('--allow-existing', action='store_true',
                   help='allow a separate running game for functional captures; do not use for timing')
    args = p.parse_args()
    binary, root = args.binary.resolve(), args.output.resolve()
    source = args.basedir.resolve()
    if (source/'id1').is_dir(): source /= 'id1'
    paks = sorted(source.glob('pak*.pak'))
    if not binary.is_file() or not paks: p.error('existing executable and Quake PAKs are required')
    if args.initial_warmup < 10 or args.warmup < 2: p.error('capture warmups require at least10 initial and2 per mode seconds')
    rc.ensure_no_game(args.allow_existing)
    root.mkdir(parents=True, exist_ok=False)
    frozen = root/'binary/vkquake'
    frozen.parent.mkdir()
    shutil.copy2(binary, frozen)
    (root/'harness').mkdir()
    for name in (Path(__file__).name, 'bench_radiance.py', 'bench_meshlets.py', 'bench_ecs_ripple.py'):
        shutil.copy2(Path(__file__).with_name(name), root/'harness'/name)
    report = {'binary': {'source_path': str(binary), 'path': str(frozen), 'sha256': rc.sha256(frozen)},
              'settings': {key: value for key, value in vars(args).items() if key not in ('binary', 'basedir', 'output')},
              'harness_sha256': {path.name: rc.sha256(path) for path in (root/'harness').iterdir()},
              'assets': {str(pak): rc.sha256(pak) for pak in paks}, 'authoritative_timing': False,
              'complete': False, 'captures': {}}
    (root/'id1').mkdir()
    (root/'user').mkdir()
    for pak in paks: (root/'id1'/pak.name).symlink_to(pak)
    startup = (f'host_timescale .000001\nscr_centertime 0\nr_rtshadows 0\nr_radiance 1\nr_radiance_backend 2\n'
               f'r_radiance_quality 0\nr_radiance_grid 1\nr_radiance_point_gain {args.point_gain}\n'
               f'r_radiance_emissive_gain 1\nr_radiance_bounce_gain {args.bounce_gain}\nr_radiance_sky_fallback .15\n'
               f'r_radiance_receiver_filter 1\nr_radiance_receiver_diagnostics 0\nr_radiance_resolution_scale 1\n'
               f'r_radiance_probe_gaussian 0\nr_radiance_checkerboard 0\nr_lightmap {int(not args.normal_textures)}\n'
               f'r_radiance_highlight 0\nfov 90\nviewsize 130\nr_drawviewmodel 0\ncrosshair 0\ncl_bob 0\ncl_rollangle 0\nmap e1m1')
    (root/'id1/boot.cfg').write_text(bench.BOOT.replace('vid_fsaa 0', 'vid_fsaa 1').replace('r_indirect 1', 'r_indirect 0').replace('map start', startup))
    config = SimpleNamespace(binary=frozen, width=args.width, height=args.height, timeout=args.timeout, validation=False, game=None)
    engine, counter = bench.Engine(config, root), None
    output = root/'results.json'
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        counter = FrameCounter(engine.process.pid, frozen)
        clock = rc.SimulationClock(counter, frozen)
        sources = None
        for pose_name in args.poses:
            pose = POSES[pose_name]
            engine.cmd('setpos '+' '.join(map(str, pose)))
            warmed = set()
            for scale in args.scales:
                for gaussian in (0, 2):
                    label = f'{pose_name}-div{rc.receiver_divisor(scale):g}-gaussian{gaussian}'
                    engine.cmd(f'r_radiance_resolution_scale {scale}\nr_radiance_probe_gaussian {gaussian}')
                    deadline = time.monotonic()+args.timeout
                    while True:
                        state = engine.cmd('radiance_stats')
                        try:
                            rc.verify_state(state, 'radiance', 'hardware', 0)
                            rc.verify_receiver_state(state, rc.receiver_name(scale, False), scale, 'texture', True)
                            if not rc.radiance_ready(state): raise RuntimeError('prepass not ready')
                            break
                        except RuntimeError:
                            if time.monotonic() >= deadline: raise
                            time.sleep(.1)
                    family = 'full' if scale == 1 else 'reduced'
                    time.sleep(args.warmup + (args.initial_warmup if family not in warmed else 0))
                    warmed.add(family)
                    before = engine.cmd('radiance_stats')
                    rc.verify_state(before, 'radiance', 'hardware', 0)
                    rc.verify_receiver_state(before, rc.receiver_name(scale, False), scale, 'texture', True)
                    actual_sources = rc.verify_sources(before, args.point_gain, 1, args.bounce_gain)
                    if sources is not None and sources != actual_sources: raise RuntimeError('source counts changed')
                    sources = actual_sources
                    rc.verify_probe_filter(engine.cmd('r_radiance_probe_gaussian'), gaussian)
                    rc.verify_hold(engine.cmd('host_timescale'))
                    rc.verify_receiver_diagnostics_off(engine.cmd('r_radiance_receiver_diagnostics'))
                    viewpos = engine.cmd('viewpos')
                    rc.verify_pose(viewpos, pose)
                    start_clock = clock.sample()
                    capture = engine.photo(label)
                    end_clock = clock.sample()
                    if not 0 <= end_clock-start_clock < .001: raise RuntimeError('simulation advanced during capture')
                    after = engine.cmd('radiance_stats')
                    rc.verify_state(after, 'radiance', 'hardware', 0)
                    rc.verify_receiver_state(after, rc.receiver_name(scale, False), scale, 'texture', True)
                    frames = rc.verify_receiver_frames(rc.receiver_frames(before), rc.receiver_frames(after), scale != 1)
                    report['captures'][label] = {'capture': str(capture), 'pose': pose, 'scale': scale, 'gaussian': gaussian,
                        'viewpos': viewpos, 'sources': sources, 'simulation_start': start_clock, 'simulation_end': end_clock,
                        'receiver_frames': frames, 'before': before, 'after': after,
                        'cvars': engine.cmd('r_lightmap\nr_radiance_highlight\nr_radiance_quality\nr_radiance_grid\nvid_fsaa\nr_indirect')}
                    output.write_text(json.dumps(report, indent=2)+'\n')
                    print(f'Captured {label}: {capture}', flush=True)
        report['complete'] = True
    finally:
        if counter: counter.close()
        engine.close()
        output.write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__': main()
