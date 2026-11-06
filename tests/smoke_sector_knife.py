#!/usr/bin/env python3
"""Exercise a sector knife cut, save/reopen and F9 playtest.

Requires a Vulkan display, SDL development headers and local Quake PAK data.
All writable game data is isolated in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import tempfile

import bench_meshlets as bench
import smoke_sector_editor as smoke


# Drive the actual viewport: two clicks close to opposite edges of the wall
# in front of the camera. Each click is inside the face, not on its vertices.
shim = smoke.bench.SHIM
old = "action == 'd' || action == 'm' || action == 'h' || action == 'j'"
assert old in shim
shim = shim.replace(old, old + " || action == 'p' || action == 'q' || action == 's' || action == 'g'")
old = "else if (action == 'm') { mx = cx - 416 - (step == 2 ? 32 : 0); my = cy - 286; }"
assert old in shim
shim = shim.replace(old, old + "\n            else if (action == 'p' || action == 'q') { mx = cx + (action == 'p' ? -380 : 380); my = cy; }\n            else if (action == 's') { mx = cx; my = cy + 160; }\n            else if (action == 'g') { mx = cx + 380; my = cy - 100; }\n            else if (action == 'h') { mx = cx; my = cy - 8; }")
old = "sc = action == 'a' ? SDL_SCANCODE_F : action == 'v' ? SDL_SCANCODE_V :"
assert old in shim
shim = shim.replace(old, "sc = action == 'i' ? SDL_SCANCODE_K : action == 'b' ? SDL_SCANCODE_T : action == 'a' ? SDL_SCANCODE_F : action == 'v' ? SDL_SCANCODE_V :")
old = "} else if (action == 'z') {\n            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_Z;"
assert old in shim
shim = shim.replace(old, "} else if (action == 'z' || action == 'w') {\n            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : (action == 'z' ? SDL_SCANCODE_Z : SDL_SCANCODE_Y);")
bench.SHIM = shim


def fixture():
    mins, maxs = (-128, -128, 0), (0, 0, 128)
    vertices = [tuple(maxs[a] if corner & (1 << a) else mins[a] for a in range(3)) for corner in range(8)]
    faces = ((0, 4, 6, 2), (1, 3, 7, 5), (0, 1, 5, 4),
             (2, 6, 7, 3), (0, 2, 3, 1), (4, 5, 7, 6))
    entities = ('{\n"classname" "worldspawn"\n}\n'
                '{\n"classname" "info_player_start"\n"origin" "-64 -64 32"\n}\n'
                '{\n"classname" "light"\n"origin" "-64 -64 96"\n"light" "400"\n}\n')
    result = 'QSECTOR 1\nVERTICES 8\n'
    result += ''.join('{} {} {}\n'.format(*vertex) for vertex in vertices)
    result += 'SECTORS 1\nSECTOR 6\n'
    result += ''.join('FACE 4 -1 -1 stone1 {} {} {} {}\n'.format(*face) for face in faces)
    return result + f'ENTITIES {len(entities.encode())}\n' + entities


def read_geometry(path):
    lines = iter(path.read_text().splitlines())
    assert next(lines) == 'QSECTOR 1'
    count = int(next(lines).split()[1])
    vertices = [tuple(map(float, next(lines).split())) for _ in range(count)]
    assert next(lines) == 'SECTORS 1'
    face_count = int(next(lines).split()[1]); faces = []
    for _ in range(face_count):
        fields = next(lines).split()
        assert fields[0] == 'FACE'
        faces.append(tuple(map(int, fields[5:5 + int(fields[1])])))
    return vertices, faces


def verify_cut(path):
    vertices, faces = read_geometry(path)
    if len(vertices) != 10 or len(faces) != 7:
        raise RuntimeError(f'expected a two-edge knife cut: {len(vertices)} vertices, {len(faces)} faces')
    for point in ((0, -128, 80), (0, 0, 80)):
        if point not in vertices:
            raise RuntimeError(f'knife endpoint {point} missing: {vertices}')
        idx = vertices.index(point)
        if sum(idx in face for face in faces) != 3:
            raise RuntimeError(f'knife endpoint {point} not shared by cut faces and adjacent wall')


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
    work = Path(tempfile.mkdtemp(prefix='quiver-sector-knife-smoke-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmap start') if args.validation else bench.BOOT
    (work / 'id1/boot.cfg').write_text(boot)
    (work / 'id1/maps/knifefixture.map').write_text(fixture())
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)
    def editor_command(body):
        engine.serial += 1; marker = f'KNIFE_CMD_{engine.serial}_DONE'; start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c'); engine.wait(lambda text: marker in text, start)
        return engine.text()[start:]
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        opened = engine.cmd('disconnect\neditor_texture cut_side\neditor_open knifefixture')
        if 'Sector map opened' not in opened: raise RuntimeError('fixture failed to open: ' + opened)
        start = len(engine.text()); os.write(engine.key, b'ipq')
        engine.wait(lambda text: 'Sector face cut' in text, start)
        start = len(engine.text()); os.write(engine.key, b'z')
        engine.wait(lambda text: 'Undid change' in text, start)
        start = len(engine.text()); os.write(engine.key, b'w')
        engine.wait(lambda text: 'Redid change' in text, start)
        saved = editor_command('editor_save knifefixture\neditor_open knifefixture')
        if 'Map saved' not in saved or 'Sector map opened' not in saved: raise RuntimeError('save/reopen failed: ' + saved)
        verify_cut(work / 'id1/maps/knifefixture.map')
        # A new cut can snap to the line created by the first, splitting its
        # edge consistently in both neighboring faces.
        start = len(engine.text()); os.write(engine.key, b'igh')
        engine.wait(lambda text: 'Sector face cut' in text, start)
        editor_command('editor_save knifeline\neditor_open knifefixture')
        vertices, faces = read_geometry(work / 'id1/maps/knifeline.map')
        point = (0, -64, 80)
        if len(vertices) != 12 or len(faces) != 8 or point not in vertices or sum(vertices.index(point) in face for face in faces) != 3:
            raise RuntimeError('second knife cut did not share a vertex on the existing cut line')
        start = len(engine.text()); os.write(engine.key, b'asb')
        engine.wait(lambda text: 'Sector material changed' in text, start)
        editor_command('editor_save knifematerial\neditor_open knifefixture')
        material_source = (work / 'id1/maps/knifematerial.map').read_text()
        if len(re.findall(r'^FACE \d+ -1 -1 cut_side ', material_source, re.M)) != 1:
            raise RuntimeError('cut faces were not independently selectable/material-editable')
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
        print('PASS: knife cut with shared edge vertices, undo/redo, independent face material, save/reopen, and F9 playtest', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
