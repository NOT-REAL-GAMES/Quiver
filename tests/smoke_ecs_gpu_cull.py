#!/usr/bin/env python3
"""Compare CPU and GPU instance culling on the playable ripple levels.

Requires a built engine, the ripple gamecode, local Quake PAKs, Pillow and a
Vulkan display. The game directory and config are disposable. The same paused
scene is captured with each path at several camera angles, including views
where the frustum cuts through the instance grid.
"""
import argparse
from pathlib import Path
import re
import tempfile
import time

from PIL import Image, ImageChops

import bench_ecs_ripple as ripple
import bench_meshlets as bench


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--progs', type=Path, required=True)
    parser.add_argument('--count', type=int, choices=(1024, 32768), default=1024)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 1280, 720, 'quiver_ripple'
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(x for x in source.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks or not args.progs.is_file():
        parser.error('Quake PAKs and compiled ripple gamecode are required')
    work = Path(tempfile.mkdtemp(prefix='quiver-gpu-instance-parity-')).resolve()
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / args.game).mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / args.game / 'progs.dat').symlink_to(args.progs.resolve())
    startup = f'r_rtshadows 0\nmax_edicts 32800\nhost_maxfps 30\nmap ecs{args.count}'
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', startup))
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        ripple.check_state(engine, args.count)
        ripple.check_pixels(engine, args.width, args.height)

        def gpu_stats():
            output = engine.cmd('ecs_gpu_stats')
            match = re.search(r'ECS GPU cull: enabled=(\d+) available=(\d+) active=(\d+) dispatches=(\d+)', output)
            if not match:
                raise RuntimeError(f'missing GPU culling diagnostics: {output}')
            return tuple(map(int, match.groups()))

        # Each camera is set while unpaused and then held steady for both
        # captures. Pitches and yaws exercise all four frustum boundaries.
        views = [(-480, 0, 32, 0, 0, 0), (-200, 0, 128, 0, 45, 0),
                 (0, -100, 96, -25, 90, 0), (200, 120, 96, 20, 180, 0)]
        for number, view in enumerate(views):
            engine.cmd('setpos ' + ' '.join(map(str, view)) + '\nr_drawviewmodel 0\nr_particles 0')
            time.sleep(.25)
            engine.cmd('pause')
            captures = []
            for value in (0, 1):
                setting = engine.cmd(f'r_ecs_gpu_cull {value}\nr_ecs_gpu_cull')
                if 'Unknown command' in setting:
                    raise RuntimeError(f'GPU culling control unavailable: {setting}')
                time.sleep(.2)
                state = gpu_stats()
                if value == 0 and (state[0] != 0 or state[2] != 0):
                    raise RuntimeError(f'CPU culling path did not activate: {state}')
                if value == 1:
                    if state[:3] != (1, 1, 1):
                        raise RuntimeError(f'GPU culling silently fell back: {state}')
                    time.sleep(.1)
                    if gpu_stats()[3] <= state[3]:
                        raise RuntimeError(f'GPU culling did not continue dispatching: {state}')
                captures.append(engine.photo(f'view-{number}-gpu-{value}'))
            with Image.open(captures[0]) as cpu, Image.open(captures[1]) as gpu:
                difference = ImageChops.difference(cpu.convert('RGB'), gpu.convert('RGB'))
                if difference.getbbox():
                    raise RuntimeError(f'CPU/GPU culling mismatch in {difference.getbbox()}: {captures}')
            engine.cmd('pause')
        engine.cmd('r_ecs_batch 0')
        time.sleep(.2)
        if gpu_stats()[2] != 0:
            raise RuntimeError('GPU culling remained active with the instanced path disabled')
        engine.cmd('r_ecs_batch 1\nr_showtris 1')
        time.sleep(.2)
        if gpu_stats()[2] != 0:
            raise RuntimeError('GPU culling remained active in wireframe mode')
        engine.cmd('r_showtris 0\nr_tasks 0')
        time.sleep(.2)
        if gpu_stats()[2] != 0:
            raise RuntimeError('GPU culling remained active in serial rendering')
        engine.cmd('r_tasks 1\nvid_restart')
        time.sleep(.5)
        state = gpu_stats()
        if state[:3] != (1, 1, 1):
            raise RuntimeError(f'GPU culling did not recover after a video restart: {state}')
        time.sleep(.1)
        if gpu_stats()[3] <= state[3]:
            raise RuntimeError('GPU culling stopped dispatching after a video restart')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace') + engine.text()
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('Vulkan validation unavailable or reported errors')
        print(f'PASS: {args.count} entities, {len(views)} paused frustum views; CPU/GPU pixels identical', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate()
            engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__':
    main()
