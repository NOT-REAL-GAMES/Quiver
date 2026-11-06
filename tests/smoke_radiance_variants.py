#!/usr/bin/env python3
"""Exercise Radiance across OIT changes and video restarts on a Vulkan display.

The game directory is disposable; only PAK files are symlinked from the given
basedir. This is an optional in-game test, not a performance benchmark.
"""
import argparse
import os
from pathlib import Path
import re
import tempfile

import bench_meshlets as bench


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--backend', choices=('software', 'hardware'), default='software')
    parser.add_argument('--hdr', action='store_true', help='require the FP16 scene target')
    parser.add_argument('--msaa', type=int, choices=(0, 4), default=0)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 640, 360, None
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-variants-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = '\n'.join(line for line in bench.BOOT.splitlines()
                     if not line.startswith(('vid_fullscreen', 'vid_vsync', 'vid_fsaa')))
    backend_value = 1 if args.backend == 'software' else 2
    active_backend = 'software' if args.backend == 'software' else 'ray-query'
    boot = boot.replace('map start', f'vid_fsaa {args.msaa}\nr_oit 0\nr_rtshadows 0\nr_radiance_backend {backend_value}\nr_radiance 1\nr_radiance_hdr {int(args.hdr)}\nmax_edicts 32000\nmap e1m1')
    (work / 'id1/boot.cfg').write_text(boot + '\n')
    engine = bench.Engine(args, work)
    restart_marker = None
    try:
        try:
            os.setpriority(os.PRIO_PROCESS, engine.process.pid, 15)
        except (AttributeError, OSError):
            pass
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        for index, variant in enumerate((0, 1, 2, 0, 'restart')):
            if variant == 'restart':
                # vid_restart is a no-op unless a video cvar has changed.
                # Drain any pending OIT recreation first, then anchor the
                # renderer log before issuing the resize.
                engine.cmd('wait\nwait\nwait\nwait')
                restart_marker = f'MESHLET_CMD_{engine.serial}_DONE'
                engine.cmd('vid_width 800\nvid_restart\nwait\nwait')
                # Stdout may remain buffered until shutdown. Check the ordered
                # log after closing the engine, when it has been flushed.
            elif index:
                engine.cmd(f'r_oit {variant}')
            out = engine.cmd('radiance_stats')
            if f'Radiance: requested=1 active=1 backend={active_backend}' not in out:
                raise RuntimeError(f'Radiance unavailable at OIT {variant}: {out}')
            total = re.search(r'tracked_memory=(\d+)', out)
            parts = re.search(r'GPU allocations inputs=(\d+) scene=(\d+) cascades=(\d+) acceleration=(\d+) B', out)
            if not total or not parts:
                raise RuntimeError(f'Radiance memory breakdown unavailable at OIT {variant}: {out}')
            values = [int(value) for value in parts.groups()]
            if int(total.group(1)) != sum(values) or not all(values[:3]):
                raise RuntimeError(f'Radiance memory accounting invalid at OIT {variant}: {out}')
            print(f'{"Video restart" if variant == "restart" else f"OIT {variant}"}: Radiance {active_backend} active', flush=True)
    finally:
        engine.close()
    # Include shutdown, where lifetime or synchronization problems may surface.
    stdout = (work / 'output.log').read_text(errors='replace')
    if not restart_marker or restart_marker not in stdout or 'Creating color buffer' not in stdout.split(restart_marker, 1)[1]:
        raise RuntimeError(f'video resources were not recreated after restart; inspect {work}')
    output = engine.text() + stdout
    formats = re.findall(r'Scene target format: ([^\r\n]+)', output)
    if args.hdr and (not formats or formats[-1].strip() != 'RGBA16F'):
        raise RuntimeError(f'FP16 scene target unavailable: {formats}; inspect {work}')
    if args.msaa and f'{args.msaa} AA Samples' not in output:
        raise RuntimeError(f'{args.msaa}x MSAA was not active; inspect {work}')
    if args.validation and ('Using VK_LAYER_KHRONOS_validation' not in output or
        re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard|expects layout .*instead, image current layout', output)):
        raise RuntimeError(f'validation unavailable or reported an error; inspect {work}')
    print('PASS: Radiance across transparency-mode restarts', flush=True)


if __name__ == '__main__':
    main()
