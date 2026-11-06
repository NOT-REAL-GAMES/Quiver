#!/usr/bin/env python3
"""Drag a coincident corner of two unlinked sectors through the real editor."""
import argparse
import os
from pathlib import Path
import re
import struct
import tempfile

import bench_meshlets as bench
import smoke_sector_vertex_snap as snap  # Installs the real mouse gesture shim.

# Move far enough to cross the halfway point of the 16-unit world grid. The
# reused weld fixture needed a shorter drag because it snapped to an off-grid
# target instead of the grid.
assert 'cy - (step == 2 ? 270 : 311)' in bench.SHIM
bench.SHIM = bench.SHIM.replace('cy - (step == 2 ? 270 : 311)', 'cy - (step == 2 ? 230 : 311)')


def fixture():
    boxes = (((-128, -128, 0), (0, 0, 128)), ((0, 0, 0), (128, 128, 128)))
    faces = ((0, 4, 6, 2), (1, 3, 7, 5), (0, 1, 5, 4),
             (2, 6, 7, 3), (0, 2, 3, 1), (4, 5, 7, 6))
    vertices = [tuple(hi[a] if corner & (1 << a) else lo[a] for a in range(3))
                for lo, hi in boxes for corner in range(8)]
    entities = ('{\n"classname" "worldspawn"\n}\n'
                '{\n"classname" "info_player_start"\n"origin" "-64 -64 32"\n}\n'
                '{\n"classname" "light"\n"origin" "-64 -64 96"\n"light" "400"\n}\n')
    result = 'QSECTOR 1\nVERTICES 16\n' + ''.join('{} {} {}\n'.format(*v) for v in vertices)
    result += 'SECTORS 2\n'
    for sector in range(2):
        result += 'SECTOR 6\n'
        result += ''.join('FACE 4 -1 -1 stone1 {} {} {} {}\n'.format(*(v + sector * 8 for v in face)) for face in faces)
    return result + f'ENTITIES {len(entities.encode())}\n' + entities


def check(path, expected, forbidden):
    lines = iter(path.read_text().splitlines()); assert next(lines) == 'QSECTOR 1'
    n = int(next(lines).split()[1]); vertices = [tuple(map(float, next(lines).split())) for _ in range(n)]
    assert next(lines) == 'SECTORS 2'
    for sector in range(2):
        nfaces = int(next(lines).split()[1]); used = set()
        for _ in range(nfaces):
            fields = next(lines).split(); assert fields[0] == 'FACE'
            assert int(fields[2]) == -1, 'test sectors should remain unlinked'
            used.update(map(int, fields[5:5 + int(fields[1])]))
        coords = {vertices[i] for i in used}
        if expected not in coords or forbidden in coords:
            raise RuntimeError(f'sector {sector} corner not updated: expected {expected}, forbidden {forbidden}')


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
    work = Path(tempfile.mkdtemp(prefix='quiver-coincident-vertices-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmap start') if args.validation else bench.BOOT
    (work / 'id1/boot.cfg').write_text(boot)
    (work / 'id1/maps/coincident.map').write_text(fixture())
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)
    def command(body):
        engine.serial += 1; marker = f'COINCIDENT_{engine.serial}'; start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c'); return engine.wait(lambda text: text if marker in text else None, start)
    def gesture(chars, status):
        start = len(engine.text()); os.write(engine.key, chars)
        engine.wait(lambda text: status in text, start)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        if 'Sector map opened' not in engine.cmd('disconnect\neditor_open coincident'):
            raise RuntimeError('could not open test fixture')
        gesture(b'hvd', 'Sector vertex moved')
        output = command('editor_save moved\neditor_return')
        if 'Map saved' not in output: raise RuntimeError('could not save movement')
        moved, old = (0., 0., 112.), (0., 0., 128.)
        check(work / 'id1/maps/moved.map', moved, old)
        gesture(b'z', 'Undid change')
        command('editor_save undone\neditor_return')
        check(work / 'id1/maps/undone.map', old, moved)
        output = command('editor_open moved force')
        if 'Sector map opened' not in output: raise RuntimeError('could not reopen movement')
        check(work / 'id1/maps/moved.map', moved, old)
        start = len(engine.text()); os.write(engine.key, b'f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        bsps = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(bsps) != 1 or bsps[0].read_bytes()[:4] != b'BSP2':
            raise RuntimeError('missing compiled BSP2 playtest')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: coincident unlinked vertices move together, undo, save/reopen, and F9', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
