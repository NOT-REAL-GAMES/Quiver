#!/usr/bin/env python3
"""Validate live receiver filter switches and optional coverage in an isolated game."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench
from bench_radiance import (RECEIVER_FILTER, RECEIVER_SCALE, RECEIVER_STATE, GAINS, receiver_divisor,
                            receiver_extent, resolution_scale, verify_probe_filter, verify_state)

ERRORS = re.compile(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard')
HEADER = re.compile(r'estimated receiver coverage frame=(\d+) viewport=(\d+)x(\d+) divisor=(\S+) output=(\w+)')
BLOCKS = re.compile(r'receiver blocks total=(\d+) empty=(\d+) reusable=(\d+) mixed=(\d+) raw_GI_valid=(\d+) selected_GI_valid=(\d+)')
PIXELS = re.compile(r'receiver pixels covered=(\d+) GI_reuse=(\d+) GI_fallback=(\d+) direct_reuse=(\d+) direct_fallback=(\d+) invalid_geometry=(\d+)')


def counters(text, bounce, scale=.25):
    h, b, p = HEADER.search(text), BLOCKS.search(text), PIXELS.search(text)
    if not h or not b or not p:
        raise RuntimeError('coverage counters unavailable')
    frame, width, height = map(int, h.groups()[:3])
    divisor = float(h[4])
    total, empty, reusable, mixed, raw, selected = map(int, b.groups())
    covered, gi, gi_fallback, direct, direct_fallback, invalid = map(int, p.groups())
    if ((width, height, h[5]) != (641, 361, 'filtered') or
            not math.isclose(divisor, receiver_divisor(scale))):
        raise RuntimeError(f'unexpected diagnostic viewport/output: {h.group()}')
    if total != receiver_extent(641, scale)*receiver_extent(361, scale) or empty + reusable + mixed != total:
        raise RuntimeError(f'inconsistent block counters: {b.group()}')
    if raw > reusable or selected > reusable or covered > 641 * 361 or invalid > covered:
        raise RuntimeError('coverage counts exceed their domains')
    if gi + gi_fallback != covered or direct + direct_fallback != covered:
        raise RuntimeError(f'inconsistent pixel counters: {p.group()}')
    if bounce == 0 and (raw or selected):
        raise RuntimeError('zero bounce still produced valid GI samples')
    if bounce > 0 and (not raw or not selected):
        raise RuntimeError('restored bounce has no valid GI samples')
    return dict(frame=frame, divisor=divisor, total=total, empty=empty, reusable=reusable, mixed=mixed,
                raw=raw, selected=selected, covered=covered, gi_reuse=gi, gi_fallback=gi_fallback,
                direct_reuse=direct, direct_fallback=direct_fallback, invalid=invalid)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--basedir', required=True, type=Path)
    parser.add_argument('--backends', nargs='+', choices=('hardware', 'software'), default=('hardware', 'software'))
    parser.add_argument('--resolution-scale', type=resolution_scale, default=.25)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.resolution_scale == 1:
        parser.error('live receiver-filter checks require a reduced scale')
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 641, 361, None
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not args.binary.is_file() or not paks:
        parser.error('existing binary and Quake PAK files required')
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='quiver-filter-toggle-'))
    if args.output:
        work.mkdir(parents=True, exist_ok=False)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    first_backend = 2 if args.backends[0] == 'hardware' else 1
    boot = bench.BOOT.replace('r_indirect 1', 'r_indirect 0').replace('map start',
        f'r_rtshadows 0\nr_radiance_backend {first_backend}\nr_radiance 1\n'
        f'r_radiance_probe_gaussian 2\nr_radiance_resolution_scale {args.resolution_scale:.12g}\n'
        'r_radiance_checkerboard 1\nr_radiance_receiver_filter 0\n'
        'r_radiance_receiver_diagnostics 0\nmap start')
    (work / 'id1/boot.cfg').write_text(boot)
    print(f'Isolated test directory: {work}', flush=True)
    manifest = {'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(), 'checks': []}
    engine = bench.Engine(args, work)
    backend = args.backends[0]

    def state(label, filtering, diagnostic=False, bounce=1, fallback=False, fresh=False):
        deadline = time.monotonic() + args.timeout
        last, seen_frame = '', None
        while time.monotonic() < deadline:
            last = engine.cmd('r_radiance_receiver_diagnostics\nr_radiance_probe_gaussian\nradiance_stats')
            try:
                verify_state(last, 'radiance', backend)
                verify_probe_filter(last, 2)
                diagnostic_cvar = re.search(r'"r_radiance_receiver_diagnostics" is "([^"]+)"', last)
                if not diagnostic_cvar or bool(float(diagnostic_cvar[1])) != diagnostic:
                    raise RuntimeError('diagnostic cvar did not match requested state')
                size, filt, mode, gains = RECEIVER_SCALE.search(last), RECEIVER_FILTER.search(last), RECEIVER_STATE.search(last), GAINS.search(last)
                expected_mode = (1, 0, 1, 0) if fallback else (1, 1, 1, 1)
                expected_active = 'full' if fallback else filtering
                if not filt or filt.groups() != (filtering, expected_active):
                    raise RuntimeError('requested/active receiver filter mismatch')
                if (not size or not math.isclose(float(size[1]), args.resolution_scale, rel_tol=1e-6) or
                        not math.isclose(float(size[2]), 1 if fallback else args.resolution_scale, rel_tol=1e-6) or
                        not math.isclose(float(size[3]), 1 if fallback else receiver_divisor(args.resolution_scale))):
                    raise RuntimeError('requested/active receiver scale mismatch')
                if not mode or tuple(map(int, mode.groups())) != expected_mode:
                    raise RuntimeError('checkerboard state mismatch')
                if not gains or float(gains[3]) != bounce:
                    raise RuntimeError('bounce gain mismatch')
                row = dict(backend=backend, label=label, filter=filtering, fallback=fallback, bounce=bounce,
                           requested_scale=args.resolution_scale, active_scale=float(size[2]), active_divisor=float(size[3]))
                if diagnostic and not fallback:
                    row.update(counters(last, bounce, args.resolution_scale))
                    if fresh and (seen_frame is None or row['frame'] == seen_frame):
                        seen_frame = row['frame']
                        time.sleep(.05)
                        continue
                elif HEADER.search(last) or BLOCKS.search(last) or PIXELS.search(last):
                    raise RuntimeError('disabled diagnostics printed stale coverage')
                (work / f'{backend}-{label}.txt').write_text(last)
                manifest['checks'].append(row)
                (work / 'summary.json').write_text(json.dumps(manifest, indent=2))
                print('PASS ' + json.dumps(row, sort_keys=True), flush=True)
                return row
            except RuntimeError:
                time.sleep(.05)
        raise RuntimeError(f'{backend} {label} failed: {last}')

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.cmd('viewsize 130\nhost_maxfps 60')
        for index, backend in enumerate(args.backends):
            engine.cmd(f'r_radiance_backend {2 if backend == "hardware" else 1}\nr_indirect 0\n'
                       'r_radiance_bounce_gain 1\nr_radiance_receiver_diagnostics 0\nr_radiance_receiver_filter 0')
            state('probe-before', 'probe')
            engine.cmd('r_radiance_receiver_filter 1')
            state('texture-first', 'texture')
            engine.cmd('r_radiance_receiver_filter 0')
            state('probe-after', 'probe')
            enabled = engine.cmd('r_radiance_receiver_filter 1\nr_radiance_receiver_diagnostics 1\nr_radiance_receiver_diagnostics')
            print(enabled.strip(), flush=True)
            state('diagnostics-on', 'texture', diagnostic=True, fresh=True)
            engine.photo(f'{backend}-texture-diagnostics')
            engine.cmd('r_radiance_receiver_diagnostics 0')
            state('diagnostics-off', 'texture')
            engine.cmd('gpu_stage_time start')
            time.sleep(.3)
            (work / f'{backend}-gpu-stages.txt').write_text(engine.cmd('gpu_stage_time stop'))
            engine.cmd('r_radiance_receiver_diagnostics 1\nr_radiance_bounce_gain 0')
            state('zero-bounce', 'texture', diagnostic=True, bounce=0, fresh=True)
            engine.cmd('r_radiance_bounce_gain 1')
            state('restored-bounce', 'texture', diagnostic=True, fresh=True)
            start = len(engine.text())
            engine.cmd(f'vid_fsaa {1 if index % 2 == 0 else 0}\nvid_restart')
            engine.wait(lambda text: 'RC experimental auxiliary resources released' in text, start)
            state('video-restart', 'texture', diagnostic=True, fresh=True)
            start = len(engine.text())
            engine.cmd('map e1m1' if index % 2 == 0 else 'map start')
            engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
            state('map-change', 'texture', diagnostic=True, fresh=True)
            engine.cmd('r_indirect 1')
            # Unsupported world submission clears completed coverage; the
            # requested diagnostic cvar remains enabled for restoration.
            state('indirect-fallback', 'texture', diagnostic=True, fallback=True)
            time.sleep(.2)
            state('indirect-fallback-stable', 'texture', diagnostic=True, fallback=True)
            engine.cmd('r_indirect 0')
            state('indirect-restored', 'texture', diagnostic=True, fresh=True)
            engine.cmd('r_radiance_receiver_diagnostics 0')
            state('disabled-final', 'texture')
        if args.validation:
            output = (work / 'output.log').read_text(errors='replace')
            if 'VALIDATION-SETTINGS' not in output or ERRORS.search(output):
                raise RuntimeError(f'validation missing or reported errors: {work}')
    finally:
        engine.close()
    output = (work / 'output.log').read_text(errors='replace')
    if args.validation and ERRORS.search(output):
        raise RuntimeError(f'validation errors during teardown: {work}')
    print('PASS: live filter switches, optional diagnostics, bounce, restart, map, and fallback', flush=True)


if __name__ == '__main__':
    main()
