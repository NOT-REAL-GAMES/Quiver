#!/usr/bin/env python3
"""Check the grazing-sun lintel at the reported start-map camera.

Runs quarter-resolution, texture-filtered, checkerboard Radiance with Gaussian
strength 2 on both traversal backends. Khronos synchronization validation is
injected explicitly, including release builds. Configurations, logs and PAK
links are isolated; the authored sunlight and source PAKs are never changed.
This is a visual regression smoke test, not a performance measurement.
"""
import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import re
import tempfile
import time
from types import SimpleNamespace

from PIL import Image, ImageStat

import bench_meshlets as bench
from bench_radiance import (GAINS, RECEIVER_FILTER, RECEIVER_SCALE, RECEIVER_STATE,
                            ensure_no_game, radiance_ready, sha256, verify_pose,
                            verify_probe_filter, verify_state)
from smoke_radiance_start_spots import bsp_entities

SIZE = (1280, 720)
POSE = (544, 288, 32, -15, 90, 0)
LINTEL_CROP = (570, 235, 710, 277)
UNIFORM_TOLERANCE = .05  # 8-bit grayscale units; corrected reference is exactly zero.
VALIDATION_ENV = {
    'VK_INSTANCE_LAYERS': 'VK_LAYER_KHRONOS_validation',
    'VK_LAYER_ENABLES': 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT',
}
VALIDATION_ERRORS = re.compile(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', re.I)
QUERY = ('viewpos\nr_lightmap\nr_indirect\nvid_fsaa\nr_radiance_sky_fallback\n'
         'r_radiance_probe_gaussian\nr_radiance_receiver_diagnostics\nradiance_stats')


@contextmanager
def validation_environment():
    old = {key: os.environ.get(key) for key in VALIDATION_ENV}
    try:
        os.environ.update(VALIDATION_ENV)
        yield
    finally:
        for key, value in old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def verify_cvar(output, name, expected):
    match = re.search(r'"' + re.escape(name) + r'" is "([^"]+)"', output)
    try:
        actual = float(match[1]) if match else math.nan
    except ValueError:
        actual = math.nan
    if not math.isfinite(actual) or actual != expected:
        raise RuntimeError(f'expected {name}={expected}: {output}')


def verify_capture_state(output, backend, gains, sky_fallback):
    verify_state(output, 'radiance', backend, 0)
    verify_pose(output, POSE)
    verify_probe_filter(output, 2)
    for name, expected in [('r_lightmap', 1), ('r_indirect', 0), ('vid_fsaa', 1),
                           ('r_radiance_sky_fallback', sky_fallback), ('r_radiance_receiver_diagnostics', 0)]:
        verify_cvar(output, name, expected)
    scale = RECEIVER_SCALE.search(output)
    if not scale or tuple(map(float, scale.groups())) != (.25, .25, 4):
        raise RuntimeError(f'quarter-resolution receiver not requested and active: {output}')
    receiver = RECEIVER_STATE.search(output)
    if not receiver or tuple(map(int, receiver.groups())) != (1, 1, 1, 1):
        raise RuntimeError(f'checkerboard receiver not requested and active: {output}')
    filtering = RECEIVER_FILTER.search(output)
    if not filtering or filtering.groups() != ('texture', 'texture'):
        raise RuntimeError(f'texture receiver filter not requested and active: {output}')
    gain_state = GAINS.search(output)
    if not gain_state or tuple(map(float, gain_state.groups())) != gains:
        raise RuntimeError(f'expected point/emissive/bounce gains {gains}: {output}')
    if 'scene_target=RGBA16F' not in output:
        raise RuntimeError(f'expected HDR scene target: {output}')


def lintel_metrics(path):
    with Image.open(path) as source:
        if source.size != SIZE:
            raise RuntimeError(f'expected capture size {SIZE}, got {source.size}: {path}')
        crop = source.convert('L').crop(LINTEL_CROP)
        stats = ImageStat.Stat(crop)
        rows = list(crop.get_flattened_data() if hasattr(crop, 'get_flattened_data') else crop.getdata())
        row_delta = sum(abs(rows[y * crop.width + x] - rows[(y + 1) * crop.width + x])
                        for y in range(crop.height - 1) for x in range(crop.width)) / (crop.width * (crop.height - 1))
        return {'crop': LINTEL_CROP, 'mean': stats.mean[0], 'stddev': stats.stddev[0], 'row_delta': row_delta}


def verify_lintel(metrics):
    if not 1 <= metrics['mean'] < 254:
        raise RuntimeError(f'lintel is empty or clipped instead of the authored-sun fixture: {metrics}')
    if metrics['stddev'] > UNIFORM_TOLERANCE or metrics['row_delta'] > UNIFORM_TOLERANCE:
        raise RuntimeError(f'grazing-sun stippling returned: {metrics}')


def validation_status(output):
    loaded = ('Using VK_LAYER_KHRONOS_validation' in output or
              'Validation Warning: [ VALIDATION-SETTINGS ]' in output or
              re.search(r'Insert instance layer.*VK_LAYER_KHRONOS_validation', output) is not None)
    return {'layer_loaded': loaded, 'error_matches': VALIDATION_ERRORS.findall(output),
            'synchronization_requested': True, 'environment': VALIDATION_ENV}


def run_backend(args, root, paks, backend):
    work = root / backend
    (work / 'id1').mkdir(parents=True)
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    settings = (f'r_rtshadows 0\nr_radiance 1\nr_radiance_backend {2 if backend == "hardware" else 1}\n'
                'r_radiance_quality 0\nr_radiance_hdr 1\nr_radiance_receiver_filter 1\n'
                'r_radiance_resolution_scale 0.25\nr_radiance_checkerboard 1\nr_radiance_probe_gaussian 2\n'
                'r_radiance_receiver_diagnostics 0\nr_radiance_point_gain 0\nr_radiance_emissive_gain 0\n'
                'r_radiance_bounce_gain 0\nr_radiance_sky_fallback 0\nr_lightmap 1\n'
                'host_timescale 0.000001\nhost_maxfps 60\nfov 90\nmap start')
    boot = bench.BOOT.replace('r_indirect 1', 'r_indirect 0').replace('vid_fsaa 0', 'vid_fsaa 1')
    (work / 'id1/boot.cfg').write_text(boot.replace('map start', settings))
    config = SimpleNamespace(binary=args.binary, width=SIZE[0], height=SIZE[1], timeout=args.timeout,
                             validation=True, game=None)
    result = {'backend': backend, 'work': str(work), 'passed': False}
    engine = None
    try:
        # The child receives validation settings; the caller's environment is restored.
        with validation_environment():
            engine = bench.Engine(config, work)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.view('start', ' '.join(map(str, POSE)))
        engine.cmd('vid_fsaa 1\nvid_restart')

        def ready_state(gains, sky):
            deadline = time.monotonic() + args.timeout
            last = ''
            while time.monotonic() < deadline:
                last = engine.cmd(QUERY)
                try:
                    verify_capture_state(last, backend, gains, sky)
                    if radiance_ready(last):
                        return last
                except RuntimeError:
                    pass
                time.sleep(.1)
            raise RuntimeError(f'{backend} did not reach the requested stable receiver state: {last}')

        ready_state((0, 0, 0), 0)
        time.sleep(1)
        state = ready_state((0, 0, 0), 0)
        capture = engine.photo(backend + '-isolated')
        metrics = lintel_metrics(capture)
        result['isolated'] = {'capture': str(capture), 'metrics': metrics, 'state': state}
        (work / 'isolated-stats.txt').write_text(state)
        verify_lintel(metrics)
        print(f'{backend}: lintel stddev={metrics["stddev"]:.6g}, row_delta={metrics["row_delta"]:.6g}', flush=True)

        engine.cmd('r_radiance_point_gain 7\nr_radiance_emissive_gain 1\nr_radiance_bounce_gain 8\nr_radiance_sky_fallback 1')
        ready_state((7, 1, 8), 1)
        time.sleep(2)
        state = ready_state((7, 1, 8), 1)
        capture = engine.photo(backend + '-full-lighting')
        result['full_lighting'] = {'capture': str(capture), 'state': state}
        (work / 'full-lighting-stats.txt').write_text(state)
        result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
    finally:
        if engine is not None:
            try:
                engine.close()
            except Exception as error:
                result['passed'] = False
                result['shutdown_error'] = str(error)
        output = (work / 'output.log').read_text(errors='replace') if (work / 'output.log').exists() else ''
        if engine is not None:
            output += engine.text()
        result['validation'] = validation_status(output)
        if not result['validation']['layer_loaded'] or result['validation']['error_matches']:
            result['passed'] = False
            result['validation_error'] = 'validation layer missing or errors reported through teardown'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--backend', choices=('hardware', 'software', 'both'), default='both')
    parser.add_argument('--output', type=Path, help='new directory for isolated games, captures and summary')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--allow-existing', action='store_true', help='caller has arranged exclusive GPU use with other game processes suspended')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted((p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower())),
                  key=lambda p: int(re.search(r'\d+', p.name).group()))
    if not args.binary.is_file() or not paks:
        parser.error('an existing executable and Quake PAKs are required')
    ensure_no_game(args.allow_existing)
    entities = bsp_entities(paks)
    sun = re.search(r'"_sunlight"\s*"([^"]+)"', entities)
    try:
        sunlight = tuple(map(float, sun[1].split())) if sun else ()
    except ValueError:
        sunlight = ()
    if len(sunlight) not in (1, 3) or not all(map(math.isfinite, sunlight)) or not any(value > 0 for value in sunlight):
        parser.error('start.bsp must contain the positive authored _sunlight used by this regression fixture')
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='quiver-sky-grazing-')).resolve()
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    report = {'binary': {'path': str(args.binary), 'sha256': sha256(args.binary)},
              'paks': [{'path': str(pak), 'sha256': sha256(pak)} for pak in paks],
              'resolution': SIZE, 'pose': POSE, 'authored_sunlight': sunlight,
              'authored_sunlight_retained': True, 'uniform_tolerance': UNIFORM_TOLERANCE,
              'settings': {'receiver_scale': .25, 'receiver_filter': 'texture', 'checkerboard': True,
                           'probe_gaussian': 2, 'r_lightmap': 1, 'r_indirect': 0, 'samples': 1},
              'backends': {}, 'complete': False}
    summary = root / 'summary.json'
    summary.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Isolated results: {root}', flush=True)
    try:
        for backend in ('hardware', 'software') if args.backend == 'both' else (args.backend,):
            report['backends'][backend] = run_backend(args, root, paks, backend)
            summary.write_text(json.dumps(report, indent=2) + '\n')
        report['binary']['sha256_after'] = sha256(args.binary)
        report['complete'] = (report['binary']['sha256'] == report['binary']['sha256_after'] and
                              all(item['passed'] for item in report['backends'].values()))
    finally:
        summary.write_text(json.dumps(report, indent=2) + '\n')
    if not report['complete']:
        raise RuntimeError(f'sky regression smoke failed; inspect {summary}')
    print(f'PASS: authored-sun lintel and full-lighting captures, active fast receiver modes, validation through teardown: {summary}', flush=True)


if __name__ == '__main__':
    main()
