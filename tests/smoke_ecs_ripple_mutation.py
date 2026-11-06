#!/usr/bin/env python3
"""Exercise QC ripple callbacks, entity mutation and save/load in a disposable game.

Requires local Quake PAKs, FTEQCC and compatible id1 QuakeC sources.  The test
builds its own tiny QC callback and never changes the user's game or saves.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

import bench_meshlets as bench

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = '''
float quiver_probe_spawned;
void(entity target, float tick_time) quiver_ripple_tick = {
    local vector position;
    local entity created;
    if (quiver_probe_spawned == 0) {
        quiver_probe_spawned = 1;
        created = spawn();
        created.classname = "quiver_probe_spawn";
    }
    position = target.origin;
    position_z = 160 + tick_time;
    setorigin(target, position);
};
'''
FREE_SCRIPT = '''
void(entity target, float tick_time) quiver_ripple_tick = {
    remove(target);
};
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--quakec-source', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=120)
    p.add_argument('--validation', action='store_true')
    a = p.parse_args()
    a.binary = a.binary.resolve(); a.width = 1280; a.height = 720; a.game = 'quiver_probe'
    compiler = a.compiler.resolve(); qc_source = a.quakec_source.resolve()
    source = a.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(x for x in source.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-ripple-mutation-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / a.game).mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    shutil.copy2(ROOT / 'Misc/vq_pak/maps/ecs32768.bsp', work / 'id1/maps/stress_alias.bsp')
    qc = work / 'qc'; shutil.copytree(qc_source, qc)
    src = qc / 'progs.src'
    lines = src.read_text().splitlines(); lines[0] = '../progs.dat'
    src.write_text('\n'.join(lines) + '\nquiver_probe.qc\n')

    def compile_probe(code):
        (qc / 'quiver_probe.qc').write_text(code)
        result = subprocess.run([str(compiler), '-O0'], cwd=qc, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode: raise RuntimeError('QC compile failed:\n' + result.stdout)
        shutil.copy2(work / 'progs.dat', work / a.game / 'progs.dat')

    def start():
        boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmax_edicts 32800\nhost_maxfps 30\nmap stress_alias')
        (work / 'id1/boot.cfg').write_text(boot)
        # A second engine run must not observe the previous run's signon log.
        (work / 'xdg/vkQuake/qconsole.log').unlink(missing_ok=True)
        return bench.Engine(a, work)

    def snapshot(e, index):
        text = e.cmd(f'ecs_stress_stats\necs_visual_stats {index}')
        stats = re.search(r'ECS stress: active=(\d+) entities=(\d+) ticks=(\d+) updates=(\d+) failures=(\d+)', text)
        visual = re.search(r'ECS visuals: local=(\d+)', text)
        position = re.search(r'ECS visual \d+:\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)', text)
        timestamp = re.search(r'ECS ripple: .*?time=([-\d.]+)', text)
        if not stats or not visual: raise RuntimeError('missing stats: ' + text)
        return tuple(map(int, stats.groups())), int(visual.group(1)), (tuple(map(float, position.groups())) if position else None), (float(timestamp.group(1)) if timestamp else None)

    compile_probe(SCRIPT)
    e = start()
    try:
        e.wait(lambda t: 'CL_SignonReply: 4' in t)
        deadline = time.monotonic() + a.timeout
        while True:
            data = snapshot(e, 0)
            if data[1] == 32768 and data[0][2] > 0: break
            if time.monotonic() >= deadline: raise RuntimeError('ripple did not start: ' + repr(data))
            time.sleep(.1)
        e.cmd('pause'); time.sleep(.2)
        state, count, pos, tick = snapshot(e, 0)
        assert state[0] == 1 and state[1] == 32768 and state[4] == 0 and count == 32768, (state, count)
        assert pos and pos[:2] == (-510., -254.) and tick is not None and abs(pos[2] - (160 + tick)) < .02, (pos, tick)
        output = e.cmd('edictcount')
        high = re.search(r'num_edicts\s*:\s*(\d+)', output)
        # Player spawn may add temporary edicts after the callback's spawn.
        high_slot = int(high.group(1)) if high else 0
        tail = '\n'.join(f'edict {i}' for i in range(max(1, high_slot - 32), high_slot))
        if not high or 'quiver_probe_spawn' not in e.cmd(tail):
            raise RuntimeError('QC callback did not create its probe entity: ' + output)
        e.cmd('pause')
        out = e.cmd('save ripple_probe')
        if 'Saving game' not in out: raise RuntimeError('save failed: ' + out)
        for command in ('fastload', 'load'):
            start_offset = len(e.text())
            out = e.cmd(f'{command} ripple_probe')
            if "Can't fastload" in out or 'ERROR' in out: raise RuntimeError(command + ' failed: ' + out)
            if command == 'load': e.wait(lambda t: 'CL_SignonReply: 4' in t, start_offset)
            deadline = time.monotonic() + a.timeout
            while True:
                state, count, pos, tick = snapshot(e, 32767)
                if state[0] and state[1] == 32768 and count == 32768 and state[2] > 0: break
                if time.monotonic() >= deadline: raise RuntimeError(f'{command} lost ripple: {(state, count, pos, tick)}')
                time.sleep(.1)
            if state[4] or pos[:2] != (510., 254.): raise RuntimeError(f'{command} corrupted ripple: {(state, count, pos)}')
        print('PASS: tagged alias map, QC entity spawn, exact Z/time, save and fast/full load', flush=True)
    finally:
        e.close()

    compile_probe(FREE_SCRIPT)
    e = start()
    try:
        e.wait(lambda t: 'CL_SignonReply: 4' in t)
        deadline = time.monotonic() + a.timeout
        while True:
            state, count, _, _ = snapshot(e, 0)
            if state[4] > 0: break
            if time.monotonic() >= deadline: raise RuntimeError('free callback was not run: ' + repr(state))
            time.sleep(.1)
        if count: raise RuntimeError(f'partial snapshot published after callback freed entities: {count}')
        print('PASS: QC freeing stress entities suppresses partial local snapshot', flush=True)
    finally:
        e.close()


if __name__ == '__main__': main()
