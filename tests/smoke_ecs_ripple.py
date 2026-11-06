#!/usr/bin/env python3
"""Run the scripted local ripple in a disposable game directory.

Requires local Quake PAKs, a built engine and a compiled ripple progs.dat.
"""
import argparse
import math
from pathlib import Path
import re
import statistics
import tempfile
import time

import bench_meshlets as bench


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--progs', type=Path, required=True)
    p.add_argument('--count', type=int, choices=(1024, 32768), default=32768,
                   help='playable ripple level to exercise (default: 32768)')
    p.add_argument('--timeout', type=float, default=120)
    p.add_argument('--validation', action='store_true')
    p.add_argument('--capture', action='store_true', help='save a screenshot in the disposable test directory')
    p.add_argument('--profile', action='store_true', help='sample uncapped host frame timing and process RSS')
    a = p.parse_args()
    a.binary = a.binary.resolve(); a.width = 1280; a.height = 720; a.game = 'quiver_ripple'
    source = a.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {source}')
    if not a.progs.is_file(): p.error(f'no compiled gamecode: {a.progs}')
    work = Path(tempfile.mkdtemp(prefix='quiver-ripple-smoke-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1').mkdir(); (work / a.game).mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / a.game / 'progs.dat').symlink_to(a.progs.resolve())
    boot = bench.BOOT.replace('map start', f'r_rtshadows 0\nmax_edicts 32800\nhost_maxfps 30\nmap ecs{a.count}')
    (work / 'id1/boot.cfg').write_text(boot)
    engine = bench.Engine(a, work)

    def stats(index):
        text = engine.cmd(f'ecs_stress_stats\necs_visual_stats {index}')
        count = re.search(r'ECS visuals: local=(\d+)', text)
        stress = re.search(r'ECS stress: active=(\d+) entities=(\d+) ticks=(\d+) updates=(\d+) failures=(\d+)', text)
        position = re.search(r'ECS visual \d+:\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)', text)
        if not count or int(count.group(1)) != a.count or not stress or not position:
            raise RuntimeError(f'missing complete visual snapshot: {text}')
        active, entities, ticks, updates, failures = map(int, stress.groups())
        if active != 1 or entities != a.count or not ticks or updates != ticks * a.count or failures:
            raise RuntimeError(f'invalid ECS flip workload: {text}')
        coords = tuple(map(float, position.groups()))
        sample = re.search(r'ECS ripple:.*time=([-\d.]+)', text)
        if not sample:
            raise RuntimeError(f'missing ripple simulation time: {text}')
        tick_time = float(sample.group(1))
        expected = 160 + 24 * math.sin(tick_time * 2 * math.pi / 3 - math.hypot(*coords[:2]) * 2 * math.pi / 256)
        if abs(expected - coords[2]) > .01:
            raise RuntimeError(f'ripple position {coords} does not match time {tick_time}: {expected}')
        return coords, text

    def revision(text):
        match = re.search(r'ECS visuals: local=\d+.*revision=(\d+)', text)
        if not match or int(match.group(1)) == 0:
            raise RuntimeError(f'missing published visual revision: {text}')
        return int(match.group(1))

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        deadline = time.monotonic() + a.timeout
        while True:
            try:
                first, result = stats(0)
                break
            except RuntimeError:
                if time.monotonic() >= deadline: raise
                time.sleep(.2)
        last, _ = stats(a.count - 1)
        extents = ((-496., -248.), (496., 248.)) if a.count == 1024 else ((-510., -254.), (510., 254.))
        if (first[:2], last[:2]) != extents:
            raise RuntimeError(f'incorrect grid extents: {first}, {last}')
        time.sleep(.5)
        later, _ = stats(0)
        if abs(later[2] - first[2]) < .01:
            raise RuntimeError(f'authoritative Z did not animate: {first}, {later}')
        print(result.strip(), flush=True)
        print(f'PASS: local visuals {a.count}; first {first} -> {later}; last {last}', flush=True)
        if a.capture:
            print(f'screenshot: {engine.photo("ripple")}', flush=True)
        if a.profile:
            engine.cmd('host_maxfps 0\nhost_speeds 1')
            time.sleep(.5)  # warm the renderer and profiling path
            start_offset = len(engine.text())
            start_time = time.monotonic()
            time.sleep(3)
            elapsed = time.monotonic() - start_time
            measured = engine.text()[start_offset:]
            engine.cmd('host_speeds 0\nhost_maxfps 30')
            samples = [tuple(map(float, row)) for row in re.findall(
                r'([\d.]+) tot\s+([\d.]+) server\s+([\d.]+) gfx\s+([\d.]+) snd', measured)]
            if not samples:
                raise RuntimeError('no host frame timing samples')
            status = Path(f'/proc/{engine.process.pid}/status').read_text()
            rss = re.search(r'^VmRSS:\s+(\d+)\s+kB', status, re.M)
            if not rss: raise RuntimeError('could not read process RSS')
            print(f'PROFILE: frames={len(samples)} elapsed_s={elapsed:.3f} fps={len(samples)/elapsed:.2f} '
                  f'host_total_median_ms={statistics.median(x[0] for x in samples):.2f} '
                  f'host_server_median_ms={statistics.median(x[1] for x in samples):.2f} '
                  f'host_gfx_median_ms={statistics.median(x[2] for x in samples):.2f} '
                  f'rss_mib={int(rss.group(1))/1024:.1f}', flush=True)
        engine.cmd('pause')
        paused, paused_log = stats(0)
        paused_revision = revision(paused_log)
        time.sleep(.3)
        still, still_log = stats(0)
        before_tick = re.search(r'ECS stress:.*ticks=(\d+)', paused_log)
        after_tick = re.search(r'ECS stress:.*ticks=(\d+)', still_log)
        if not before_tick or not after_tick or before_tick.group(1) != after_tick.group(1) or paused != still:
            raise RuntimeError('ripple advanced while paused')
        if revision(still_log) != paused_revision:
            raise RuntimeError('snapshot revision changed while paused')
        engine.cmd('pause')
        time.sleep(.2)
        resumed, resumed_log = stats(0)
        if abs(resumed[2] - still[2]) < .01:
            raise RuntimeError('ripple did not resume')
        previous_revision = revision(resumed_log)
        if previous_revision <= paused_revision:
            raise RuntimeError('snapshot revision did not advance after resume')
        if 'Saving game' not in engine.cmd('save ecs_ripple_smoke'):
            raise RuntimeError('saving ripple level failed')
        for command in ('fastload', 'load'):
            start = len(engine.text())
            output = engine.cmd(f'{command} ecs_ripple_smoke')
            if "Can't fastload" in output or 'ERROR' in output:
                raise RuntimeError(f'{command} failed: {output}')
            if command == 'load': engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
            _, loaded_log = stats(0); stats(a.count - 1)
            loaded_revision = revision(loaded_log)
            if loaded_revision <= previous_revision:
                raise RuntimeError(f'{command} did not publish a newer snapshot revision')
            previous_revision = loaded_revision
        print('PASS: pause/resume, save, fastload and load', flush=True)
        start = len(engine.text())
        engine.cmd('map start')
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        switched = engine.cmd('ecs_visual_stats')
        if not re.search(r'ECS visuals: local=0\b', switched):
            raise RuntimeError('visuals leaked into next map')
        if not re.search(r'revision=0\b', switched):
            raise RuntimeError('snapshot revision remained valid after map switch')
        if a.validation:
            output = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in output or re.search(r'VUID-|Validation Error|SYNC-HAZARD', output):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: map switch cleared visuals', flush=True)
    finally:
        engine.close()


if __name__ == '__main__': main()
