#!/usr/bin/env python3
"""Exercise isolated cold/warm, corrupt-cache, and video-restart lifecycles.

Requires a Vulkan display, SDL development headers, and Quake PAKs. Uses a
private game directory and writes no user configuration. Timing is diagnostic,
not a performance claim: driver caches and system load affect it.
"""
import argparse
import os
from pathlib import Path
import re
import tempfile
import time
from types import SimpleNamespace

import bench_meshlets as bench


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, required=True)
    ap.add_argument('--basedir', type=Path, required=True)
    ap.add_argument('--backend', choices=('software', 'hardware'), default='software')
    ap.add_argument('--timeout', type=float, default=180)
    ap.add_argument('--validation', action='store_true')
    args = ap.parse_args()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    if (source / 'Id1').is_dir(): source /= 'Id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: ap.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-pipeline-cache-')).resolve()
    print(f'Isolated directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    backend = 1 if args.backend == 'software' else 2
    boot = bench.BOOT.replace('host_maxfps 0', 'host_maxfps 60').replace(
        'map start', f'r_rtshadows 0\nr_radiance 1\nr_radiance_quality 0\nr_radiance_backend {backend}\nmap e1m1')
    (work / 'id1/boot.cfg').write_text(boot)
    config = SimpleNamespace(binary=args.binary.resolve(), width=640, height=360,
                             timeout=args.timeout, validation=args.validation, game=None)
    cache = work / 'vulkan-pipeline-cache.bin'
    marker = 'Loaded Vulkan pipeline cache'
    expected = 'software' if args.backend == 'software' else 'ray-query'

    def run(name, expect_loaded, restart=False):
        log = work / 'xdg/vkQuake/qconsole.log'
        if log.exists(): log.unlink()
        t = time.monotonic()
        engine = bench.Engine(config, work)
        try:
            try: os.setpriority(os.PRIO_PROCESS, engine.process.pid, 15)
            except (AttributeError, OSError): pass
            engine.wait(lambda text: 'CL_SignonReply: 4' in text)
            status = engine.cmd('radiance_stats')
            if f'active=1 backend={expected}' not in status:
                raise RuntimeError(f'{name}: unexpected Radiance state: {status}')
            elapsed = time.monotonic() - t
            if restart:
                result = engine.cmd('vid_restart\nwait\nwait\nradiance_stats')
                if f'active=1 backend={expected}' not in result:
                    raise RuntimeError(f'{name}: video restart failed: {result}')
        finally:
            engine.close()
        output = (work / 'output.log').read_text(errors='replace')
        (work / f'{name}.log').write_text(output)
        if engine.process.returncode != 0 or not cache.is_file():
            raise RuntimeError(f'{name}: engine exit/cache save failed')
        if (marker in output) != expect_loaded:
            raise RuntimeError(f'{name}: unexpected cache load state')
        if args.validation and ('Using VK_LAYER_KHRONOS_validation' not in output or
            re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output)):
            raise RuntimeError(f'{name}: validation unavailable or reported errors; inspect {work}')
        print(f'{name}: {elapsed:.3f} s, cache {cache.stat().st_size} bytes', flush=True)

    run('cold', False)
    run('warm-restart', True, restart=True)
    payload = bytearray(cache.read_bytes())
    if not payload: raise RuntimeError('empty pipeline cache')
    payload[-1] ^= 0x80
    cache.write_bytes(payload)
    run('corrupt', False)
    run('recovered', True)
    print('PASS: cold/warm pipeline cache, video restart, and corrupt-cache recovery', flush=True)


if __name__ == '__main__':
    main()
