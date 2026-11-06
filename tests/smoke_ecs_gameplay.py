#!/usr/bin/env python3
"""Exercise map lifecycle, normal/fast saves and demo seeking on a live engine.

Uses disposable configuration and locally supplied Quake PAKs; requires SDL
and a Vulkan display. Optional validation checks Vulkan diagnostic output.
"""
import argparse
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--validation', action='store_true')
    a = p.parse_args()
    a.binary = a.binary.resolve(); a.width = 1280; a.height = 720
    source = a.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(x for x in source.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-ecs-gameplay-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1').mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', 'r_rtshadows 0\nmap e1m1'))
    e = bench.Engine(a, work)
    try:
        e.wait(lambda t: 'CL_SignonReply: 4' in t)
        def check_ecs(server):
            output = e.cmd('ecs_stats')
            memory = re.search(r'ECS allocation \(bytes, excluding allocator overhead\): server (\d+), client QC (\d+), client render (\d+)', output)
            slots = re.search(r'Client protocol slots bound: (\d+)', output)
            if not memory or not slots or not int(memory[3]) or not int(slots[1]) or (server and not int(memory[1])):
                raise RuntimeError('ECS world missing after signon:\n' + output)
        check_ecs(True)
        e.cmd('host_maxfps 72')
        player = e.cmd('edict 1')
        if not re.search(r'classname\s+player', player):
            raise RuntimeError('missing player edict:\n' + player)
        saved = e.cmd('save ecs_smoke')
        if 'Saving game' not in saved: raise RuntimeError('save failed:\n' + saved)
        deadline = time.monotonic() + a.timeout
        while time.monotonic() < deadline and not list(work.rglob('ecs_smoke.sav')):
            time.sleep(.05)
        for command in ('fastload', 'load'):
            start = len(e.text())
            out = e.cmd(f'{command} ecs_smoke')
            if "Can't fastload" in out or 'ERROR' in out: raise RuntimeError(out)
            if command == 'load': e.wait(lambda t: 'CL_SignonReply: 4' in t, start)
            check_ecs(True)
            player = e.cmd('edict 1')
            if not re.search(r'classname\s+player', player): raise RuntimeError(command + ' lost player:\n' + player)
        for name in ('e1m2', 'e1m1'):
            start = len(e.text()); e.cmd('map ' + name)
            e.wait(lambda t: 'CL_SignonReply: 4' in t, start)
            check_ecs(True)
            if not re.search(r'classname\s+player', e.cmd('edict 1')): raise RuntimeError('player missing after map ' + name)
        start = len(e.text()); e.cmd('playdemo demo1')
        e.wait(lambda t: 'CL_SignonReply: 4' in t, start)
        check_ecs(False)
        for position in ('15', '+5', '-5'):
            result = e.cmd('seek ' + position)
            if 'Not playing a demo' in result or 'Expected time format' in result:
                raise RuntimeError('seek failed: ' + result)
            time.sleep(.25)
            check_ecs(False)
        e.cmd('stopdemo')
        start = len(e.text()); e.cmd('map e1m1')
        e.wait(lambda t: 'CL_SignonReply: 4' in t, start)
        check_ecs(True)
        if a.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('Vulkan validation unavailable or reported an error')
        print('PASS: player entity, normal/fast load, map resets, demo seek and reconnect', flush=True)
    finally:
        e.close()


if __name__ == '__main__': main()
