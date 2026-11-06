#!/usr/bin/env python3
"""Exercise the packaged playable ECS stress level using a real client/server.

Uses a disposable user/config directory and locally supplied Quake PAKs.
Requires SDL development headers, a Vulkan display and an engine binary with
its packaged resources. Does not alter the user's configuration or save files.
"""
import argparse
import math
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench

STATS = re.compile(r'ECS stress: active=(\d+) entities=(\d+) ticks=(\d+) updates=(\d+) failures=(\d+) mean_ms=([\d.]+)')
ORIGIN = re.compile(r"origin\s+'\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s*'")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=45)
    p.add_argument('--validation', action='store_true')
    a = p.parse_args()
    a.binary = a.binary.resolve(); a.width = 1280; a.height = 720
    source = a.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(x for x in source.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-ecs-playable-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1').mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    # Keep all tests isolated and exercise the lowest reasonable capacity,
    # without changing the user's one-million setting.
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', 'r_rtshadows 0\nmax_edicts 32800\nmap start'))
    e = bench.Engine(a, work)
    try:
        def get_stats():
            out = e.cmd('ecs_stress_stats')
            match = STATS.search(out)
            if not match: raise RuntimeError('missing stress stats:\n' + out)
            active, entities, ticks, updates, failures = map(int, match.groups()[:5])
            return dict(active=active, entities=entities, ticks=ticks, updates=updates, failures=failures)

        def ready():
            deadline = time.monotonic() + a.timeout
            while time.monotonic() < deadline:
                stat = get_stats()
                if stat['active'] and stat['entities'] == 32768 and stat['ticks'] > 0:
                    if stat['failures'] or stat['updates'] != stat['entities'] * stat['ticks']:
                        raise RuntimeError('incorrect stress updates: ' + str(stat))
                    return stat
                time.sleep(.05)
            raise RuntimeError('stress workload never became ready: ' + str(stat))

        def load_map(name):
            start = len(e.text())
            e.cmd('map ' + name)
            e.wait(lambda text: 'CL_SignonReply: 4' in text, start)

        def player_origin():
            out = e.cmd('edict 1')
            if not re.search(r'classname\s+player', out):
                raise RuntimeError('player missing:\n' + out)
            match = ORIGIN.search(out)
            if not match: raise RuntimeError('player origin missing:\n' + out)
            return tuple(map(float, match.groups()))

        e.wait(lambda t: 'CL_SignonReply: 4' in t)
        if 'inactive' not in e.cmd('ecs_stress_stats'):
            raise RuntimeError('stress workload active on ordinary map')
        load_map('ecs32768')
        first = ready()
        e.cmd('host_maxfps 72')
        before = player_origin()
        e.cmd('+forward'); time.sleep(.45); e.cmd('-forward')
        after = player_origin()
        if math.dist(before, after) < 2:
            raise RuntimeError(f'player could not move: {before} -> {after}')
        second = ready()
        if second['ticks'] <= first['ticks']:
            raise RuntimeError(f'stress did not continue while playing: {first} -> {second}')
        e.cmd('pause')
        paused = get_stats(); time.sleep(.25); still_paused = get_stats()
        if paused['ticks'] != still_paused['ticks']:
            raise RuntimeError(f'stress advanced while paused: {paused} -> {still_paused}')
        e.cmd('pause')
        resumed = ready()
        if resumed['ticks'] <= still_paused['ticks']:
            time.sleep(.12); resumed = ready()
        if resumed['ticks'] <= still_paused['ticks']:
            raise RuntimeError('stress did not resume after pause')
        output = e.cmd('save ecs_playable')
        if 'Saving game' not in output: raise RuntimeError('save failed:\n' + output)
        for command in ('fastload', 'load'):
            start = len(e.text()); out = e.cmd(f'{command} ecs_playable')
            if "Can't fastload" in out or 'ERROR' in out:
                raise RuntimeError(f'{command} failed:\n' + out)
            if command == 'load': e.wait(lambda text: 'CL_SignonReply: 4' in text, start)
            ready(); player_origin()
        load_map('ecs32768'); ready()
        load_map('start')
        if 'inactive' not in e.cmd('ecs_stress_stats'):
            raise RuntimeError('stress workload active after switching maps')
        e.cmd('max_edicts 256')
        load_map('ecs32768')
        low = get_stats()
        if low['active'] or low['entities']:
            raise RuntimeError('low-capacity map started an incomplete workload: ' + str(low))
        if 'not enough edict slots' not in e.text():
            raise RuntimeError('low-capacity map did not report why workload is inactive')
        if a.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer unavailable or reported an error')
        print('PASS: playable movement, 32768 ECS flips, pause/resume, save/load, fastload, reload, map switch and low capacity', flush=True)
    finally:
        e.close()


if __name__ == '__main__': main()
