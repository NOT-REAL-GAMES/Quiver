#!/usr/bin/env python3
"""Run the fixed-camera receiver-filter acceptance matrix on one frozen binary.

The default matrix has 64 reduced settings grouped into eight camera/viewport/
Gaussian cohorts. Each cohort measures full references once per paired row
and reuses them for all receiver divisors and checkerboard settings. Whole-frame
timings are authoritative; use a separate --gpu-stages run for diagnostics.
"""
import argparse
import itertools
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import bench_radiance as bench


CAMERAS = {
    'doorway': ('e1m1', (607, 143, 40, 3, 91, 0)),
    'shaft': ('e4m1', (1312, 680, 688, 55, 90, 0)),
    'start': ('start', (544, 288, 32, -15, 90, 0)),
}
PRIMARY_CAMERAS = ('doorway', 'shaft')


def viewport(value):
    match = re.fullmatch(r'(\d+)[xX](\d+)', value)
    if not match or int(match[1]) < 320 or int(match[2]) < 200:
        raise argparse.ArgumentTypeError('viewport must be WIDTHxHEIGHT, at least 320x200')
    return tuple(map(int, match.groups()))


def workloads(cameras, resolutions, scales, checkerboards, gaussians):
    result = []
    for camera, size, scale, checker, gaussian in itertools.product(cameras, resolutions, scales, checkerboards, gaussians):
        if scale == 1:
            raise ValueError('matrix receiver scales must be reduced')
        scene, pose = CAMERAS[camera]
        divisor = bench.receiver_divisor(scale)
        name = f'{camera}-{size[0]}x{size[1]}-div{divisor:g}-checker{checker}-gaussian{gaussian:g}'
        result.append({'name': name, 'camera': camera, 'scene': scene, 'pose': list(pose), 'resolution': list(size),
                       'resolution_scale': scale, 'divisor': divisor, 'checkerboard': bool(checker), 'gaussian': gaussian})
    if len({item['name'] for item in result}) != len(result):
        raise ValueError('matrix selections contain duplicate workloads')
    return result


def cohorts(items):
    groups = {}
    for item in items:
        name = f'{item["camera"]}-{item["resolution"][0]}x{item["resolution"][1]}-gaussian{item["gaussian"]:g}'
        group = groups.setdefault(name, {'name': name, 'scene': item['scene'], 'pose': item['pose'],
            'resolution': item['resolution'], 'gaussian': item['gaussian'], 'scales': [], 'checkerboards': [], 'settings': []})
        if item['resolution_scale'] not in group['scales']: group['scales'].append(item['resolution_scale'])
        if int(item['checkerboard']) not in group['checkerboards']: group['checkerboards'].append(int(item['checkerboard']))
        group['settings'].append(item['name'])
    return list(groups.values())


def command(args, binary, output, item):
    cmd = [sys.executable, str(Path(__file__).with_name('bench_radiance.py')),
           '--binary', str(binary), '--basedir', str(args.basedir.resolve()),
           '--scenes', item['scene'], '--pose', *map(str, item['pose']),
           '--width', str(item['resolution'][0]), '--height', str(item['resolution'][1]),
           '--receiver-scales', *(f'{scale:.12g}' for scale in item['scales']),
           '--receiver-checkerboards', *map(str, item['checkerboards']),
           '--probe-gaussian', str(item['gaussian']), '--compare-receiver-filters', '--hold-simulation',
           '--backend', args.backend, '--quality', str(args.quality), '--grid', str(args.grid),
           '--point-gain', str(args.point_gain), '--emissive-gain', str(args.emissive_gain),
           '--bounce-gain', str(args.bounce_gain), '--sky-fallback', str(args.sky_fallback),
           '--pairs', str(args.pairs), '--seconds', str(args.seconds), '--warmup', str(args.warmup),
           '--initial-warmup', str(args.initial_warmup), '--prepass-samples', str(args.prepass_samples),
           '--timeout', str(args.timeout), '--output', str(output)]
    if args.production_full_reference: cmd.append('--production-full-reference')
    if not args.legacy_reference: cmd.append('--no-legacy-reference')
    if args.capture: cmd.append('--capture')
    if args.gpu_stages: cmd.append('--gpu-stages')
    if args.validation: cmd.append('--validation')
    if args.allow_existing: cmd.append('--allow-existing')
    return cmd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--basedir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--resume', action='store_true', help='resume the identical matrix using its saved binary')
    parser.add_argument('--diagnostic', action='store_true', help='allow fewer than five pairs for initial screening')
    parser.add_argument('--cameras', nargs='+', choices=tuple(CAMERAS), default=list(PRIMARY_CAMERAS))
    parser.add_argument('--resolutions', nargs='+', type=viewport, default=[(1280, 720), (1720, 1370)])
    parser.add_argument('--scales', nargs='+', type=bench.resolution_scale, default=[2/3, .5, 1/3, .25])
    parser.add_argument('--checkerboards', nargs='+', type=int, choices=(0, 1), default=[0, 1])
    parser.add_argument('--gaussians', nargs='+', type=bench.probe_gaussian_strength, default=[1, 2])
    parser.add_argument('--backend', choices=('auto', 'software', 'hardware'), default='hardware')
    parser.add_argument('--quality', type=int, choices=(0, 1, 2), default=1)
    parser.add_argument('--grid', type=int, choices=(0, 1), default=0)
    parser.add_argument('--point-gain', type=float, default=1)
    parser.add_argument('--emissive-gain', type=float, default=1)
    parser.add_argument('--bounce-gain', type=float, default=1)
    parser.add_argument('--sky-fallback', type=float, default=.15)
    parser.add_argument('--pairs', type=int, default=5)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--warmup', type=float, default=5)
    parser.add_argument('--initial-warmup', type=float, default=10)
    parser.add_argument('--prepass-samples', type=int, default=0)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--capture', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--production-full-reference', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--legacy-reference', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--gpu-stages', action='store_true')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--allow-existing', action='store_true')
    args = parser.parse_args()
    if args.pairs < (1 if args.diagnostic else 5) or not math.isfinite(args.seconds) or args.seconds <= 0 or not math.isfinite(args.warmup) or args.warmup <= 0 or not math.isfinite(args.initial_warmup) or args.initial_warmup < 10:
        parser.error('acceptance runs require at least 5 pairs (or --diagnostic), positive samples/warmups, and 10-second initial warmups')
    try:
        items = workloads(args.cameras, args.resolutions, args.scales, args.checkerboards, args.gaussians)
    except ValueError as exc:
        parser.error(str(exc))
    groups = cohorts(items)
    binary, root = args.binary.resolve(), args.output.resolve()
    if not binary.is_file(): parser.error(f'binary not found: {binary}')
    settings = {key: value for key, value in vars(args).items() if key not in {'binary', 'basedir', 'output', 'resume'}}
    # Round-trip tuples to the same JSON representation used for resume checks.
    settings = json.loads(json.dumps(settings))
    settings['basedir'] = str(args.basedir.resolve())
    result_path, frozen = root / 'matrix.json', root / 'binary/vkquake'
    if args.resume:
        report = json.loads(result_path.read_text())
        if report['settings'] != settings or report['workloads'] != items or report['cohorts'] != groups:
            parser.error('resume settings differ from the saved matrix')
        if not frozen.is_file() or bench.sha256(frozen) != report['binary']['sha256']:
            parser.error('saved benchmark binary is missing or changed')
    else:
        root.mkdir(parents=True, exist_ok=False)
        frozen.parent.mkdir()
        shutil.copy2(binary, frozen)
        report = {'binary': {'source_path': str(binary), 'path': str(frozen), 'sha256': bench.sha256(frozen)},
                  'settings': settings, 'workloads': items, 'cohorts': groups, 'results': {}, 'complete': False,
                  'acceptance_timing': not args.diagnostic and args.pairs >= 5,
                  'authoritative_timing': not args.gpu_stages and not args.validation}
        try:
            report['gpu'] = subprocess.check_output(['nvidia-smi', '--query-gpu=name,driver_version', '--format=csv,noheader'], text=True).strip()
        except (FileNotFoundError, subprocess.CalledProcessError):
            report['gpu'] = None
    report['complete'] = False
    result_path.write_text(json.dumps(report, indent=2) + '\n')
    try:
        for number, item in enumerate(groups, 1):
            output = root / item['name']
            saved = report['results'].get(item['name'], {})
            if saved.get('complete'):
                continue
            if output.exists():
                suffix = 1
                while output.with_name(output.name + f'.incomplete-{suffix}').exists(): suffix += 1
                output.rename(output.with_name(output.name + f'.incomplete-{suffix}'))
            cmd = command(args, frozen, output, item)
            print(f'Cohort {number}/{len(groups)} ({len(item["settings"])} reduced settings): {item["name"]}', flush=True)
            started = time.monotonic()
            run = subprocess.run(cmd)
            child_path = output / 'results.json'
            child = json.loads(child_path.read_text()) if child_path.exists() else {}
            report['results'][item['name']] = {'complete': run.returncode == 0 and bool(child.get('complete')),
                'returncode': run.returncode, 'elapsed_s': time.monotonic() - started, 'result_path': str(child_path),
                'command': cmd, 'summary': child.get('scenes', {}).get(item['scene'], {}).get('summary')}
            result_path.write_text(json.dumps(report, indent=2) + '\n')
            if not report['results'][item['name']]['complete']:
                raise RuntimeError(f'matrix stopped at {item["name"]}; own benchmark exited {run.returncode}; inspect {output}')
        report['complete'] = True
    finally:
        result_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Completed matrix: {result_path}', flush=True)


if __name__ == '__main__': main()
