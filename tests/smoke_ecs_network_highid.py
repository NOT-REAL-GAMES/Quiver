#!/usr/bin/env python3
"""Exercise high-ID PEXT2 baselines, attachments, sound and deltas locally.

Requires Quake PAKs, fteqcc, compatible id1 QuakeC sources, a Vulkan display,
and a vkquake build with bundled map tools. Uses isolated disposable state.
"""
import argparse
from pathlib import Path
import re
import shutil
import tempfile
import time

import bench_meshlets as bench
from qbsp_smoke import brush
from stress_ecs_quakec import command


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--quakec-source', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=120)
    a = p.parse_args()
    a.binary = a.binary.resolve(); a.width = 1280; a.height = 720; a.game = 'probe'; a.validation = False
    source = a.quakec_source.resolve(); compiler = a.compiler.resolve(); pakdir = a.basedir.resolve()
    if (pakdir/'id1').is_dir(): pakdir /= 'id1'
    paks = sorted(x for x in pakdir.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {pakdir}')
    root = Path(__file__).resolve().parents[1]
    work = Path(tempfile.mkdtemp(prefix='quiver-ecs-network-highid-'))
    print(f'results: {work}', flush=True)
    (work/'id1').mkdir(); (work/'user').mkdir(); (work/'probe/maps').mkdir(parents=True)
    for pak in paks: (work/'id1'/pak.name.lower()).symlink_to(pak)
    qc = work/'probe/qc'; shutil.copytree(source, qc, ignore=shutil.ignore_patterns('.git', '*.dat'))
    defs, world, src = qc/'defs.qc', qc/'world.qc', qc/'progs.src'
    with defs.open('a') as f: f.write('\nvoid() ecs_net_setup;\nvoid() ecs_net_frame;\n')
    text, n = re.subn(r'(void\s*\(\s*\)\s*StartFrame\s*=\s*\{)', r'\1\n    ecs_net_frame();', world.read_text(), count=1)
    if n != 1: raise RuntimeError('could not find StartFrame')
    text, n = re.subn(r'(void\s*\(\s*\)\s*worldspawn\s*=\s*\{)', r'\1\n    ecs_net_setup();', text, count=1)
    if n != 1: raise RuntimeError('could not find worldspawn')
    text, n = re.subn(r'(?m)^\s*InitBodyQue\(\);', '', text, count=1)
    if n != 1: raise RuntimeError('could not remove test body queue')
    world.write_text(text)
    lines = src.read_text().splitlines(); lines[0] = '../progs.dat'; src.write_text('\n'.join(lines)+'\necs_network.qc\n')
    shutil.copyfile(root/'tests/fixtures/ecs/network_highid_probe.qc', qc/'ecs_network.qc')
    compiled, _ = command([compiler], qc, a.timeout)
    if compiled.returncode: raise RuntimeError('QC compile failed:\n'+compiled.stdout)
    wad = root/'Misc/editor_defaults.wad'
    walls = [(-144,-144,-16,144,144,0), (-144,-144,128,144,144,144),
             (-144,-144,0,-128,144,128), (128,-144,0,144,144,128),
             (-128,-144,0,128,-128,128), (-128,128,0,128,144,128)]
    text = '{\n"classname" "worldspawn"\n"wad" "' + str(wad) + '"\n'
    text += ''.join(brush(*wall) for wall in walls) + '}\n'
    text += '{\n"classname" "info_player_start"\n"origin" "-80 0 32"\n}\n'
    mapfile = work/'probe/maps/network_highid.map'; mapfile.write_text(text)
    built, _ = command([a.binary, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest',
                        mapfile, mapfile.with_suffix('.bsp')], work, a.timeout)
    if built.returncode: raise RuntimeError('map compile failed:\n'+built.stdout)
    (work/'id1/boot.cfg').write_text(bench.BOOT.replace('map start',
        'max_edicts 65544\nr_rtshadows 0\nsv_autosave 0\nmap network_highid'))
    e = bench.Engine(a, work)
    try:
        e.wait(lambda t: 'CL_SignonReply: 4' in t)
        if 'ECS_NET_FAIL' in e.text() or 'ECS_NET_BASELINE_READY' not in e.text():
            raise RuntimeError('QC did not initialize high baselines')
        status = e.cmd('pext')
        if 'Replacement Entity Deltas' not in status:
            raise RuntimeError('PEXT2 replacement deltas not active:\n'+status)
        e.cmd('host_maxfps 72')
        time.sleep(.5)
        def find_model(text, number):
            m = re.search(rf'(?m)^\s*{number}:progs/armor\.mdl:\s*\d+\s*\(\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)\)', text)
            return tuple(map(float, m.groups())) if m else None
        parent, child = find_model(e.cmd('entities 65536'), 65536), find_model(e.cmd('entities 65537'), 65537)
        if not parent or not child:
            raise RuntimeError('client did not receive high parent/child baselines; inspect '+str(work))
        e.cmd('cl_shownet 2')
        start = len(e.text()); e.cmd('impulse 211')
        e.wait(lambda t: 'ECS_NET_DELTA_READY' in t, start)
        time.sleep(.5)
        e.cmd('cl_shownet 0')
        dynamic = re.search(r'ECS_NET_DYNAMIC_ID\s+(\d+)', e.text()[start:])
        if not dynamic: raise RuntimeError('missing dynamic entity id; inspect '+str(work))
        number = int(dynamic[1])
        parent2 = find_model(e.cmd('entities 65536'), 65536)
        child2 = find_model(e.cmd('entities 65537'), 65537)
        created = find_model(e.cmd(f'entities {number}'), number)
        if not parent2 or not child2 or not created or abs(parent2[0]-48)>1 or abs(child2[0]-48)>1:
            raise RuntimeError(f'high deltas/attachment missing: parent={parent2}, child={child2}, created={created}; inspect {work}')
        if 'svc_sound' not in e.text()[start:]:
            raise RuntimeError('no sound packet observed after high-ID update; inspect '+str(work))
        if 'ECS_NET_FAIL' in e.text() or 'Bad server message' in e.text():
            raise RuntimeError('server/client protocol failure; inspect '+str(work))
        print(f'PASS: baseline parent={parent}, child={child}; high delta parent={parent2}, child={child2}, spawned={created}; sound packet parsed')
    finally:
        e.close()


if __name__ == '__main__': main()
