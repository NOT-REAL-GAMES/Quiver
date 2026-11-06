#!/usr/bin/env python3
"""Exercise local batched ECS aliases as Radiance occluder candidates.

Requires a built engine, installed Quake PAKs and ripple progs.dat. Every run
uses a disposable game directory and leaves the supplied files untouched.
"""
import argparse
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--basedir', required=True, type=Path)
    parser.add_argument('--progs', required=True, type=Path)
    parser.add_argument('--count', type=int, choices=(1024, 32768), default=1024)
    parser.add_argument('--backend', choices=('auto', 'software', 'hardware'), default='auto')
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--capture', action='store_true')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.progs = args.progs.resolve()
    args.width, args.height, args.game = 640, 360, 'quiver_ripple'
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    if not args.binary.is_file() or not args.progs.is_file() or not source.is_dir():
        parser.error('binary, progs.dat or basedir does not exist')
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs in {source}')

    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-ecs-occluders-')).resolve()
    print(f'isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / args.game).mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / args.game / 'progs.dat').symlink_to(args.progs)
    backend = {'auto': 0, 'software': 1, 'hardware': 2}[args.backend]
    startup = (f'r_rtshadows 0\nmax_edicts 32800\nhost_maxfps 30\n'
               f'r_radiance_backend {backend}\nr_radiance_quality 0\nr_radiance 1\nmap ecs{args.count}')
    (work / 'id1' / 'boot.cfg').write_text(bench.BOOT.replace('map start', startup))
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.cmd('setpos -480 0 32 0 0 0')
        deadline = time.monotonic() + args.timeout
        while True:
            output = engine.cmd('ecs_visual_stats\nradiance_stats')
            state = re.search(r'Radiance: requested=1 active=1 backend=(\S+)', output)
            batch = re.search(r'ECS visuals: local=(\d+).*batched=(\d+)', output)
            totals = re.search(r'Radiance: tracked_memory=.*? dynamic=(\d+) omitted=(\d+)', output)
            detail = re.search(r'Radiance: occluder candidates=(\d+) ECS_batch=(\d+) '
                               r'omitted limit=(\d+) unsupported=(\d+) transparent=(\d+)', output)
            if state and batch and totals and detail and int(batch[1]) == args.count and int(batch[2]) == 1:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(f'Radiance batch did not become active: {output}')
            time.sleep(.2)

        if args.backend != 'auto' and state[1] != {'software': 'software', 'hardware': 'ray-query'}[args.backend]:
            raise RuntimeError(f'wrong backend: {output}')
        selected, omitted = map(int, totals.groups())
        considered, batch_count, limit, unsupported, transparent = map(int, detail.groups())
        if batch_count != args.count or considered < batch_count:
            raise RuntimeError(f'batched entities missing from candidate list: {output}')
        # At most this many selected instances could be ordinary entities. A
        # larger selected count proves at least one batch alias was extracted.
        ordinary = considered - batch_count
        selected_batch_min = max(0, selected - ordinary)
        if selected != 256 or not selected_batch_min:
            raise RuntimeError(f'no batch alias selected under the shared budget: {output}')
        if omitted != limit + unsupported + transparent or omitted < args.count - 256:
            raise RuntimeError(f'overflow omissions are inconsistent: {output}')
        print(output.strip(), flush=True)
        print(f'PASS: {batch_count} batch candidates; at least {selected_batch_min} selected; '
              f'{omitted} omitted', flush=True)
        if args.capture:
            print(f'screenshot: {engine.photo("ecs-occluders")}', flush=True)
        if args.validation:
            text = engine.text() + (work / 'output.log').read_text(errors='replace')
            if ('Using VK_LAYER_KHRONOS_validation' not in text or
                    re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', text, re.I)):
                raise RuntimeError(f'validation missing or reported errors; inspect {work}')
    finally:
        engine.close()


if __name__ == '__main__':
    main()
