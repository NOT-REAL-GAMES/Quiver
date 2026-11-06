#!/usr/bin/env python3
"""Compare ECS and baseline builds using isolated timedemos and server ticks.

Requires local Quake PAKs, a Vulkan display and SDL development headers. Results
and logs are kept in a temporary directory; saved user settings are untouched.
Server measurements use a freshly loaded map and sv_speeds instrumentation,
separately from the uninstrumented timedemo throughput measurements.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import tempfile
import time
from types import SimpleNamespace

import bench_meshlets as bench

TICK = re.compile(r'sv_speeds:\s*(\d+) ticks \| clients ([\d.]+) \| physics ([\d.]+).*?\| stats ([\d.]+) \| send ([\d.]+)')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--demo', default='demo1')
    p.add_argument('--map', default='e1m1')
    p.add_argument('--pairs', type=int, default=4)
    p.add_argument('--replays', type=int, default=3, help='timed replays per process after warmup')
    p.add_argument('--width', type=int, default=1280)
    p.add_argument('--height', type=int, default=720)
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--server-seconds', type=float, default=4)
    a = p.parse_args()
    if a.pairs < 1 or a.replays < 1 or a.server_seconds < 2 or not re.fullmatch(r'[A-Za-z0-9_]+', a.map) or not re.fullmatch(r'[A-Za-z0-9_.-]+', a.demo):
        p.error('invalid run count, duration, map, or demo')
    binaries = {'baseline': a.baseline.resolve(), 'candidate': a.candidate.resolve()}
    source = a.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(x for x in source.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {source}')
    root = Path(tempfile.mkdtemp(prefix='quiver-ecs-bench-'))
    print(f'results: {root}', flush=True)
    report = dict(binaries={k: dict(path=str(v), sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for k,v in binaries.items()},
                  assets=[str(x) for x in paks], demo=a.demo, map=a.map, pairs=[], resolution=[a.width,a.height])

    def measure(label, iteration):
        work = root / f'{iteration:02d}-{label}'
        (work / 'id1').mkdir(parents=True); (work / 'user').mkdir()
        for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
        (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', f'r_rtshadows 0\nmap {a.map}'))
        args = SimpleNamespace(binary=binaries[label], width=a.width, height=a.height, timeout=a.timeout,
                               validation=False, stock_only=True, game=None)
        engine = bench.Engine(args, work)
        try:
            engine.wait(lambda t: 'CL_SignonReply: 4' in t)
            engine.cmd('host_maxfps 72\nsv_speeds 1')
            time.sleep(a.server_seconds)
            sample = engine.cmd('sv_speeds 0')
            # sv_speeds prints once a second, from the full log window.
            ticks = TICK.findall(engine.text())
            if not ticks: raise RuntimeError(f'no server tick timing in {work}: {sample}')
            rss = None
            status = Path(f'/proc/{engine.process.pid}/status')
            if status.exists():
                match = re.search(r'^VmRSS:\s*(\d+)\s*kB', status.read_text(), re.M)
                if match: rss = int(match[1])
            ecs_stats = engine.cmd('ecs_stats')
            ecs_memory = re.search(r'ECS allocation \(bytes, excluding allocator overhead\): server (\d+), client QC (\d+), client render (\d+)', ecs_stats)
            engine.cmd('host_maxfps 0')
            warm = engine.demo(a.demo, 0, warmup=True)
            timed = [engine.demo(a.demo, 0) for _ in range(a.replays)]
            if len({x['frames'] for x in [warm, *timed]}) != 1:
                raise RuntimeError('timedemo frame counts differ between replays')
            return dict(frame_ms=statistics.median(x['frame_ms'] for x in timed), frames=timed[0]['frames'],
                        replay_frame_ms=[x['frame_ms'] for x in timed],
                        server_physics_ms=statistics.median(float(x[2]) for x in ticks[1:] or ticks),
                        server_samples=len(ticks), rss_kib=rss,
                        ecs_bytes=dict(zip(('server', 'client_qc', 'client_render'), map(int, ecs_memory.groups()))) if ecs_memory else None,
                        warmup=warm)
        finally:
            engine.close()

    try:
        for i in range(a.pairs):
            row = {}
            for label in bench.balanced_order(['baseline', 'candidate'], i):
                row[label] = measure(label, i)
                print(f"{label}: {row[label]}", flush=True)
            if row['baseline']['frames'] != row['candidate']['frames']:
                raise RuntimeError('timedemo frame counts differ')
            report['pairs'].append(row)
        for metric in ('frame_ms', 'server_physics_ms', 'rss_kib'):
            values = [100 * (r['candidate'][metric] / r['baseline'][metric] - 1)
                      for r in report['pairs'] if r['baseline'][metric] and r['candidate'][metric] is not None]
            report[metric] = {k: statistics.median(r[k][metric] for r in report['pairs'] if r[k][metric] is not None)
                              for k in binaries}
            report[metric]['paired_change_percent'] = statistics.median(values) if values else None
        report['success'] = True
    finally:
        (root / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__': main()
