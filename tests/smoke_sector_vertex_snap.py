#!/usr/bin/env python3
"""Exercise real 3D vertex snapping, welding, save/reopen and F9 playtest.

Requires a Vulkan display, SDL development headers and local Quake PAK data.
The fixture and all writable data are placed in an isolated temporary directory.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import tempfile

import bench_meshlets as bench
import smoke_sector_editor as smoke


# Reuse the existing SDL injector; only replace its test-specific mouse
# gestures. Click inside the first room's front wall, then drag its top-right
# corner onto a nearby off-grid corner of the second sector.
old_gestures = '''if (action == 'd') { mx = cx; my = cy - (step == 2 ? 48 : 0); }
            else if (action == 'm') { mx = cx - 416 - (step == 2 ? 32 : 0); my = cy - 286; }
            else { mx = cx + (action == 'j' ? 2 : 0); my = cy + (action == 'j' ? 2 : 0); }'''
new_gestures = '''if (action == 'd') { mx = cx - 415 - (step == 2 ? 1 : 0); my = cy - (step == 2 ? 270 : 311); }
            else if (action == 'h') { mx = cx - 208; my = cy; }
            else { mx = cx; my = cy; }'''
assert old_gestures in smoke.bench.SHIM
bench.SHIM = smoke.bench.SHIM.replace(old_gestures, new_gestures)


def fixture():
    boxes = (((-128, -128, 0), (0, 0, 128)), ((10, 10, 0), (138, 138, 128)))
    faces = ((0, 4, 6, 2), (1, 3, 7, 5), (0, 1, 5, 4),
             (2, 6, 7, 3), (0, 2, 3, 1), (4, 5, 7, 6))
    vertices = [tuple(hi[a] if corner & (1 << a) else lo[a] for a in range(3))
                for lo, hi in boxes for corner in range(8)]
    entities = ('{\n"classname" "worldspawn"\n}\n'
                '{\n"classname" "info_player_start"\n"origin" "-64 -64 32"\n}\n'
                '{\n"classname" "light"\n"origin" "-64 -64 96"\n"light" "400"\n}\n')
    source = 'QSECTOR 1\nVERTICES 16\n'
    source += ''.join('{} {} {}\n'.format(*vertex) for vertex in vertices)
    source += 'SECTORS 2\n'
    for sector in range(2):
        source += 'SECTOR 6\n'
        source += ''.join('FACE 4 -1 -1 stone1 {} {} {} {}\n'.format(*(v + sector * 8 for v in face)) for face in faces)
    return source + f'ENTITIES {len(entities.encode())}\n' + entities


def verify_weld(path):
    lines = iter(path.read_text().splitlines())
    assert next(lines) == 'QSECTOR 1'
    count = int(next(lines).split()[1])
    vertices = [tuple(map(float, next(lines).split())) for _ in range(count)]
    assert next(lines) == 'SECTORS 2'
    indices = []
    for _ in range(2):
        face_count = int(next(lines).split()[1]); used = set()
        for _ in range(face_count):
            fields = next(lines).split()
            assert fields[0] == 'FACE'
            used.update(map(int, fields[5:5 + int(fields[1])]))
        indices.append(used)
    common = indices[0] & indices[1]
    if not any(vertices[i] == (10, 10, 128) for i in common):
        raise RuntimeError('saved sectors do not share the off-grid target vertex')
    if any(vertices[i] == (0, 0, 128) for i in indices[0]):
        raise RuntimeError('source corner was not moved')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=45)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args(); args.width, args.height = 1280, 720
    args.binary = args.binary.resolve(); source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-sector-weld-smoke-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmap start') if args.validation else bench.BOOT
    (work / 'id1/boot.cfg').write_text(boot)
    (work / 'id1/maps/snapfixture.map').write_text(fixture())
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        opened = engine.cmd('disconnect\neditor_open snapfixture')
        if 'Sector map opened' not in opened: raise RuntimeError('fixture failed to open: ' + opened)
        os.write(engine.key, b'hvd')
        engine.serial += 1; marker = f'SNAP_CMD_{engine.serial}_DONE'; start = len(engine.text())
        (work / 'id1/control.tmp').write_text('editor_save snapfixture\neditor_open snapfixture\n' + 'echo ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c')
        engine.wait(lambda text: marker in text, start)
        output = engine.text()[start:]
        if 'Map saved' not in output or 'Sector map opened' not in output:
            raise RuntimeError('save/reopen failed:\n' + output)
        verify_weld(work / 'id1/maps/snapfixture.map')
        start = len(engine.text()); os.write(engine.key, b'f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        output = engine.text()[start:]
        if not all(s in output for s in ('Calculating visibility', 'Calculating lighting')):
            raise RuntimeError('playtest skipped VIS or LIGHT')
        results = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(results) != 1: raise RuntimeError('expected one compiled playtest BSP')
        bsp = results[0].read_bytes()
        if len(bsp) < 124 or bsp[:4] != b'BSP2': raise RuntimeError('invalid playtest BSP')
        for lump in (4, 8):
            offset, length = struct.unpack_from('<ii', bsp, 4 + 8 * lump)
            if offset < 0 or length <= 0 or offset + length > len(bsp): raise RuntimeError('missing playtest lump')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: off-grid 3D vertex snap and weld, save/reopen, and F9 playtest', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
