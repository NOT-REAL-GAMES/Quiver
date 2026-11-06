#!/usr/bin/env python3
"""Exercise front/right editing in the running editor, then save and playtest."""
import argparse
import os
from pathlib import Path
import re
import tempfile

import bench_meshlets as bench
import smoke_sector_editor as smoke

shim = smoke.bench.SHIM
old = "} else if (action == 'z') {\n            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_Z;"
assert old in shim
shim = shim.replace(old, "} else if (action == 'z' || action == 'w') {\n"
                    "            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : "
                    "(action == 'z' ? SDL_SCANCODE_Z : SDL_SCANCODE_Y);")
old = "mx = cx + (step == 2 ? 256 : 512); my = cy + (step == 2 ? -128 : 128);"
assert old in shim
shim = shim.replace(old, "mx = cx + (step == 2 ? 80 : 112); my = cy - (step == 2 ? 32 : 64);")
old = "action == 'd' || action == 'm' || action == 'h' || action == 'j'"
assert old in shim
shim = shim.replace(old, old + " || action == 'p' || action == 'b'")
old = "if (action == 'd') { mx = cx; my = cy - (step == 2 ? 48 : 0); }"
assert old in shim
shim = shim.replace(old, "if (action == 'd') { mx = cx + (step == 2 ? 16 : 0); my = cy - (step == 2 ? 64 : 48); }")
old = "else { mx = cx + (action == 'j' ? 2 : 0); my = cy + (action == 'j' ? 2 : 0); }"
assert old in shim
shim = shim.replace(old, "else if (action == 'b') { mx = width - 117; my = 12; }\n"
                    "            else if (action == 'p') { mx = cx - 96; my = cy - 112; }\n"
                    "            else if (action == 'h') { mx = cx - (step == 2 ? 64 : 96); my = cy - (step == 2 ? 128 : 112); }\n"
                    "            else if (action == 'j') { mx = cx + (step == 2 ? 32 : 16); my = cy - (step == 2 ? 112 : 128); }\n"
                    "            else { mx = cx; my = cy; }")
old = "sc = action == 'a' ? SDL_SCANCODE_F : action == 'v' ? SDL_SCANCODE_V :"
assert old in shim
shim = shim.replace(old, "sc = action == '2' ? SDL_SCANCODE_2 : action == '3' ? SDL_SCANCODE_3 : "
                    "action == '4' ? SDL_SCANCODE_4 : action == 'g' ? SDL_SCANCODE_G : "
                    "action == 'i' ? SDL_SCANCODE_H : action == 'q' ? SDL_SCANCODE_RIGHTBRACKET : "
                    "action == 'a' ? SDL_SCANCODE_F : action == 'v' ? SDL_SCANCODE_V :")
bench.SHIM = shim


def inspect(path):
    data = path.read_text(); lines = iter(data.splitlines())
    assert next(lines) == 'QSECTOR 1'
    n = int(next(lines).split()[1]); vertices = [tuple(map(float, next(lines).split())) for _ in range(n)]
    n = int(next(lines).split()[1]); assert n == 2
    sectors = []
    for _ in range(n):
        nfaces = int(next(lines).split()[1]); used = set()
        for _ in range(nfaces):
            fields = next(lines).split(); assert fields[0] == 'FACE'
            used.update(map(int, fields[5:5 + int(fields[1])]))
        sectors.append(used)
    if not re.search(r'\nSECTOR_PROPS\s+0\s+0\s+0\s+0\s+1\s+0\s+0\s+0\s', data):
        raise RuntimeError('front-created object is not a static sector prop')
    bounds = [(min(vertices[i][axis] for i in sectors[1]), max(vertices[i][axis] for i in sectors[1])) for axis in range(3)]
    expected = [(80.,112.), (-48.,80.), (80.,112.)]
    if bounds != expected: raise RuntimeError(f'prop move in right view changed wrong axes: {bounds}, expected {expected}')
    tail = re.search(r'(?m)^ENTITIES (\d+)\r?\n', data); assert tail
    entities = [dict(re.findall(r'"([^"\\]*)"\s+"([^"\\]*)"', block)) for block in re.findall(r'\{([^{}]*)\}', data[tail.end():], re.S)]
    lights = [e for e in entities if e.get('classname') == 'light']
    if len(lights) != 2: raise RuntimeError(f'expected one created light: {lights}')
    position = tuple(map(float, lights[-1]['origin'].split()))
    if position != (32., -48., 144.):
        raise RuntimeError(f'point edits used the wrong world axes: {position}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True); p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=30); p.add_argument('--validation', action='store_true')
    args = p.parse_args(); args.width, args.height = 1280, 720; args.binary = args.binary.resolve()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(q for q in source.iterdir() if re.fullmatch(r'pak\d+\.pak', q.name.lower()))
    if not paks: p.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-orthographic-views-')); print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmap start') if args.validation else bench.BOOT
    (work / 'id1/boot.cfg').write_text(boot)
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)
    def gesture(keys, status):
        start = len(engine.text()); os.write(engine.key, keys); engine.wait(lambda text: status in text, start)
    def command(body):
        engine.serial += 1; marker = f'ORTHO_{engine.serial}'; start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c'); return engine.wait(lambda text: text if marker in text else None, start)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        if 'New 3D sector map' not in engine.cmd('disconnect\neditor_new'):
            raise RuntimeError('editor_new failed')
        # Enter FRONT via its clickable toolbar button; use keyboard
        # shortcuts for RIGHT, FRONT and 3D later in the same run.
        gesture(b'bgr', 'Sector prop created')
        gesture(b'3d', 'Sector moved')
        os.write(engine.key, b'p')
        gesture(b'i', 'Light created')
        gesture(b'q', 'Light moved')
        gesture(b'h', 'Light moved')
        gesture(b'2j', 'Light moved')
        gesture(b'q', 'Light moved')
        gesture(b'z', 'Undid change')
        gesture(b'w', 'Redid change')
        output = command('editor_save ortho_views\neditor_open ortho_views')
        if not all(s in output for s in ('Map saved', 'Sector map opened')):
            raise RuntimeError('save/reopen failed:\n' + output)
        inspect(work / 'id1/maps/ortho_views.map')
        start = len(engine.text()); os.write(engine.key, b'4f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        bsps = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(bsps) != 1 or bsps[0].read_bytes()[:4] != b'BSP2': raise RuntimeError('missing BSP2 playtest')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: front/right sector and light editing, save/reopen and F9 playtest', flush=True)
    finally:
        if engine.process.poll() is None: engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()

if __name__ == '__main__': main()
