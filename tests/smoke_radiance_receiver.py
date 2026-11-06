#!/usr/bin/env python3
"""Exercise reduced-resolution Radiance receivers in a disposable game.

Requires a Vulkan display and Quake PAK. The script never edits the user's
configuration. It fails if a requested receiver path silently falls back.
"""
import argparse
import json
import math
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench
from bench_radiance import (RECEIVER_SCALE, RECEIVER_STATE, probe_gaussian_strength, receiver_frames,
                            verify_probe_filter, verify_receiver_frames, verify_receiver_state, verify_state)
from diagnose_radiance_receiver_filter import coverage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--basedir', required=True, type=Path)
    parser.add_argument('--backend', choices=('software', 'hardware'), default='software')
    parser.add_argument('--receiver-filter', choices=('probe', 'texture'), default='probe')
    parser.add_argument('--probe-gaussian', type=probe_gaussian_strength, default=0.0)
    parser.add_argument('--exercise-gaussian', action='store_true',
                        help='also sweep texture Gaussian 0, .5, 1.5 and 2 over all scales/checker modes')
    parser.add_argument('--check-startup-default', action='store_true',
                        help='leave r_indirect unset at boot and require its engine default to be 0')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--output', type=Path, help='new empty directory for captures and logs')
    args = parser.parse_args()
    if args.exercise_gaussian and args.receiver_filter != 'texture':
        parser.error('--exercise-gaussian requires --receiver-filter texture')
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 641, 361, None
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not args.binary.is_file() or not paks:
        parser.error('an existing binary and Quake PAKs are required')
    if args.output:
        work = args.output.resolve()
        work.mkdir(parents=True, exist_ok=False)
    else:
        work = Path(tempfile.mkdtemp(prefix='quiver-radiance-receiver-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    backend = 1 if args.backend == 'software' else 2
    boot = bench.BOOT.replace('r_indirect 1', '' if args.check_startup_default else 'r_indirect 0')
    boot = boot.replace('map start', f'r_rtshadows 0\nr_radiance_backend {backend}\nr_radiance 1\n'
                        f'r_radiance_receiver_filter {int(args.receiver_filter == "texture")}\n'
                        f'r_radiance_probe_gaussian {args.probe_gaussian}\nmap start')
    (work / 'id1/boot.cfg').write_text(boot)
    engine = bench.Engine(args, work)
    current_gaussian = args.probe_gaussian
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        if args.check_startup_default:
            startup = engine.cmd('r_indirect')
            (work / 'startup-default.txt').write_text(startup)
            if not re.search(r'"r_indirect" is "0" \(default\)', startup):
                raise RuntimeError(f'r_indirect did not start at its engine default 0: {startup}')
            print('PASS: r_indirect starts at engine default 0', flush=True)
        engine.cmd('viewsize 130\nhost_maxfps 60')

        def wait_mode(mode, divisor=None):
            deadline = time.monotonic() + args.timeout
            last = ''
            while time.monotonic() < deadline:
                last = engine.cmd('r_radiance_probe_gaussian\nradiance_stats')
                try:
                    verify_state(last, 'radiance', args.backend)
                    verify_probe_filter(last, current_gaussian)
                    verify_receiver_state(last, mode)
                    filter_state = re.search(r'receiver filter requested=(probe|texture) active=(full|probe|texture)', last)
                    expected_filter = 'full' if mode == 'full' else args.receiver_filter
                    if not filter_state or filter_state.groups() != (args.receiver_filter, expected_filter):
                        raise RuntimeError(f'{expected_filter} receiver filter not active')
                    if divisor is not None:
                        scale = RECEIVER_SCALE.search(last)
                        if (not scale or not math.isclose(float(scale.group(3)), divisor) or
                                abs(float(scale.group(1)) - 1 / divisor) > 1e-6 or
                                abs(float(scale.group(2)) - 1 / divisor) > 1e-6):
                            raise RuntimeError(f'receiver divisor {divisor} not active')
                    if mode == 'full':
                        state = RECEIVER_STATE.search(last)
                        if not state or tuple(map(int, state.groups())) != (0, 0, 0, 0):
                            raise RuntimeError('full-resolution receiver not active')
                    return
                except RuntimeError:
                    time.sleep(.1)
            raise RuntimeError(f'{mode} receiver not active: {last}')

        def wait_fallback(reason, requested_scale=2/3):
            deadline = time.monotonic() + args.timeout
            last = ''
            while time.monotonic() < deadline:
                last = engine.cmd('r_radiance_probe_gaussian\nradiance_stats')
                try:
                    verify_state(last, 'radiance', args.backend)
                    verify_probe_filter(last, args.probe_gaussian)
                    state = RECEIVER_STATE.search(last)
                    filter_state = re.search(r'receiver filter requested=(probe|texture) active=(full|probe|texture)', last)
                    if not filter_state or filter_state.groups() != (args.receiver_filter, 'full'):
                        raise RuntimeError('receiver filter did not fall back')
                    scale = RECEIVER_SCALE.search(last)
                    if (state and tuple(map(int, state.groups())) == (1, 0, 1, 0) and scale and
                            abs(float(scale.group(1))-requested_scale) < 1e-6 and
                            float(scale.group(2)) == 1 and float(scale.group(3)) == 1):
                        print(f'PASS: {reason} uses the full-resolution fallback', flush=True)
                        return
                except RuntimeError:
                    pass
                time.sleep(.1)
            raise RuntimeError(f'{reason} did not fall back: {last}')

        def check_two_thirds_coverage(mode):
            if args.receiver_filter != 'texture':
                return
            engine.cmd('r_radiance_receiver_diagnostics 1')
            deadline, previous = time.monotonic()+args.timeout, None
            try:
                while time.monotonic() < deadline:
                    state = engine.cmd('radiance_stats')
                    try:
                        output_kind = 'filtered' if current_gaussian > 0 or mode == 'checkerboard' else 'raw'
                        value = coverage(state, 'texture', 1.5, 641, 361, output_kind)
                        if value['viewport'] != [641, 361] or value['blocks']['total'] != 428*241:
                            raise RuntimeError('two-thirds coverage did not use ceil(2N/3) extent')
                        if previous is not None and value['frame'] > previous:
                            (work/f'receiver-divisor-1.5-{mode}-coverage.json').write_text(json.dumps(value, indent=2))
                            return
                        previous = value['frame']
                    except RuntimeError:
                        pass
                    time.sleep(.05)
                raise RuntimeError('fresh two-thirds coverage unavailable')
            finally:
                engine.cmd('r_radiance_receiver_diagnostics 0')

        def measure_stages(label, postprocess):
            before = engine.cmd('radiance_stats')
            engine.cmd('gpu_stage_time start')
            time.sleep(.3)
            timing_output = engine.cmd('gpu_stage_time stop')
            after = engine.cmd('radiance_stats')
            verify_receiver_frames(receiver_frames(before), receiver_frames(after), True)
            (work / (label + '-gpu-stages.txt')).write_text(timing_output)
            timings = {}
            for stage in ('classify', 'shade', 'filter'):
                match = re.search(r'GPU receiver_' + stage + r' stage: (\d+) samples, ([\d.]+) ms mean', timing_output)
                if not match or int(match[1]) == 0:
                    raise RuntimeError(f'receiver {stage} timestamps unavailable: {timing_output}')
                mean = float(match[2])
                if mean < 0 or (stage != 'filter' or postprocess) and mean <= 0:
                    raise RuntimeError(f'receiver {stage} timestamps do not contain expected work: {timing_output}')
                timings[stage] = {'samples': int(match[1]), 'mean_ms': mean}
            # The recorder writes adjacent timestamps even with no filter.
            # Their interval may contain timestamp overhead rather than exact
            # zero; the CPU frame test proves that this mode dispatches nothing.
            timings['filter_empty_work_range'] = not postprocess
            return timings

        engine.cmd('r_radiance_half_resolution 0\nr_radiance_checkerboard 0')
        wait_mode('full', 1)
        engine.photo('receiver-full')
        for mode in ('half', 'checkerboard'):
            engine.cmd(f'r_radiance_half_resolution 1\nr_radiance_checkerboard {int(mode == "checkerboard")}')
            wait_mode(mode, 2)
            engine.photo('receiver-' + mode)
        for divisor, scale in ((1.5, '0.666'), (2, '0.5'), (3, '0.333'), (4, '0.25')):
            for mode in ('half', 'checkerboard'):
                engine.cmd(f'r_radiance_resolution_scale {scale}\nr_radiance_checkerboard {int(mode == "checkerboard")}')
                wait_mode(mode, divisor)
                if divisor == 1.5:
                    alias = engine.cmd('r_radiance_half_resolution')
                    if not re.search(r'"r_radiance_half_resolution" is "4"', alias):
                        raise RuntimeError(f'two-thirds scale did not mirror alias4: {alias}')
                    check_two_thirds_coverage(mode)
                engine.photo(f'receiver-divisor-{divisor}-{mode}')
        if args.exercise_gaussian:
            sweep = []
            for gaussian in (0, .5, 1.5, 2):
                engine.cmd(f'r_radiance_probe_gaussian {gaussian}')
                current_gaussian = gaussian
                for divisor, scale in ((1.5, '0.666666667'), (2, '0.5'), (3, '0.333333333'), (4, '0.25')):
                    for checker in (False, True):
                        mode = 'checkerboard' if checker else 'half'
                        engine.cmd(f'r_radiance_resolution_scale {scale}\nr_radiance_checkerboard {int(checker)}')
                        wait_mode(mode, divisor)
                        label = f'gaussian-{gaussian:g}-divisor-{divisor}-checker-{int(checker)}'
                        timing = measure_stages(label, gaussian > 0 or checker)
                        engine.photo(label)
                        sweep.append({'gaussian': gaussian, 'divisor': divisor, 'checkerboard': checker, 'timings': timing})
                        (work / 'gaussian-sweep.json').write_text(json.dumps(sweep, indent=2) + '\n')
                        print(f'PASS: {label}, filter={"work" if gaussian > 0 or checker else "empty range"}', flush=True)
            engine.cmd(f'r_radiance_probe_gaussian {args.probe_gaussian}')
            current_gaussian = args.probe_gaussian
        # Legacy setters must override every non-half reduced scale, including
        # the distinct two-thirds mirror value4.
        for scale in ('0.666', '0.25', '0.333'):
            engine.cmd(f'r_radiance_resolution_scale {scale}\nr_radiance_half_resolution 1')
            wait_mode('checkerboard', 2)
            engine.cmd('r_radiance_half_resolution 0\nr_radiance_checkerboard 0')
            wait_mode('full', 1)
            engine.cmd('r_radiance_checkerboard 1')
        engine.cmd('r_radiance_resolution_scale 0.666666667')
        wait_mode('checkerboard', 1.5)
        engine.cmd('gpu_stage_time start')
        time.sleep(.3)
        timing_output = engine.cmd('gpu_stage_time stop')
        (work / 'receiver-gpu-stages.txt').write_text(timing_output)
        for stage in ('classify', 'shade', 'filter'):
            timing = re.search(r'GPU receiver_' + stage + r' stage: (\d+) samples, ([\d.]+) ms mean', timing_output)
            if not timing or int(timing.group(1)) == 0 or float(timing.group(2)) <= 0:
                raise RuntimeError(f'receiver {stage} timestamps unavailable: {timing_output}')
        engine.cmd('vid_restart')
        wait_mode('checkerboard', 1.5)
        # Indirect world rendering and multisampling cannot use the isolated
        # single-sample receiver pass. Keep the modes requested while testing
        # that the renderer submits the ordinary world draw, then reactivates
        # the receiver when each unsupported condition is removed.
        # Resource allocation can be printed before qconsole.log is opened;
        # stdout captures those early startup messages as well.
        indirect_log = engine.text() + (work / 'output.log').read_text(errors='replace')
        if 'Allocating indirect draw data' in indirect_log:
            engine.cmd('scr_speeds 0\nr_gpulightmapupdate 1\nr_indirect 1')
            wait_fallback('indirect world')
            engine.cmd('r_indirect 0')
            wait_mode('checkerboard', 1.5)
        else:
            print('SKIP: indirect-world fallback (indirect draws unavailable)', flush=True)
        restart_start = len(engine.text())
        engine.cmd('vid_fsaa 4\nvid_restart')
        # vid_restart is deferred until after the console command marker. The
        # auxiliary-resource release establishes that the restart took effect.
        engine.wait(lambda text: 'RC experimental auxiliary resources released' in text, restart_start)
        deadline = time.monotonic() + args.timeout
        active_count = 0
        msaa_active = None
        while time.monotonic() < deadline:
            restart = engine.text()[restart_start:]
            samples = re.search(r'provider prerequisites: geometry=\d+ samples=(\d+)', restart)
            if samples and int(samples.group(1)) > 1:
                msaa_active = True
                break
            state_output = engine.cmd('radiance_stats')
            verify_state(state_output, 'radiance', args.backend)
            state = RECEIVER_STATE.search(state_output)
            if state and tuple(map(int, state.groups())) == (1, 1, 1, 1):
                active_count += 1
                if active_count >= 3:
                    msaa_active = False
                    break
            else:
                active_count = 0
            time.sleep(.1)
        if msaa_active is True:
            wait_fallback('MSAA')
        elif msaa_active is False:
            print('SKIP: MSAA fallback (multisampling unavailable)', flush=True)
        else:
            raise RuntimeError(f'could not determine multisample count after restart: {engine.text()[restart_start:]}')
        engine.cmd('vid_fsaa 1\nvid_restart')
        wait_mode('checkerboard', 1.5)
        start = len(engine.text())
        engine.cmd('map e1m1')
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        wait_mode('checkerboard', 1.5)
        engine.cmd('r_radiance_checkerboard 0\nr_radiance_half_resolution 0')
        wait_mode('full', 1)
        if args.validation:
            output = engine.text() + (work / 'output.log').read_text(errors='replace')
            # Release builds do not enable the layer through vkQuake's debug
            # flag, but the loader can inject it through VK_INSTANCE_LAYERS.
            # Its settings warning positively identifies that loaded layer.
            layer_loaded = ('Using VK_LAYER_KHRONOS_validation' in output or
                            'Validation Warning: [ VALIDATION-SETTINGS ]' in output)
            if (not layer_loaded or
                    re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output)):
                raise RuntimeError(f'validation unavailable or reported errors; inspect {work}')
    finally:
        engine.close()

    # Check after shutdown too: teardown validation messages can arrive after
    # the last console command has completed.
    if args.validation:
        output = (work / 'output.log').read_text(errors='replace')
        if re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output):
            raise RuntimeError(f'validation errors during shutdown; inspect {work}')
    print('PASS: receiver toggles, fallbacks, video restart, and map change', flush=True)


if __name__ == '__main__':
    main()
