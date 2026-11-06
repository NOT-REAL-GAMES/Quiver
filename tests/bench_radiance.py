#!/usr/bin/env python3
"""Compare lighting paths or receiver-filter strengths in alternating runs.

Requires Linux, an unstripped vkQuake binary, a Vulkan display, SDL development
headers and Quake PAKs. Ripple scenes additionally require the compiled ripple
progs.dat. Each scene uses its own temporary game and configuration directory.
The whole-frame GPU timer includes all passes; the radiance prepass samples are
separate diagnostics and exclude per-fragment receiver shading.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import random
import statistics
import struct
import subprocess
import tempfile
import time
from types import SimpleNamespace

SCENES = ('start', 'e1m1', 'e4m1', 'ecs1024', 'ecs32768')
GPU_MEAN = re.compile(r'GPU frame time:\s*(\d+) samples,\s*([\d.]+) ms mean')
GPU_MEDIAN = re.compile(r'GPU frame median:\s*([\d.]+) ms(?:\s*\((\d+)/(\d+) samples\))?')
GPU_P95 = re.compile(r'GPU frame p95:\s*([\d.]+) ms(?:\s*\((\d+)/(\d+) samples\))?')
PREPASS = re.compile(r'Radiance:\s*completed GPU prepass=([\d.]+) ms')
STATE = re.compile(r'Radiance:\s*requested=(\d+)\s+active=(\d+)\s+backend=(\S+)')
QUALITY = re.compile(r'\bquality=(-?\d+)\b')
STATIC_SCENE = re.compile(r'BLAS_upload static=(\d+) B dynamic=\d+ B; builds static=(\d+)')
GPU_STAGE = re.compile(r'GPU (world|entities|receiver|receiver_classify|receiver_shade|receiver_filter) stage:\s*(\d+) samples,\s*([\d.]+) ms mean,\s*([\d.]+) ms median(?:,\s*([\d.]+) ms p95)?\s*\((\d+)/(\d+) samples\)')
PROBE_FILTER = re.compile(r'"r_radiance_probe_gaussian" is "([^"]+)"')
RECEIVER_STATE = re.compile(r'half-resolution receiver requested=(\d+) active=(\d+) checkerboard requested=(\d+) active=(\d+)')
RECEIVER_SCALE = re.compile(r'receiver scale requested=(\S+) active=(\S+) divisor=(\S+)')
RECEIVER_FILTER = re.compile(r'receiver filter requested=(probe|texture) active=(probe|texture|full)')
GAINS = re.compile(r'gains point=(\S+) emissive=(\S+) bounce=(\S+)')
SOURCES = re.compile(r'\blights=(\d+) emissive=(\d+) emissive_omitted=(\d+)')
RECEIVER_FRAMES = re.compile(r'receiver frames reduced=(\d+) full=(\d+)')
MODEL_RECEIVER = re.compile(r'deferred opaque requested=(\d+) models_active=(\d+) draws=(\d+) triangles=(\d+) frames_active=(\d+) frames_fallback=(\d+)')
VIEWPOS = re.compile(r'Viewpos:\s*\((-?\d+) (-?\d+) (-?\d+)\) (-?\d+) (-?\d+) (-?\d+)')


def verify_pose(output, expected):
    match = VIEWPOS.search(output)
    if not match or any(abs(float(actual) - wanted) > 1.1 for actual, wanted in zip(match.groups(), expected)):
        raise RuntimeError(f'benchmark camera moved from {expected}: {output}')


def verify_hold(output):
    match = re.search(r'"host_timescale" is "([^"]+)"', output)
    try:
        actual = float(match[1]) if match else math.nan
    except ValueError:
        actual = math.nan
    if not math.isclose(actual, 1e-6, rel_tol=1e-5):
        raise RuntimeError(f'benchmark simulation timescale is not held: {output}')


def verify_receiver_diagnostics_off(output):
    match = re.search(r'"r_radiance_receiver_diagnostics" is "([^"]+)"', output)
    try:
        actual = float(match[1]) if match else math.nan
    except ValueError:
        actual = math.nan
    if actual != 0:
        raise RuntimeError(f'authoritative receiver benchmark requires diagnostics disabled: {output}')


def receiver_frames(output):
    match = RECEIVER_FRAMES.search(output)
    if not match or any(int(value) > 0xffffffff for value in match.groups()):
        raise RuntimeError(f'actual receiver submission counters unavailable: {output}')
    return {'reduced': int(match[1]), 'full': int(match[2])}


def verify_receiver_frames(before, after, reduced):
    delta = {name: (after[name]-before[name]) & 0xffffffff for name in ('reduced', 'full')}
    if reduced and (delta['full'] or not delta['reduced']):
        raise RuntimeError(f'full-resolution receiver submissions occurred during a reduced sample, or no reduced frames completed: {delta}')
    if not reduced and (delta['reduced'] or not delta['full']):
        raise RuntimeError(f'reduced receiver submissions occurred during a full-resolution sample, or no full frames completed: {delta}')
    return {'before': before, 'after': after, 'delta': delta}


def model_receiver_state(output):
    match = MODEL_RECEIVER.search(output)
    if not match:
        raise RuntimeError('actual deferred model state/counters unavailable')
    names = ('requested', 'active', 'draws', 'triangles', 'frames_active', 'frames_fallback')
    result = dict(zip(names, map(int, match.groups())))
    if any(value > 0xffffffff for value in result.values()) or result['requested'] > 1 or result['active'] > 1:
        raise RuntimeError('invalid deferred model state/counters')
    return result


def verify_model_receiver_state(output, requested, active, minimum_triangles=1):
    result = model_receiver_state(output)
    if (result['requested'], result['active']) != (int(requested), int(active)):
        raise RuntimeError(f'wrong actual deferred model mode: {result}')
    if active and (not result['draws'] or result['triangles'] < minimum_triangles):
        raise RuntimeError(f'deferred model path contains insufficient geometry: {result}')
    return result


def verify_model_receiver_frames(before, after, active):
    keys = ('frames_active', 'frames_fallback')
    delta = {key: (after[key] - before[key]) & 0xffffffff for key in keys}
    wanted, unwanted = keys if active else keys[::-1]
    if not delta[wanted] or delta[unwanted]:
        raise RuntimeError(f'wrong actual deferred model submissions: expected {wanted}, deltas={delta}')
    return {'before': {key: before[key] for key in keys}, 'after': {key: after[key] for key in keys}, 'delta': delta}


class SimulationClock:
    """Read cl.time through the benchmark's existing read-only process FD."""
    def __init__(self, counter, binary):
        symbols = subprocess.check_output(['nm', '-an', str(binary)], text=True)
        addresses = {name: int(address, 16) for address, name in re.findall(r'^([0-9a-fA-F]+)\s+[bBdD]\s+(cl|host_framecount)$', symbols, re.M)}
        dwarf = subprocess.check_output(['gdb', '-nx', '-batch', str(binary), '-ex', 'p/x &((client_state_t *)0)->time'], text=True, stderr=subprocess.STDOUT)
        offset = re.search(r'\$\d+\s*=\s*(0x[0-9a-fA-F]+)', dwarf)
        if not offset or set(addresses) != {'cl', 'host_framecount'}:
            raise RuntimeError('held simulation verification requires cl/host_framecount symbols and client_state_t debug information')
        self.fd = counter.fd
        self.address = counter.address - addresses['host_framecount'] + addresses['cl'] + int(offset[1], 16)

    def sample(self):
        data = os.pread(self.fd, 8, self.address)
        if len(data) != 8: raise RuntimeError('could not read simulation clock')
        value = struct.unpack('=d', data)[0]
        if not math.isfinite(value): raise RuntimeError('simulation clock is not finite')
        return value


def probe_gaussian_strength(value):
    try:
        strength = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError('probe Gaussian strength must be a number in 0..2') from exc
    if not math.isfinite(strength) or not 0 <= strength <= 2:
        raise argparse.ArgumentTypeError('probe Gaussian strength must be finite and in 0..2')
    return strength


def verify_probe_filter(output, expected):
    match = PROBE_FILTER.search(output)
    try:
        actual = float(match[1]) if match else math.nan
    except ValueError:
        actual = math.nan
    if not math.isfinite(actual) or not math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-7):
        raise RuntimeError(f'expected probe filter strength {expected}: {output}')


def resolution_scale(value):
    fractions = {'2/3': 2/3, '1/3': 1/3}
    if str(value).strip() in fractions:
        return fractions[str(value).strip()]
    try:
        scale = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError('receiver scale must be 1, 2/3, 0.5, 1/3, or 0.25') from exc
    # Retain the short spelling commonly entered at the game console.
    if scale == .666:
        return 2/3
    for supported in (1, 2/3, .5, 1/3, .25):
        if math.isfinite(scale) and math.isclose(scale, supported, rel_tol=1e-5, abs_tol=1e-7):
            return supported
    raise argparse.ArgumentTypeError('receiver scale must be 1, approximately 0.666666667, 0.5, approximately 0.333333333, or 0.25')


def receiver_divisor(scale):
    """Keep integral legacy metadata, with a real divisor1.5 for two-thirds."""
    value = 1 / resolution_scale(scale)
    return int(value) if value.is_integer() else value


def receiver_extent(extent, scale):
    """Exact rational ceiling, including partial one/two-pixel blocks."""
    numerator, denominator = {1: (1, 1), 2/3: (2, 3), .5: (1, 2),
                              1/3: (1, 3), .25: (1, 4)}[resolution_scale(scale)]
    return (extent * numerator + denominator - 1) // denominator


def verify_receiver_state(output, receiver, scale=None, receiver_filter=None, strict=False, *,
                          full_texture=False, checkerboard_requested=None):
    if receiver == 'full' and not strict:
        return
    state = RECEIVER_STATE.search(output)
    reduced, checker = int(receiver != 'full'), int(receiver == 'checkerboard')
    requested_checker = checker if checkerboard_requested is None else int(checkerboard_requested)
    expected = (reduced, reduced, requested_checker, checker)
    if not state or tuple(map(int, state.groups())) != expected:
        raise RuntimeError(f'{receiver} receiver path not active: {output}')
    if strict:
        scale = scale if scale is not None else (0.5 if reduced else 1.0)
        size, filtering = RECEIVER_SCALE.search(output), RECEIVER_FILTER.search(output)
        try:
            scales = tuple(float(size[i]) for i in (1, 2, 3)) if size else ()
        except ValueError:
            scales = ()
        if (len(scales) != 3 or
                any(not math.isclose(value, scale, rel_tol=1e-5, abs_tol=1e-7) for value in scales[:2]) or
                not math.isclose(scales[2], receiver_divisor(scale), rel_tol=1e-6, abs_tol=1e-7)):
            raise RuntimeError(f'expected active receiver scale {scale}: {output}')
        if full_texture and receiver_filter != 'texture':
            raise RuntimeError('full texture receiver assertion requires texture filtering')
        expected_filter = receiver_filter if reduced or full_texture else 'full'
        if not filtering or filtering[1] != receiver_filter or filtering[2] != expected_filter:
            raise RuntimeError(f'expected receiver filter requested={receiver_filter} active={expected_filter}: {output}')


def verify_sources(output, point_gain, emissive_gain, bounce_gain):
    gains, sources = GAINS.search(output), SOURCES.search(output)
    try:
        actual = tuple(float(value) for value in gains.groups()) if gains else ()
    except ValueError:
        actual = ()
    expected = (point_gain, emissive_gain, bounce_gain)
    if len(actual) != 3 or any(not math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-7) for a, b in zip(actual, expected)) or not sources:
        raise RuntimeError(f'Radiance gains or source counts unavailable or changed: {output}')
    return tuple(map(int, sources.groups()))


def receiver_name(scale, checkerboard):
    return 'full' if scale == 1 else 'checkerboard' if checkerboard else 'half'


def comparison_order(pair, modes):
    """Balance both ordering position and predecessor across a full cycle."""
    if len(modes) == 2:
        return alternating_order(pair, *modes)
    count = len(modes)
    offsets = [0] + [(index + 1) // 2 if index % 2 else count - index // 2 for index in range(1, count)]
    row = tuple(modes[(index + (pair if count % 2 == 0 else pair // 2)) % count] for index in offsets)
    return row[::-1] if count % 2 and pair % 2 else row


def receiver_comparisons(rows):
    comparisons = {}
    for texture in (name for name in rows[0] if name.startswith('texture_')):
        suffix = texture[len('texture_'):]
        probe = 'probe_' + suffix
        label = '' if suffix == 'reduced' else '_' + suffix
        for reference in ('full_direct', 'full_indirect'):
            if reference in rows[0]:
                for filtering, mode in (('probe', probe), ('texture', texture)):
                    if mode in rows[0]:
                        comparisons[f'{filtering}{label}_vs_{reference}'] = summarize_pairs(rows, reference, mode)
        if probe in rows[0]: comparisons[f'texture{label}_vs_probe'] = summarize_pairs(rows, probe, texture)
    return comparisons


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def parse_gpu(output):
    mean = GPU_MEAN.search(output)
    if not mean:
        raise RuntimeError(f'GPU timestamps unavailable: {output}')
    result = {'samples': int(mean[1]), 'mean_ms': float(mean[2])}
    if not result['samples']:
        raise RuntimeError('GPU timer reported no completed frames')
    median = GPU_MEDIAN.search(output)
    if median:
        result['median_ms'] = float(median[1])
        if median[2]:
            result['median_samples'], result['total_samples'] = int(median[2]), int(median[3])
            result['median_truncated'] = result['median_samples'] < result['total_samples']
    else:
        result['median_ms'] = None
    p95 = GPU_P95.search(output)
    result['p95_ms'] = float(p95[1]) if p95 else None
    if p95 and p95[2]:
        result['p95_samples'], result['p95_total_samples'] = int(p95[2]), int(p95[3])
        result['p95_truncated'] = result['p95_samples'] < result['p95_total_samples']
    return result


def verify_state(output, mode, requested_backend, requested_quality=None):
    state = STATE.search(output)
    expected = int(mode == 'radiance')
    if not state or int(state[1]) != expected or int(state[2]) != expected:
        raise RuntimeError(f'{mode} renderer not active: {output}')
    backend = state[3]
    if expected and requested_backend != 'auto' and backend != {'software': 'software', 'hardware': 'ray-query'}[requested_backend]:
        raise RuntimeError(f'expected {requested_backend} backend, got {backend}: {output}')
    quality = QUALITY.search(output)
    if expected and requested_quality is not None and (not quality or int(quality[1]) != requested_quality):
        raise RuntimeError(f'expected radiance quality {requested_quality}: {output}')
    return backend


def radiance_ready(output):
    """Wait for a completed prepass and an already-uploaded static scene.

    Runtime preparation and shader compilation can outlast a fixed sleep.
    Dynamic builds are intentionally unrestricted because animated entities
    may legitimately change them on every frame.
    """
    state = STATE.search(output)
    prepass = PREPASS.search(output)
    scene = STATIC_SCENE.search(output)
    return bool(state and state[1] == '1' and state[2] == '1' and prepass and
                scene and int(scene[1]) == 0 and int(scene[2]) == 0)


def parse_gpu_stages(output):
    result = {}
    for match in GPU_STAGE.finditer(output):
        name, count, mean, median, p95, stored, total = match.groups()
        result[name] = {'samples': int(count), 'mean_ms': float(mean), 'median_ms': float(median),
                        'p95_ms': float(p95) if p95 else None,
                        'median_samples': int(stored), 'total_samples': int(total),
                        'median_truncated': int(stored) < int(total), 'p95_truncated': int(stored) < int(total)}
    if not {'world', 'entities'} <= set(result) or any(not result[name]['samples'] for name in ('world', 'entities')):
        raise RuntimeError(f'GPU stage timestamps unavailable: {output}')
    return result


def summarize_pairs(rows, baseline='classic', treatment='radiance'):
    if not rows:
        raise ValueError('at least one measurement pair is required')
    gpu_deltas = [x[treatment]['whole_frame_gpu']['mean_ms'] - x[baseline]['whole_frame_gpu']['mean_ms'] for x in rows]
    medians = [x[treatment]['whole_frame_gpu']['median_ms'] - x[baseline]['whole_frame_gpu']['median_ms'] for x in rows
               if x[treatment]['whole_frame_gpu']['median_ms'] is not None and x[baseline]['whole_frame_gpu']['median_ms'] is not None
               and not x[treatment]['whole_frame_gpu'].get('median_truncated') and not x[baseline]['whole_frame_gpu'].get('median_truncated')]
    wall_deltas = [x[treatment]['whole_frame_wall_ms'] - x[baseline]['whole_frame_wall_ms'] for x in rows]
    percentages = [100 * (x[treatment]['whole_frame_gpu']['median_ms'] / x[baseline]['whole_frame_gpu']['median_ms'] - 1) for x in rows
                   if x[treatment]['whole_frame_gpu']['median_ms'] is not None and x[baseline]['whole_frame_gpu']['median_ms']
                   and not x[treatment]['whole_frame_gpu'].get('median_truncated') and not x[baseline]['whole_frame_gpu'].get('median_truncated')]
    p95_deltas = [x[treatment]['whole_frame_gpu']['p95_ms'] - x[baseline]['whole_frame_gpu']['p95_ms'] for x in rows
                  if x[treatment]['whole_frame_gpu'].get('p95_ms') is not None and x[baseline]['whole_frame_gpu'].get('p95_ms') is not None
                  and not x[treatment]['whole_frame_gpu'].get('p95_truncated') and not x[baseline]['whole_frame_gpu'].get('p95_truncated')]
    result = {'median_paired_gpu_mean_delta_ms': statistics.median(gpu_deltas),
            'median_paired_gpu_frame_median_delta_ms': statistics.median(medians) if len(medians) == len(rows) else None,
            'median_paired_gpu_frame_median_change_percent': statistics.median(percentages) if len(percentages) == len(rows) else None,
            'median_paired_gpu_p95_delta_ms': statistics.median(p95_deltas) if len(p95_deltas) == len(rows) else None,
            'median_paired_whole_frame_wall_delta_ms': statistics.median(wall_deltas)}
    if len(medians) == len(rows) and len(percentages) == len(rows):
        result['paired_bootstrap_95'] = paired_bootstrap(medians, percentages)
    stages = set.intersection(*(set(x[baseline].get('graphics_stages_gpu') or {}) & set(x[treatment].get('graphics_stages_gpu') or {}) for x in rows))
    if stages:
        result['graphics_stages'] = {}
        for name in sorted(stages):
            values = [x[treatment]['graphics_stages_gpu'][name]['median_ms'] - x[baseline]['graphics_stages_gpu'][name]['median_ms'] for x in rows
                      if not x[treatment]['graphics_stages_gpu'][name].get('median_truncated') and not x[baseline]['graphics_stages_gpu'][name].get('median_truncated')]
            result['graphics_stages'][name] = {'median_paired_median_delta_ms': statistics.median(values) if len(values) == len(rows) else None}
    return result


def paired_bootstrap(deltas, percentages, repetitions=10000):
    """Resample whole paired runs, keeping each treatment with its reference."""
    generator = random.Random(0)
    milliseconds, changes = [], []
    for _ in range(repetitions):
        indices = generator.choices(range(len(deltas)), k=len(deltas))
        milliseconds.append(statistics.median(deltas[index] for index in indices))
        changes.append(statistics.median(percentages[index] for index in indices))
    milliseconds.sort()
    changes.sort()
    low, high = math.floor(.025 * repetitions), math.ceil(.975 * repetitions) - 1
    return {'method': 'paired run bootstrap of median effect', 'resamples': repetitions, 'seed': 0,
            'pair_count': len(deltas), 'delta_ms': [milliseconds[low], milliseconds[high]],
            'change_percent': [changes[low], changes[high]]}


def alternating_order(pair, baseline='classic', treatment='radiance'):
    return (baseline, treatment) if pair % 2 == 0 else (treatment, baseline)


def other_game_pids(proc_root, ignored):
    for path in proc_root.iterdir():
        if not path.name.isdigit() or int(path.name) in ignored:
            continue
        try:
            if (path / 'comm').read_text().strip().lower() == 'vkquake':
                yield int(path.name)
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue


def ensure_no_game(allow_existing, benchmark_pid=None):
    if allow_existing:
        return
    ignored = {os.getpid(), benchmark_pid}
    for pid in other_game_pids(Path('/proc'), ignored):
        raise RuntimeError(f'vkQuake process {pid} is running; stop the benchmark and leave the game running, '
                           'or explicitly pass --allow-existing')


def main():
    # These helpers depend on Pillow and SDL; parsing/summarizing tests do not.
    import bench_meshlets as bench
    from bench_ecs_ripple import FrameCounter
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--basedir', required=True, type=Path, help='directory containing id1 or its PAKs')
    parser.add_argument('--progs', type=Path, help='compiled ripple progs.dat; required for ECS scenes')
    parser.add_argument('--scenes', nargs='+', choices=SCENES, default=list(SCENES))
    parser.add_argument('--backend', choices=('auto', 'software', 'hardware'), default='auto')
    parser.add_argument('--quality', type=int, choices=(0, 1, 2), default=1,
                        help='Radiance interval budget and emissive shadow quality')
    parser.add_argument('--grid', type=int, choices=(0, 1), default=0,
                        help='light-grid mode: camera-centered coarse (0) or map-sized fine (1)')
    parser.add_argument('--emissive-gain', type=float, default=1.0,
                        help='emissive-light gain for controlled profiling (default: 1)')
    parser.add_argument('--point-gain', type=float, default=1.0,
                        help='point-light gain for controlled profiling (default: 1)')
    parser.add_argument('--bounce-gain', type=float, default=1.0,
                        help='indirect-light gain for controlled profiling (default: 1)')
    parser.add_argument('--probe-gaussian', type=probe_gaussian_strength, default=0.0,
                        help='receiver probe blending strength, from 0 to 2')
    parser.add_argument('--compare-probe-gaussian', type=probe_gaussian_strength,
                        help='compare this strength against --probe-gaussian in alternating Radiance-only pairs')
    parser.add_argument('--receiver', choices=('full', 'half', 'checkerboard'), default='full',
                        help='receiver mode to test; reduced-resolution modes must actually be active')
    parser.add_argument('--resolution-scale', type=resolution_scale,
                        help='receiver scale: 1, approximately 0.666666667, 0.5, approximately 0.333333333, or 0.25; overrides the legacy receiver scale')
    parser.add_argument('--receiver-scales', type=resolution_scale, nargs='+',
                        help='compare these reduced scales in one process, sharing full-resolution reference measurements')
    parser.add_argument('--receiver-checkerboards', type=int, choices=(0, 1), nargs='+',
                        help='compare these checkerboard settings in the shared-reference receiver cohort')
    parser.add_argument('--receiver-filter', choices=('probe', 'texture'),
                        help='reduced receiver interpolation (default: probe); explicitly verify the active filter')
    parser.add_argument('--compare-receiver-filters', action='store_true',
                        help='compare full-direct, reduced probe, and reduced texture receivers with Radiance enabled')
    parser.add_argument('--compare-deferred-models', action='store_true',
                        help='compare forward and shared opaque model receivers with matched Radiance settings')
    parser.add_argument('--minimum-deferred-triangles', type=int, default=1,
                        help='minimum actual cached model triangles required by the deferred comparison')
    parser.add_argument('--production-full-reference', action='store_true',
                        help='also measure full-resolution indirect world draws in the receiver comparison')
    parser.add_argument('--no-legacy-reference', action='store_true',
                        help='omit legacy probe receiver measurements, retaining full references and texture treatments')
    parser.add_argument('--world-direct', action='store_true',
                        help='use CPU-recorded world draws in full mode for comparison with reduced modes')
    parser.add_argument('--sky-fallback', type=float, default=0.15,
                        help='sky-texture fallback gain for controlled profiling (default: 0.15)')
    parser.add_argument('--pairs', type=int, default=5)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--warmup', type=float, default=5)
    parser.add_argument('--initial-warmup', type=float,
                        help='extra warmup after each comparison mode first becomes ready (default: 10 for receiver comparisons, otherwise 0)')
    parser.add_argument('--hold-simulation', action='store_true',
                        help='set host_timescale 0.000001 before map startup to hold animation without pausing rendering')
    parser.add_argument('--prepass-samples', type=int, default=5)
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--pose', type=float, nargs=6, metavar=('X', 'Y', 'Z', 'PITCH', 'YAW', 'ROLL'),
                        help='set a fixed camera position and view angles in map units')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--capture', action='store_true', help='capture both modes after their measured runs')
    parser.add_argument('--gpu-stages', action='store_true', help='also time world/entity command groups (adds timestamp queries)')
    parser.add_argument('--validation', action='store_true', help='check Vulkan validation; affects timings')
    parser.add_argument('--allow-existing', action='store_true', help='allow a separate vkQuake process to run')
    parser.add_argument('--output', type=Path, help='new empty output directory')
    args = parser.parse_args()
    guarded_comparison = args.compare_receiver_filters or args.compare_deferred_models
    if args.compare_deferred_models and (args.compare_receiver_filters or args.compare_probe_gaussian is not None):
        parser.error('deferred model comparisons must run separately from receiver-filter or Gaussian comparisons')
    if args.minimum_deferred_triangles < 1:
        parser.error('minimum deferred triangles must be positive')
    if args.compare_receiver_filters and args.compare_probe_gaussian is not None:
        parser.error('receiver-filter and Gaussian comparisons must run separately')
    if args.production_full_reference and not args.compare_receiver_filters:
        parser.error('--production-full-reference requires --compare-receiver-filters')
    if args.no_legacy_reference and not args.compare_receiver_filters:
        parser.error('--no-legacy-reference requires --compare-receiver-filters')
    if (args.receiver_scales or args.receiver_checkerboards) and not args.compare_receiver_filters:
        parser.error('receiver cohorts require --compare-receiver-filters')
    if args.receiver_scales and args.resolution_scale is not None:
        parser.error('--receiver-scales and --resolution-scale must run separately')
    args.initial_warmup = args.initial_warmup if args.initial_warmup is not None else (10 if guarded_comparison else 0)
    if not math.isfinite(args.initial_warmup) or args.initial_warmup < 0 or (guarded_comparison and args.initial_warmup < 10):
        parser.error('receiver comparisons need at least 10 seconds initial warmup; other initial warmups must be finite and nonnegative')
    scale = args.receiver_scales[0] if args.receiver_scales else args.resolution_scale if args.resolution_scale is not None else (0.5 if args.receiver != 'full' or args.compare_receiver_filters else 1.0)
    scales = args.receiver_scales or [scale]
    if args.compare_receiver_filters and 1 in scales:
        parser.error('receiver-filter comparison requires a reduced --resolution-scale')
    if scale == 1 and args.receiver == 'checkerboard' and not args.compare_deferred_models:
        parser.error('checkerboarding requires a reduced receiver scale')
    checkerboard = bool(args.receiver_checkerboards[0]) if args.receiver_checkerboards else args.receiver == 'checkerboard'
    checkerboards = args.receiver_checkerboards or [int(checkerboard)]
    variants = [(size, bool(checker)) for size in scales for checker in checkerboards]
    if len(set(variants)) != len(variants):
        parser.error('receiver cohort contains duplicate settings')
    active_filter = args.receiver_filter or ('texture' if args.compare_deferred_models else 'probe')
    if args.compare_deferred_models and active_filter != 'texture':
        parser.error('deferred model comparison requires texture filtering')
    strict_receiver = guarded_comparison or args.resolution_scale is not None or args.receiver_filter is not None
    receiver = receiver_name(scale, checkerboard)
    compare_filter = args.compare_probe_gaussian is not None
    baseline, treatment = (('forward', 'deferred') if args.compare_deferred_models else
                           ('filter_base', 'filter_compare') if compare_filter else ('classic', 'radiance'))
    common = {'scale': scale, 'checkerboard': checkerboard, 'filter': active_filter,
              'indirect': receiver == 'full' and not args.world_direct and not args.compare_deferred_models, 'gaussian': args.probe_gaussian}
    if args.compare_receiver_filters:
        modes = {'full_direct': {**common, 'scale': 1.0, 'checkerboard': False, 'filter': 'probe', 'indirect': False, 'radiance': True}}
        if args.production_full_reference:
            modes['full_indirect'] = {**modes['full_direct'], 'indirect': True}
        for size, checker in variants:
            suffix = 'reduced' if len(variants) == 1 else f'd{receiver_divisor(size):g}_c{int(checker)}'
            for filtering in (('texture',) if args.no_legacy_reference else ('probe', 'texture')):
                modes[f'{filtering}_{suffix}'] = {**common, 'scale': size, 'checkerboard': checker,
                    'filter': filtering, 'indirect': False, 'radiance': True}
    elif args.compare_deferred_models:
        modes = {baseline: {**common, 'radiance': True, 'deferred': False},
                 treatment: {**common, 'radiance': True, 'deferred': True}}
    elif compare_filter:
        modes = {baseline: {**common, 'radiance': True}, treatment: {**common, 'radiance': True, 'gaussian': args.compare_probe_gaussian}}
    else:
        modes = {baseline: {**common, 'radiance': False}, treatment: {**common, 'radiance': True}}
    if args.pairs < 1 or not math.isfinite(args.seconds) or args.seconds <= 0 or not math.isfinite(args.warmup) or args.warmup < 0 or args.prepass_samples < 0 or args.width < 320 or args.height < 200:
        parser.error('invalid measurement duration, resolution, or sample count')
    if args.pose is not None and not all(math.isfinite(value) for value in args.pose):
        parser.error('camera pose values must be finite')
    if not math.isfinite(args.emissive_gain) or args.emissive_gain < 0:
        parser.error('emissive gain must be finite and nonnegative')
    if not math.isfinite(args.point_gain) or args.point_gain < 0:
        parser.error('point gain must be finite and nonnegative')
    if not math.isfinite(args.bounce_gain) or args.bounce_gain < 0:
        parser.error('bounce gain must be finite and nonnegative')
    if not math.isfinite(args.sky_fallback) or not 0 <= args.sky_fallback <= 1:
        parser.error('sky fallback must be finite and in 0..1')
    binary = args.binary.resolve()
    if not binary.is_file(): parser.error(f'binary not found: {binary}')
    progs = args.progs.resolve() if args.progs else None
    if any(s.startswith('ecs') for s in args.scenes) and (not progs or not progs.is_file()):
        parser.error('--progs must name the compiled ripple gamecode for ECS scenes')
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs in {source}')
    ensure_no_game(args.allow_existing)
    if args.output:
        root = args.output.resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        root = Path(tempfile.mkdtemp(prefix='quiver-radiance-bench-')).resolve()
    print(f'Results: {root}', flush=True)
    report = {'binary': {'path': str(binary), 'sha256': sha256(binary)},
              'paks': [{'path': str(p), 'sha256': sha256(p)} for p in paks],
              'progs': {'path': str(progs), 'sha256': sha256(progs)} if progs else None,
              'resolution': [args.width, args.height], 'pose': args.pose, 'backend_requested': args.backend, 'radiance_quality': args.quality,
              'radiance_grid': args.grid, 'radiance_emissive_gain': args.emissive_gain, 'radiance_point_gain': args.point_gain,
              'radiance_bounce_gain': args.bounce_gain, 'radiance_probe_gaussian': args.probe_gaussian,
              'compare_probe_gaussian': args.compare_probe_gaussian,
              'receiver_requested': receiver, 'resolution_scale_requested': scale,
              'receiver_filter_requested': active_filter, 'compare_receiver_filters': args.compare_receiver_filters,
              'compare_deferred_models': args.compare_deferred_models,
              'minimum_deferred_triangles': args.minimum_deferred_triangles if args.compare_deferred_models else None,
              'receiver_variants': [{'scale': size, 'checkerboard': checker} for size, checker in variants],
              'production_full_reference': args.production_full_reference, 'mode_settings': modes,
              'legacy_reference': not args.no_legacy_reference,
              'world_indirect': common['indirect'] if not args.compare_receiver_filters else False,
              'radiance_sky_fallback': args.sky_fallback,
              'gpu_stages': args.gpu_stages,
              'receiver_diagnostics': False if guarded_comparison else None,
              'receiver_submission_guard': guarded_comparison,
              'model_submission_guard': args.compare_deferred_models,
              'pairs': args.pairs, 'seconds': args.seconds, 'warmup_seconds': args.warmup,
              'initial_warmup_seconds': args.initial_warmup, 'hold_simulation': args.hold_simulation,
              'acceptance_timing': args.pairs >= 5 and args.seconds > 0 and args.warmup > 0 and args.initial_warmup >= 10,
              'validation': args.validation, 'scenes': {}, 'complete': False,
              'note': ('GPU frame means and medians are measured whole frames; prepass samples are separate '
                       'and omit receiver shading. Median paired run medians are not the median of per-frame deltas. '
                       'Gaussian comparisons report filter_compare minus filter_base. Receiver comparisons keep Radiance on, '
                       'match CPU world draws for causal comparisons, and optionally add the production indirect full-resolution reference. '
                       'Stage ranges can overlap on the GPU and are not additive; stage profiling changes the workload. '
                       'Short runs are supported for diagnostics; acceptance_timing identifies the requested minimum timing policy.')}
    result_path = root / 'results.json'
    backend_code = {'auto': 0, 'software': 1, 'hardware': 2}[args.backend]

    def run_scene(name):
        work = root / name
        (work / 'id1').mkdir(parents=True)
        (work / 'user').mkdir()
        for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
        game = None
        if name.startswith('ecs'):
            game = 'quiver_ripple'
            (work / game).mkdir()
            (work / game / 'progs.dat').symlink_to(progs)
        scale_command = (f'r_radiance_resolution_scale {scale:.12g}' if args.resolution_scale is not None or guarded_comparison
                         else f'r_radiance_half_resolution {int(receiver != "full")}')
        startup = (f'r_rtshadows 0\nmax_edicts 32800\nhost_maxfps 0\nhost_speeds 0\nsv_speeds 0\n'
                   f'scr_showfps 0\nr_radiance 0\nr_radiance_backend {backend_code}\nr_radiance_quality {args.quality}\n'
                   f'r_radiance_grid {args.grid}\nr_radiance_emissive_gain {args.emissive_gain}\n'
                   f'r_radiance_point_gain {args.point_gain}\nr_radiance_bounce_gain {args.bounce_gain}\n'
                   f'r_radiance_probe_gaussian {args.probe_gaussian}\nr_radiance_sky_fallback {args.sky_fallback}\n'
                   f'{scale_command}\nr_radiance_receiver_filter {int(active_filter == "texture")}\n'
                   f'r_radiance_checkerboard {int(checkerboard)}\n'
                   + ('r_radiance_receiver_diagnostics 0\n' if guarded_comparison else '')
                   + ('host_timescale 0.000001\nscr_centertime 0\n' if args.hold_simulation else '') + f'map {name}')
        # The reduced receiver prepass currently consumes CPU-recorded world
        # draws. Keep the classic comparison on that same draw path as well.
        boot = bench.BOOT.replace('r_indirect 1', f'r_indirect {int(common["indirect"] and not args.compare_receiver_filters)}')
        (work / 'id1/boot.cfg').write_text(boot.replace('map start', startup))
        config = SimpleNamespace(binary=binary, width=args.width, height=args.height,
                                 timeout=args.timeout, validation=args.validation, game=game, stock_only=True)
        engine = bench.Engine(config, work)
        counter = None
        try:
            engine.wait(lambda text: 'CL_SignonReply: 4' in text)
            if args.pose is not None:
                engine.cmd('setpos ' + ' '.join(str(value) for value in args.pose))
            elif game:
                engine.cmd('setpos -480 0 32 0 0 0')
            engine.cmd('host_maxfps 0\nvid_vsync 0\nhost_speeds 0\nsv_speeds 0\nscr_showfps 0')
            counter = FrameCounter(engine.process.pid, binary)
            simulation = SimulationClock(counter, binary) if args.hold_simulation else None
            rows = []
            actual_radiance_backend = None
            actual_sources = None
            warmed_modes = set()
            for pair in range(args.pairs):
                row = {}
                for mode in comparison_order(pair, tuple(modes)):
                    ensure_no_game(args.allow_existing, engine.process.pid)
                    settings = modes[mode]
                    is_radiance = settings['radiance']
                    mode_receiver = receiver_name(settings['scale'], settings['checkerboard'])
                    if compare_filter:
                        engine.cmd(f'r_radiance_probe_gaussian {settings["gaussian"]}')
                    if guarded_comparison:
                        engine.cmd(f'r_indirect {int(settings["indirect"])}\n'
                                   f'r_radiance_resolution_scale {settings["scale"]:.12g}\n'
                                   f'r_radiance_checkerboard {int(settings["checkerboard"])}\n'
                                   f'r_radiance_receiver_filter {int(settings["filter"] == "texture")}')
                    if args.compare_deferred_models:
                        engine.cmd(f'r_radiance_deferred {int(settings["deferred"])}')
                    receiver_options = {'full_texture': bool(args.compare_deferred_models and settings['deferred'] and settings['scale'] == 1)}
                    if args.compare_deferred_models:
                        receiver_options['checkerboard_requested'] = settings['checkerboard']
                    engine.cmd(f'r_radiance {int(is_radiance)}')
                    if is_radiance:
                        deadline = time.monotonic() + args.timeout
                        while True:
                            ready_output = engine.cmd('radiance_stats')
                            ready = radiance_ready(ready_output)
                            if ready and strict_receiver:
                                try:
                                    verify_receiver_state(ready_output, mode_receiver, settings['scale'], settings['filter'], True, **receiver_options)
                                    if args.compare_deferred_models:
                                        verify_model_receiver_state(ready_output, settings['deferred'], settings['deferred'], args.minimum_deferred_triangles)
                                except RuntimeError:
                                    ready = False
                            if ready:
                                break
                            if time.monotonic() >= deadline:
                                raise RuntimeError(f'radiance did not reach a completed prepass with a stable static scene: {ready_output}')
                            time.sleep(.2)
                    # Divisors and checkerboarding share the same reduced
                    # pipelines; warm each filter family once in a cohort.
                    warm_key = ((settings['filter'], settings['indirect']) if settings['scale'] == 1 else (settings['filter'], 'reduced')) if len(variants) > 1 else mode
                    if warm_key not in warmed_modes:
                        time.sleep(args.initial_warmup)
                        warmed_modes.add(warm_key)
                    time.sleep(args.warmup)
                    ensure_no_game(args.allow_existing, engine.process.pid)
                    if compare_filter or (is_radiance and guarded_comparison):
                        verify_probe_filter(engine.cmd('r_radiance_probe_gaussian'), settings['gaussian'])
                    if args.hold_simulation:
                        verify_hold(engine.cmd('host_timescale'))
                    if guarded_comparison:
                        verify_receiver_diagnostics_off(engine.cmd('r_radiance_receiver_diagnostics'))
                    camera_output = engine.cmd('viewpos') if args.pose is not None and strict_receiver else None
                    if camera_output is not None: verify_pose(camera_output, args.pose)
                    state_output = engine.cmd('radiance_stats')
                    frame_counts_before = receiver_frames(state_output) if guarded_comparison else None
                    model_before = (verify_model_receiver_state(state_output, settings['deferred'], settings['deferred'], args.minimum_deferred_triangles)
                                    if args.compare_deferred_models else None)
                    state_mode = 'radiance' if is_radiance else 'classic'
                    backend = verify_state(state_output, state_mode, args.backend, args.quality)
                    if is_radiance:
                        verify_receiver_state(state_output, mode_receiver, settings['scale'], settings['filter'], strict_receiver, **receiver_options)
                        if guarded_comparison:
                            sources = verify_sources(state_output, args.point_gain, args.emissive_gain, args.bounce_gain)
                            if actual_sources is not None and sources != actual_sources:
                                raise RuntimeError(f'Radiance source counts changed during receiver comparison: {actual_sources} -> {sources}')
                            actual_sources = sources
                        if actual_radiance_backend is not None and backend != actual_radiance_backend:
                            raise RuntimeError(f'radiance backend changed during comparison: {actual_radiance_backend} -> {backend}')
                        actual_radiance_backend = backend
                    engine.cmd('gpu_frame_time start' + ('\ngpu_stage_time start' if args.gpu_stages else ''))
                    wall_start_unix = time.time()
                    simulation_start = simulation.sample() if simulation else None
                    t0, f0 = counter.sample()
                    time.sleep(args.seconds)
                    ensure_no_game(args.allow_existing, engine.process.pid)
                    t1, f1 = counter.sample()
                    simulation_end = simulation.sample() if simulation else None
                    if simulation and (simulation_end < simulation_start or simulation_end - simulation_start > .001):
                        raise RuntimeError(f'held simulation advanced during sample: {simulation_start} -> {simulation_end}')
                    wall_end_unix = time.time()
                    gpu_output = engine.cmd('gpu_frame_time stop' + ('\ngpu_stage_time stop' if args.gpu_stages else ''))
                    gpu = parse_gpu(gpu_output)
                    gpu_stages = parse_gpu_stages(gpu_output) if args.gpu_stages else None
                    frame_counts = None
                    model_frames = None
                    if is_radiance and strict_receiver:
                        final_state = engine.cmd('radiance_stats')
                        verify_state(final_state, state_mode, args.backend, args.quality)
                        verify_receiver_state(final_state, mode_receiver, settings['scale'], settings['filter'], True, **receiver_options)
                        if guarded_comparison and verify_sources(final_state, args.point_gain, args.emissive_gain, args.bounce_gain) != actual_sources:
                            raise RuntimeError('Radiance source counts changed during the timed sample')
                        if guarded_comparison:
                            verify_receiver_diagnostics_off(engine.cmd('r_radiance_receiver_diagnostics'))
                            frame_counts = verify_receiver_frames(frame_counts_before, receiver_frames(final_state), settings['scale'] != 1)
                        if args.compare_deferred_models:
                            model_after = verify_model_receiver_state(final_state, settings['deferred'], settings['deferred'], args.minimum_deferred_triangles)
                            model_frames = verify_model_receiver_frames(model_before, model_after, settings['deferred'])
                    frames = (f1 - f0) & 0xffffffff
                    if not frames or t1 <= t0: raise RuntimeError('engine stopped producing frames')
                    prepass = []
                    if is_radiance:
                        for _ in range(args.prepass_samples):
                            output = engine.cmd('radiance_stats')
                            verify_state(output, state_mode, args.backend, args.quality)
                            match = PREPASS.search(output)
                            if match: prepass.append(float(match[1]))
                            time.sleep(.05)
                        if args.prepass_samples and not prepass:
                            raise RuntimeError('radiance prepass timing unavailable after warmup')
                    row[mode] = {'backend': backend, 'whole_frame_gpu': gpu,
                                 'receiver_settings': settings,
                                 'receiver_frames': frame_counts,
                                 'model_frames': model_frames,
                                 'viewpos': camera_output.strip() if camera_output else None,
                                 'simulation_start': simulation_start, 'simulation_end': simulation_end,
                                 'graphics_stages_gpu': gpu_stages,
                                 'measurement_start_unix': wall_start_unix, 'measurement_end_unix': wall_end_unix,
                                 'frames': frames, 'elapsed_s': t1 - t0,
                                 'whole_frame_wall_ms': 1000 * (t1 - t0) / frames,
                                 'fps': frames / (t1 - t0),
                                 'prepass_samples_ms': prepass,
                                 'prepass_sample_median_ms': statistics.median(prepass) if prepass else None,
                                 'radiance_stats': state_output.strip()}
                    if args.capture and pair == args.pairs - 1:
                        row[mode]['screenshot'] = str(engine.photo(f'{name}-{mode}'))
                    print(f'{name} {pair + 1}/{args.pairs} {mode}: '
                          f'{gpu["mean_ms"]:.3f} ms GPU mean, {gpu["median_ms"]} ms median, '
                          f'{gpu["p95_ms"]} ms p95, {row[mode]["fps"]:.1f} FPS', flush=True)
                rows.append(row)
                report['scenes'][name] = {'pairs': rows}
                result_path.write_text(json.dumps(report, indent=2) + '\n')
            summary = receiver_comparisons(rows) if args.compare_receiver_filters else summarize_pairs(rows, baseline, treatment)
            if args.validation:
                output = engine.text() + (work / 'output.log').read_text(errors='replace')
                if 'Using VK_LAYER_KHRONOS_validation' not in output or re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', output, re.I):
                    raise RuntimeError(f'validation missing or reported errors; inspect {work}')
            return {'pairs': rows, 'summary': summary}
        finally:
            if counter: counter.close()
            engine.close()

    try:
        for name in args.scenes:
            report['scenes'][name] = run_scene(name)
            result_path.write_text(json.dumps(report, indent=2) + '\n')
        report['complete'] = True
    finally:
        result_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Completed: {result_path}', flush=True)


if __name__ == '__main__':
    main()
