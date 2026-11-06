#!/usr/bin/env python3
"""Exercise native sector-prop spawning, player pushing, collision and saves.

Requires an SDL/Vulkan display and local Quake PAK data. Everything writable is
created in a temporary directory; the fixture is a conventional brush map.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

import bench_meshlets as bench
from qbsp_smoke import brush


def origin(output):
    match = re.search(r"origin\s+'\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s*'", output)
    if not match:
        raise RuntimeError('missing entity origin:\n' + output)
    return tuple(map(float, match.groups()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=30)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args(); args.width, args.height = 1280, 720
    args.binary = args.binary.resolve(); source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-prop-runtime-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmap start') if args.validation else bench.BOOT
    (work / 'id1/boot.cfg').write_text(boot)
    wad = Path(__file__).resolve().parents[1] / 'Misc/editor_defaults.wad'
    walls = [(-144,-144,-16,144,144,0),(-144,-144,128,144,144,144),
             (-144,-144,0,-128,144,128),(128,-144,0,144,144,128),
             (-128,-144,0,128,-128,128),(-128,128,0,128,144,128)]
    text = '{\n"classname" "worldspawn"\n"wad" "' + str(wad) + '"\n'
    text += ''.join(brush(*wall) for wall in walls) + '}\n'
    text += '{\n"classname" "info_player_start"\n"origin" "-80 0 32"\n"angle" "0"\n}\n'
    text += '{\n"classname" "light"\n"origin" "0 0 110"\n"light" "500"\n}\n'
    for classname, position, extra in (('prop_static', '0 85 16', ''),
                                       ('prop_dynamic', '-50 -85 16', '"velocity" "10 0 0"\n'),
                                       ('prop_movable', '0 0 16', '')):
        text += '{\n"classname" "' + classname + '"\n"origin" "' + position + '"\n' + extra
        text += brush(-16,-16,-16,16,16,16) + '}\n'
    src = work / 'id1/maps/prop_runtime.map'; src.write_text(text)
    bsp = src.with_suffix('.bsp')
    for cmd in (('qbsp','-bsp2','-nopercent','-leaktest',src,bsp), ('vis','-threads','2',bsp), ('light','-threads','2',bsp)):
        result = subprocess.run([str(args.binary), '--quiver-map-tool', *map(str,cmd)], cwd=work,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode: raise RuntimeError(f'{cmd[0]} failed:\n{result.stdout}')
    e = bench.Engine(args, work)
    try:
        e.wait(lambda t: 'CL_SignonReply: 4' in t)
        start = len(e.text()); e.cmd('map prop_runtime'); e.wait(lambda t: 'CL_SignonReply: 4' in t, start)
        entities = e.cmd('edicts')
        ids = {}
        for match in re.finditer(r'EDICT\s+(\d+):\s*(.*?)(?=\n\s*EDICT\s+\d+:|\nFree-list:|\Z)', entities, re.S):
            for classname in ('prop_static', 'prop_dynamic', 'prop_movable'):
                if re.search(r'(?m)^classname\s+' + classname + r'\s*$', match[2]):
                    ids[classname] = int(match[1])
        if len(ids) != 3: raise RuntimeError('native prop entities were not spawned:\n' + entities)
        def get(classname): return e.cmd(f'edict {ids[classname]}')
        static = get('prop_static'); dynamic = get('prop_dynamic'); moving = get('prop_movable')
        if 'SOLID_BSP' not in static or 'MOVETYPE_PUSH' not in static: raise RuntimeError('static prop is not a solid BSP')
        if 'SOLID_BSP' not in dynamic or 'MOVETYPE_PUSH' not in dynamic: raise RuntimeError('dynamic prop is not a BSP pusher')
        if 'SOLID_BBOX' not in moving or 'MOVETYPE_TOSS' not in moving: raise RuntimeError('movable prop is not a tossed AABB')
        dynamic_before = origin(dynamic)[0]
        e.cmd('host_maxfps 72\n+forward'); time.sleep(3.0)
        e.cmd('-forward')
        dynamic_after = origin(get('prop_dynamic'))[0]
        moved = origin(get('prop_movable'))[0]
        if dynamic_after <= dynamic_before + 10: raise RuntimeError(f'dynamic prop did not advance: {dynamic_before} -> {dynamic_after}')
        if not 20 < moved <= 112.1: raise RuntimeError(f'walking did not push movable prop safely: x={moved}')
        output = e.cmd('save prop_runtime_save')
        if 'Saving game' not in output: raise RuntimeError('save failed:\n' + output)
        before_load = origin(get('prop_movable'))[0]
        start = len(e.text()); e.cmd('load prop_runtime_save'); e.wait(lambda t: 'CL_SignonReply: 4' in t, start)
        after_load = origin(get('prop_movable'))[0]
        if abs(after_load - before_load) > 1.0: raise RuntimeError(f'prop position changed across save/load: {before_load} -> {after_load}')
        # Push again after loading, then hold against the end wall. The crate
        # must resume responding to the player without crossing world geometry.
        e.cmd('+forward'); time.sleep(3.0); e.cmd('-forward')
        stopped = origin(get('prop_movable'))[0]
        if stopped <= after_load + 1 or stopped > 112.1:
            raise RuntimeError(f'loaded prop did not push safely against the wall: {after_load} -> {stopped}')
        # Fastload reuses the server VM and edict slots. It must rebuild ECS
        # identities while retaining the original saved gameplay state.
        e.cmd('fastload prop_runtime_save')
        after_fastload = origin(get('prop_movable'))[0]
        if abs(after_fastload - before_load) > 1.0:
            raise RuntimeError(f'prop position changed across fastload: {before_load} -> {after_fastload}')
        e.cmd('+forward'); time.sleep(3.0); e.cmd('-forward')
        fast_stopped = origin(get('prop_movable'))[0]
        if fast_stopped <= after_fastload + 1 or fast_stopped > 112.1:
            raise RuntimeError(f'fastloaded prop did not push safely against the wall: {after_fastload} -> {fast_stopped}')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: native props spawn, dynamic motion, player push, wall collision, save/load and fastload', flush=True)
    finally:
        if e.process.poll() is None: e.process.terminate(); e.process.wait(timeout=10)
        e.close()

if __name__ == '__main__': main()
