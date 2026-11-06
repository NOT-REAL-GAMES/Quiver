#!/usr/bin/env python3
"""Exercise radiance activation in a disposable Quake user directory.

Requires local registered PAKs, a Vulkan display, and a built engine. Use
--validation to require the Vulkan validation layer and reject its messages.
"""
import argparse
from pathlib import Path
import re
import shutil
import tempfile
import time

import bench_meshlets as bench


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--backend', choices=('auto', 'software', 'hardware'), default='auto')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--warmup-seconds', type=float, default=3)
    parser.add_argument('--lifecycle', action='store_true', help='also switch maps and restart video resources')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height = 1280, 720
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-smoke-'))
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', 'host_maxfps 60\nmap start'))
    engine = bench.Engine(args, work)
    def photo(label):
        start = len(engine.text())
        engine.cmd('screenshot png')
        deadline = time.monotonic() + args.timeout
        next_retry = time.monotonic() + 3
        while time.monotonic() < deadline:
            match = re.search(r'Wrote [\x01\x02]?([^\n]+\.png)', engine.text()[start:])
            if match:
                image = next(work.rglob(match[1]), None)
                if image:
                    target = work / (label + '.png')
                    shutil.copy2(image, target)
                    return target
            if engine.process.poll() is not None:
                raise RuntimeError(f'engine exited during screenshot; inspect {work}')
            # A screenshot requested on a frame without an acquired swapchain
            # image is dropped; retry instead of mistaking that for a hang.
            if time.monotonic() >= next_retry:
                engine.cmd('screenshot png')
                next_retry = time.monotonic() + 3
            time.sleep(.1)
        raise RuntimeError(f'screenshot timed out; inspect {work}')
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        backend = {'auto': 0, 'software': 1, 'hardware': 2}[args.backend]
        engine.cmd(f'r_radiance_backend {backend}\nr_radiance 1')
        def wait_active():
            deadline = time.monotonic() + args.timeout
            last = ''
            while time.monotonic() < deadline:
                last = engine.cmd('radiance_stats')
                if re.search(r'Radiance: requested=1 active=1\b', last):
                    return last
                time.sleep(.1)
            raise RuntimeError(f'radiance did not activate: {last}')

        last = wait_active()
        if args.backend != 'auto' and f'backend={"ray-query" if args.backend == "hardware" else "software"}' not in last:
            raise RuntimeError(f'unexpected backend: {last}')
        print(last.strip(), flush=True)
        if args.capture:
            print(f'Early screenshot: {photo("radiance-early")}', flush=True)
            time.sleep(max(0, args.warmup_seconds))
            print(f'Warm screenshot: {photo("radiance-warm")}', flush=True)
        if args.lifecycle:
            start = len(engine.text())
            engine.cmd('map e1m1')
            engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
            wait_active()
            engine.cmd('vid_restart')
            wait_active()
            print('PASS: map switch and video restart', flush=True)
        result = engine.cmd('r_radiance 0\nwait\nwait\nradiance_stats')
        if not re.search(r'Radiance: requested=0 active=0\b', result):
            raise RuntimeError(f'classic fallback did not activate: {result}')
        if args.capture:
            print(f'Classic screenshot: {photo("classic")}', flush=True)
        if args.validation:
            output = engine.text() + (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in output or re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', output, re.I):
                raise RuntimeError(f'validation missing or reported errors; inspect {work}')
        print('PASS: radiance activation, backend and classic fallback', flush=True)
    finally:
        engine.close()


if __name__ == '__main__':
    main()
