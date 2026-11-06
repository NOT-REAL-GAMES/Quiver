#!/usr/bin/env python3
"""Fill all 1,048,576 edict slots and verify last-slot reuse on a real server.

Requires a built engine, Quake PAKs, an fteqcc compiler and compatible id1
QuakeC sources. The fixture uses invisible filler edicts and no clients; it
checks server-only capacity and reuse, not network transmission.
"""
import argparse
from pathlib import Path
import re
import shutil
import tempfile

from qbsp_smoke import brush
from stress_ecs_quakec import command


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', required=True, type=Path)
    p.add_argument('--basedir', required=True, type=Path)
    p.add_argument('--compiler', required=True, type=Path)
    p.add_argument('--quakec-source', required=True, type=Path)
    p.add_argument('--timeout', default=120, type=float)
    a = p.parse_args()
    binary, compiler, source = a.binary.resolve(), a.compiler.resolve(), a.quakec_source.resolve()
    pakdir = a.basedir.resolve()
    if (pakdir/'id1').is_dir(): pakdir /= 'id1'
    paks = sorted(x for x in pakdir.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {pakdir}')
    root = Path(__file__).resolve().parents[1]
    work = Path(tempfile.mkdtemp(prefix='quiver-ecs-capacity-'))
    print(f'results: {work}', flush=True)
    (work/'id1').mkdir(); (work/'user').mkdir(); (work/'probe/maps').mkdir(parents=True)
    for pak in paks: (work/'id1'/pak.name.lower()).symlink_to(pak)
    qc = work/'probe/qc'; shutil.copytree(source, qc, ignore=shutil.ignore_patterns('.git', '*.dat'))
    defs, world, src = qc/'defs.qc', qc/'world.qc', qc/'progs.src'
    with defs.open('a') as f: f.write('\nvoid() ecs_capacity_frame;\n')
    text, count = re.subn(r'(void\s*\(\s*\)\s*StartFrame\s*=\s*\{)',
                          r'\1\n    ecs_capacity_frame();', world.read_text(), count=1)
    if count != 1: raise RuntimeError('could not find StartFrame')
    text, count = re.subn(r'(?m)^\s*InitBodyQue\(\);', '', text, count=1)
    if count != 1: raise RuntimeError('could not remove test body queue')
    world.write_text(text)
    lines = src.read_text().splitlines(); lines[0] = '../progs.dat'
    src.write_text('\n'.join(lines) + '\necs_capacity.qc\n')
    shutil.copyfile(root/'tests/fixtures/ecs/capacity_probe.qc', qc/'ecs_capacity.qc')
    result, _ = command([compiler], qc, a.timeout)
    if result.returncode: raise RuntimeError('QuakeC compile failed:\n'+result.stdout)
    wad = root/'Misc/editor_defaults.wad'
    walls = [(-144,-144,-16,144,144,0), (-144,-144,128,144,144,144),
             (-144,-144,0,-128,144,128), (128,-144,0,144,144,128),
             (-128,-144,0,128,-128,128), (-128,128,0,128,144,128)]
    text = '{\n"classname" "worldspawn"\n"wad" "' + str(wad) + '"\n'
    text += ''.join(brush(*wall) for wall in walls) + '}\n'
    mapfile = work/'probe/maps/highid.map'; mapfile.write_text(text)
    result, _ = command([binary, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest',
                         mapfile, mapfile.with_suffix('.bsp')], work, a.timeout)
    if result.returncode: raise RuntimeError('map compile failed:\n'+result.stdout)
    result, elapsed = command([binary, '-dedicated', '1', '-basedir', '.', '-userdir', 'user', '-nosound', '-nojoy',
                               '-game', 'probe', '+developer', '1', '+max_edicts', '1048576',
                               '+sv_autosave', '0', '+host_framerate', '0.001', '+sys_ticrate', '0.001', '+map', 'highid'], work, a.timeout)
    (work/'output.log').write_text(result.stdout)
    required = ('ECS_CAPACITY_PASS',)
    if result.returncode or 'ECS_CAPACITY_FAIL' in result.stdout or not all(m in result.stdout for m in required):
        raise RuntimeError(f'capacity probe failed (exit {result.returncode}); inspect {work}/output.log\n'+result.stdout[-2000:])
    print(f'PASS: 1,048,576 slots allocated and last edict reused ({elapsed:.2f}s)')


if __name__ == '__main__': main()
