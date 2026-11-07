#!/usr/bin/env python3
"""Exercise optional GPU stage timers across render modes and video restart.

Run on a Vulkan display; game files are linked read-only into a temporary
basedir. With --validation, enable Vulkan synchronization validation as well.
"""
import argparse
import math
import os
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench
import bench_radiance as radiance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--backend', choices=('software', 'hardware'), default='software')
    parser.add_argument('--receiver-modes', action='store_true',
                        help='also require active half-resolution and checkerboard receiver paths')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 640, 360, None
    radiance.ensure_no_game(False)
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-gpu-stages-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT.replace('map start',
        f'host_maxfps 30\nr_rtshadows 0\nr_radiance_backend {1 if args.backend == "software" else 2}\n'
        'r_radiance 1\nmap e1m1')
    (work / 'id1/boot.cfg').write_text(boot + '\n')
    engine = bench.Engine(args, work)
    restart_marker = None

    def measure(label, require_samples=True):
        engine.cmd('gpu_stage_time start')
        time.sleep(1.2)
        output = engine.cmd('gpu_stage_time stop')
        if not require_samples and 'GPU stage time: no completed samples' in output:
            print(f'{label}: no stage samples (no submitted contexts)', flush=True)
            return
        values = radiance.parse_gpu_stages(output)
        for item in values.values():
            if not all(math.isfinite(item[key]) and item[key] >= 0 for key in ('mean_ms', 'median_ms')):
                raise RuntimeError(f'invalid GPU stage timing: {output}')
        print(f'{label}: {values}', flush=True)

    try:
        try:
            os.setpriority(os.PRIO_PROCESS, engine.process.pid, 15)
        except (AttributeError, OSError):
            pass
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        for variant in (0, 1, 2):
            engine.cmd(f'r_oit {variant}\nwait\nwait\nwait')
            measure(f'OIT {variant}')
        if args.receiver_modes:
            for receiver, checkerboard in (('half', 0), ('checkerboard', 1)):
                engine.cmd('r_oit 0\nr_indirect 0\nr_radiance_resolution_scale 0.5\n'
                           f'r_radiance_checkerboard {checkerboard}\nwait\nwait\nwait')
                time.sleep(2)
                radiance.verify_receiver_state(engine.cmd('radiance_stats'), receiver)
                measure(f'{receiver} receiver')
            engine.cmd('r_radiance_resolution_scale 1\nr_radiance_checkerboard 0\nwait\nwait')
        # Starting twice discards the old measurement. Stop must remain safe
        # across the swapchain recreation and after an ordinary fresh start.
        engine.cmd('gpu_stage_time start')
        time.sleep(.4)
        restart_marker = f'MESHLET_CMD_{engine.serial}_DONE'
        engine.cmd('gpu_stage_time start\nvid_width 800\nvid_restart\nwait\nwait')
        time.sleep(1.2)
        radiance.parse_gpu_stages(engine.cmd('gpu_stage_time stop'))
        measure('after video restart')
        # Existing whole-frame timing remains usable when stage timing is off.
        engine.cmd('gpu_frame_time start')
        time.sleep(1.2)
        radiance.parse_gpu(engine.cmd('gpu_frame_time stop'))
        engine.cmd('disconnect\nmenu_main\nwait\nwait')
        measure('menu without world', require_samples=False)
    finally:
        engine.close()
    stdout = (work / 'output.log').read_text(errors='replace')
    if not restart_marker or restart_marker not in stdout or 'Creating color buffer' not in stdout.split(restart_marker, 1)[1]:
        raise RuntimeError(f'video recreation was not observed; inspect {work}')
    output = engine.text() + stdout
    if args.validation and ('Using VK_LAYER_KHRONOS_validation' not in output or
        re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard|expects layout .*instead, image current layout', output)):
        raise RuntimeError(f'validation unavailable or reported an error; inspect {work}')
    print('PASS: GPU stage timers across OIT, restart, and menu', flush=True)


if __name__ == '__main__':
    main()
