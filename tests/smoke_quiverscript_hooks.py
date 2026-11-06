#!/usr/bin/env python3
"""Test optional map lifecycle QC hooks in a disposable single-player game.

Requires a built engine, compatible id1 QuakeC sources, FTEQCC and local PAKs.
"""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

import bench_meshlets as bench

ROOT = Path(__file__).resolve().parents[1]
QC = '''
void() quiver_map_start = {
    if (self != world) dprint("QS_CONTEXT_ERROR\\n");
    dprint("QS_START_HOOK\\n");
};
void(float tick_time) quiver_map_tick = {
    if (self != world || tick_time != time) dprint("QS_CONTEXT_ERROR\\n");
    if (tick_time < 1.19) dprint("QS_EARLY_TICK\\n");
    dprint("QS_TICK_HOOK\\n");
};
'''


def fixture_map(path, marked):
    data = bytearray((ROOT / 'Misc/vq_pak/maps/ecs32768.bsp').read_bytes())
    offset, length = struct.unpack_from('<ii', data, 4)
    entities = bytes(data[offset:offset + length]).rstrip(b'\0')
    entities = entities.replace(b'"_quiver_ecs_stress" "32768"', b'')
    if marked:
        pos = entities.index(b'{') + 1
        entities = entities[:pos] + b'\n"_quiver_script" "scripts/hooks.qs"\n' + entities[pos:]
    entities += b'\0'
    struct.pack_into('<ii', data, 4, len(data), len(entities))
    data.extend(entities)
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--quakec-source', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--validation', action='store_true')
    a = parser.parse_args()
    a.binary = a.binary.resolve(); a.width = 1280; a.height = 720; a.game = 'quiver_hooks'
    source = a.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(x for x in source.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-script-hooks-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / a.game).mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    fixture_map(work / 'id1/maps/hooks_on.bsp', True)
    fixture_map(work / 'id1/maps/hooks_off.bsp', False)
    qc = work / 'qc'; shutil.copytree(a.quakec_source.resolve(), qc)
    src = qc / 'progs.src'; lines = src.read_text().splitlines(); lines[0] = '../progs.dat'
    src.write_text('\n'.join(lines) + '\nhooks_probe.qc\n')
    (qc / 'hooks_probe.qc').write_text(QC)
    result = subprocess.run([str(a.compiler.resolve()), '-O0'], cwd=qc, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode: raise RuntimeError('QC compile failed:\n' + result.stdout)
    shutil.copy2(work / 'progs.dat', work / a.game / 'progs.dat')
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', 'r_rtshadows 0\nmax_edicts 1024\nhost_maxfps 30\nmap start'))
    e = bench.Engine(a, work)
    try:
        e.wait(lambda t: 'CL_SignonReply: 4' in t)
        time.sleep(.2)
        if 'QS_START_HOOK' in e.text() or 'QS_TICK_HOOK' in e.text():
            raise RuntimeError('hooks ran without worldspawn script marker')
        pos = len(e.text()); e.cmd('map hooks_on')
        e.wait(lambda t: 'CL_SignonReply: 4' in t and 'QS_TICK_HOOK' in t, pos)
        initial = e.text()[pos:]
        if initial.count('QS_START_HOOK') != 1 or 'QS_CONTEXT_ERROR' in initial or 'QS_EARLY_TICK' in initial:
            raise RuntimeError('fresh map lifecycle or context incorrect:\n' + initial)
        e.cmd('pause'); time.sleep(.2)
        paused = e.text().count('QS_TICK_HOOK'); time.sleep(.3)
        if e.text().count('QS_TICK_HOOK') != paused:
            raise RuntimeError('map hook tick ran while paused')
        e.cmd('pause')
        if 'Saving game' not in e.cmd('save hooks_test'): raise RuntimeError('save failed')
        for command in ('fastload', 'load'):
            pos = len(e.text()); e.cmd(f'{command} hooks_test')
            if command == 'load': e.wait(lambda t: 'CL_SignonReply: 4' in t, pos)
            e.wait(lambda t: 'QS_TICK_HOOK' in t, pos)
            restored = e.text()[pos:]
            if 'QS_START_HOOK' in restored or 'QS_CONTEXT_ERROR' in restored:
                raise RuntimeError(command + ' repeated start hook or corrupted context:\n' + restored)
        pos = len(e.text()); e.cmd('map hooks_off')
        e.wait(lambda t: 'CL_SignonReply: 4' in t, pos)
        time.sleep(.3)
        unmarked = e.text()[pos:]
        if 'QS_START_HOOK' in unmarked or 'QS_TICK_HOOK' in unmarked:
            raise RuntimeError('hooks leaked onto unmarked map:\n' + unmarked)
        print('PASS: map hook gate, once-only start, tick context, pause, fast/full load and map switch', flush=True)
    finally:
        e.close()


if __name__ == '__main__': main()
