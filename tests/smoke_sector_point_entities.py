#!/usr/bin/env python3
"""Exercise lights and prop duplication in the actual in-engine editor.

Requires Vulkan, SDL headers and local Quake PAKs. All writable files are
created in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import tempfile

import bench_meshlets as bench
import smoke_sector_props_editor  # Sets up the real SDL prop-creation shim.
from editor_sector_intrusion_test import render_contents
from editor_sector_portal_compile_test import collision_reader

shim = bench.SHIM
old = "} else if (action == 'z') {\n            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_Z;"
assert old in shim
shim = shim.replace(old, "} else if (action == 'z' || action == 'w' || action == 'b') {\n"
                    "            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : "
                    "(action == 'z' ? SDL_SCANCODE_Z : action == 'w' ? SDL_SCANCODE_Y : SDL_SCANCODE_D);")
old = "sc = action == 'g' ? SDL_SCANCODE_G : action == 'o' ? SDL_SCANCODE_O :"
assert old in shim
shim = shim.replace(old, "sc = action == 'i' ? SDL_SCANCODE_H : action == 'J' ? SDL_SCANCODE_RIGHTBRACKET : " + old.removeprefix('sc = '))
old = "action == 'd' || action == 'm' || action == 'h' || action == 'j'"
assert old in shim
shim = shim.replace(old, old + " || action == 'x' || action == 'q' || action == 'p'")
old = "else if (action == 'h') { mx = cx + 128; my = cy - 96; }"
assert old in shim
shim = shim.replace(old, old + "\n            else if (action == 'x') { mx = cx - (step == 2 ? 96 : 128); my = cy - 160; }\n"
                         "            else if (action == 'q') { mx = 50; my = 100; }\n"
                         "            else if (action == 'p') { mx = cx - 160; my = cy - 128; }")
bench.SHIM = shim


def source_data(path):
    data = path.read_text()
    lines = iter(data.splitlines())
    assert next(lines) == 'QSECTOR 1'
    count = int(next(lines).split()[1])
    vertices = [tuple(map(float, next(lines).split())) for _ in range(count)]
    count = int(next(lines).split()[1]); sectors = []
    for _ in range(count):
        count = int(next(lines).split()[1]); used = set()
        for _ in range(count):
            fields = next(lines).split()
            assert fields[0] == 'FACE'
            used.update(map(int, fields[5:5 + int(fields[1])]))
        sectors.append(used)
    entities_match = re.search(r'(?m)^ENTITIES (\d+)\r?\n', data)
    assert entities_match
    entities_text = data[entities_match.end():]
    assert len(entities_text.encode()) == int(entities_match.group(1))
    entities = []
    for block in re.findall(r'\{([^{}]*)\}', entities_text, re.S):
        entities.append(dict(re.findall(r'"([^"\\]*)"\s+"([^"\\]*)"', block)))
    return data, vertices, sectors, entities


def vec(value):
    return tuple(map(float, value.split()))


def verify(path):
    data, vertices, sectors, entities = source_data(path)
    if len(sectors) != 3 or not re.search(r'\nSECTOR_PROPS\s+0\s+0\s+0\s+0\s+1\s+0\s+0\s+0\s+1\s+0\s+0\s+0\s', data):
        raise RuntimeError('saved map does not contain two static sector props')
    original, copied = sectors[1], sectors[2]
    if not original or original & copied:
        raise RuntimeError('duplicated prop shares source vertex IDs')
    first = {vertices[i] for i in original}; second = {vertices[i] for i in copied}
    if len(first) != len(second): raise RuntimeError('duplicated prop shape changed')
    possible = [tuple(b[i] - a[i] for i in range(3)) for a in first for b in second]
    offset = next((d for d in possible if any(d) and
                   {tuple(a[i] + d[i] for i in range(3)) for a in first} == second), None)
    if offset is None: raise RuntimeError('duplicated prop is not a translated copy')
    lights = [e for e in entities if e.get('classname') == 'light']
    if len(lights) != 5: raise RuntimeError(f'expected existing plus duplicated top and 3D lights: {lights}')
    created = [e for e in lights if e.get('light') == '450']
    if len(created) != 2: raise RuntimeError(f'expected two lights retaining edited intensity: {lights}')
    properties = [{k: v for k, v in e.items() if k != 'origin'} for e in created]
    if properties[0] != properties[1] or not properties[0].get('light'):
        raise RuntimeError(f'light duplication lost its editable properties: {created}')
    if vec(created[0]['origin']) == vec(created[1]['origin']):
        raise RuntimeError('light duplicate was not offset')
    for perspective in lights[-2:]:
        x, y, z = vec(perspective['origin'])
        if not (-256 < x < 256 and -256 < y < 256 and 0 < z < 256):
            raise RuntimeError(f'3D light or its duplicate is not inside the room: {perspective}')
    if vec(lights[-2]['origin']) == vec(lights[-1]['origin']):
        raise RuntimeError('3D light duplicate was not offset')


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
    work = Path(tempfile.mkdtemp(prefix='quiver-point-entities-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmap start') if args.validation else bench.BOOT
    (work / 'id1/boot.cfg').write_text(boot)
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)

    def gesture(keys, status):
        start = len(engine.text()); os.write(engine.key, keys)
        engine.wait(lambda text: status in text, start)

    def command(body):
        engine.serial += 1; marker = f'POINT_EDITOR_{engine.serial}'; start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c')
        return engine.wait(lambda text: text if marker in text else None, start)

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        if 'New 3D sector map' not in engine.cmd('disconnect\neditor_new'):
            raise RuntimeError('could not start sector editor')
        gesture(b'tgr', 'Sector prop created')
        gesture(b'b', 'Entity duplicated')
        # Place the first light in clear room space, away from the two props.
        os.write(engine.key, b'p')
        gesture(b'i', 'Light created')
        output = command('editor_light_intensity 450\neditor_return')
        if 'Light intensity updated' not in output: raise RuntimeError('light property update failed:\n' + output)
        gesture(b'J', 'Light moved')
        gesture(b'b', 'Entity duplicated')
        gesture(b'x', 'Light moved')
        gesture(b'J', 'Light moved')
        gesture(b'z', 'Undid change')
        gesture(b'w', 'Redid change')
        os.write(engine.key, b'4')
        gesture(b'i', 'Light created')
        gesture(b'b', 'Entity duplicated')
        # Clicking empty space clears the light selection; duplication must
        # not accidentally copy the previously selected point entity.
        os.write(engine.key, b'tq')
        gesture(b'b', 'Select a light or sector prop to duplicate')
        output = command('editor_save point_entities\neditor_open point_entities')
        if 'Map saved' not in output or 'Sector map opened' not in output:
            raise RuntimeError('save/reopen failed:\n' + output)
        verify(work / 'id1/maps/point_entities.map')
        start = len(engine.text()); os.write(engine.key, b'f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        output = engine.text()[start:]
        if not all(x in output for x in ('Calculating visibility', 'Calculating lighting')):
            raise RuntimeError('playtest skipped VIS or LIGHT')
        bsps = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(bsps) != 1 or bsps[0].read_bytes()[:4] != b'BSP2':
            raise RuntimeError('missing compiled BSP2')
        bsp = bsps[0].read_bytes(); offset, size = struct.unpack_from('<ii', bsp, 4 + 8 * 8)
        if offset < 0 or size <= 0 or offset + size > len(bsp) or not any(bsp[offset:offset + size]):
            raise RuntimeError('lightmap data was not compiled')
        entities = engine.cmd('edicts')
        if re.search(r'(?m)^classname\s+prop_static\s*$', entities):
            raise RuntimeError('static props should be baked into the world, not spawned:\n' + entities)
        _, vertices, sectors, _ = source_data(work / 'id1/maps/point_entities.map')
        render = render_contents(bsps[0]); player, _ = collision_reader(bsps[0])
        # Sample exclusive interiors on both sides of the overlapping source
        # boxes so one baked prop cannot accidentally satisfy both checks.
        for sector, fraction in ((sectors[1], .25), (sectors[2], .75)):
            bounds = [(min(vertices[i][a] for i in sector), max(vertices[i][a] for i in sector)) for a in range(3)]
            p = tuple(lo + (hi - lo) * (fraction if a < 2 else .5) for a, (lo, hi) in enumerate(bounds))
            if render(p) != -2 or player(p) != -2:
                raise RuntimeError(f'baked prop is missing from world rendering/collision at {p}')
        if render((0, 0, 128)) != -1:
            raise RuntimeError('room interior became solid after baking props')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: prop and point-light creation, duplication, edits, undo/redo, save/reopen and F9', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
