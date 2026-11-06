#!/usr/bin/env python3
"""Create and convert a prop via the real editor, then save and playtest it."""
import argparse
import os
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench
import smoke_sector_editor as smoke
from smoke_sector_props_runtime import origin


shim = smoke.bench.SHIM
old = "mx = cx + (step == 2 ? 256 : 512); my = cy + (step == 2 ? -128 : 128);"
assert old in shim
shim = shim.replace(old, "mx = cx + (action == 's' ? (step == 2 ? 96 : 128) : (step == 2 ? 96 : 160)); "
                         "my = cy + (action == 's' ? (step == 2 ? 16 : -16) : (step == 2 ? -64 : -128));")
old = "} else if (action == 'r') {"
assert old in shim
shim = shim.replace(old, "} else if (action == 'r' || action == 's') {")
old = "else { mx = cx + (action == 'j' ? 2 : 0); my = cy + (action == 'j' ? 2 : 0); }"
assert old in shim
shim = shim.replace(old, "else if (action == 'h') { mx = cx + 128; my = cy - 96; }\n            " + old)
old = "sc = action == 'a' ? SDL_SCANCODE_F : action == 'v' ? SDL_SCANCODE_V :"
assert old in shim
shim = shim.replace(old, "sc = action == 'g' ? SDL_SCANCODE_G : action == 'o' ? SDL_SCANCODE_O : action == 'a' ? SDL_SCANCODE_F : action == 'v' ? SDL_SCANCODE_V :")
bench.SHIM = shim


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
    work = Path(tempfile.mkdtemp(prefix='quiver-sector-props-editor-'))
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
        engine.serial += 1; marker = f'PROP_EDITOR_{engine.serial}'; start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c')
        return engine.wait(lambda text: text if marker in text else None, start)

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        if 'New 3D sector map' not in engine.cmd('disconnect\neditor_new'):
            raise RuntimeError('could not start sector editor')
        # Switch to top view, select static-prop creation, and draw a box
        # entirely inside the initial sealed room; explicitly reselect it.
        gesture(b'tgr', 'Sector prop created')
        gesture(b'ho', 'Sector type changed')
        saved = command('editor_prop_velocity 24 0 0\neditor_save prop_editor\neditor_open prop_editor')
        if not all(s in saved for s in ('Prop velocity updated', 'Map saved', 'Sector map opened')):
            raise RuntimeError('velocity/save/reopen failed:\n' + saved)
        source_text = (work / 'id1/maps/prop_editor.map').read_text()
        if '\nSECTORS 2\n' not in source_text or not re.search(r'\nSECTOR_PROPS\s+0\s+0\s+0\s+0\s+2\s+24\s+0\s+0\s', source_text):
            raise RuntimeError('saved source does not contain the dynamic prop and velocity')
        start = len(engine.text()); os.write(engine.key, b'f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        entities = engine.cmd('edicts')
        match = re.search(r'EDICT\s+(\d+):\s*(.*?)(?=\n\s*EDICT\s+\d+:|\nFree-list:|\Z)', entities, re.S)
        for candidate in re.finditer(r'EDICT\s+(\d+):\s*(.*?)(?=\n\s*EDICT\s+\d+:|\nFree-list:|\Z)', entities, re.S):
            if re.search(r'(?m)^classname\s+prop_dynamic\s*$', candidate[2]): match = candidate; break
        else: raise RuntimeError('dynamic prop did not spawn:\n' + entities)
        detail = engine.cmd(f'edict {match[1]}')
        if 'SOLID_BSP' not in detail or 'MOVETYPE_PUSH' not in detail or not re.search(r'model\s+\*\d+', detail):
            raise RuntimeError('dynamic prop did not spawn as a solid moving brush model:\n' + detail)
        dynamic_before = origin(detail)[0]
        engine.cmd('host_maxfps 72'); time.sleep(0.5)
        dynamic_after = origin(engine.cmd(f'edict {match[1]}'))[0]
        if dynamic_after <= dynamic_before + 2:
            raise RuntimeError(f'editor-created dynamic prop did not move: {dynamic_before} -> {dynamic_after}')

        # Start a fresh room. Creation mode remains static, so cycle twice to
        # movable and draw a box directly in the player's path.
        if 'New 3D sector map' not in engine.cmd('editor_new force'):
            raise RuntimeError('could not start second sector map')
        gesture(b'ggs', 'Sector prop created')
        saved = command('editor_save prop_movable_editor\neditor_open prop_movable_editor')
        if 'Map saved' not in saved or 'Sector map opened' not in saved:
            raise RuntimeError('movable prop save/reopen failed:\n' + saved)
        source_text = (work / 'id1/maps/prop_movable_editor.map').read_text()
        if not re.search(r'\nSECTOR_PROPS\s+0\s+0\s+0\s+0\s+3\s+0\s+0\s+0\s', source_text):
            raise RuntimeError('saved source does not contain the movable prop')
        start = len(engine.text()); os.write(engine.key, b'f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        entities = engine.cmd('edicts')
        for candidate in re.finditer(r'EDICT\s+(\d+):\s*(.*?)(?=\n\s*EDICT\s+\d+:|\nFree-list:|\Z)', entities, re.S):
            if re.search(r'(?m)^classname\s+prop_movable\s*$', candidate[2]): match = candidate; break
        else: raise RuntimeError('movable prop did not spawn:\n' + entities)
        def prop(): return engine.cmd(f'edict {match[1]}')
        detail = prop()
        if 'SOLID_BBOX' not in detail or 'MOVETYPE_TOSS' not in detail:
            raise RuntimeError('movable prop did not spawn with pushable collision:\n' + detail)
        before = origin(detail)[0]
        engine.cmd('host_maxfps 72\n+forward'); time.sleep(3.0); engine.cmd('-forward')
        after = origin(prop())[0]
        if after <= before + 1 or after > 240.1:
            raise RuntimeError(f'editor-created prop did not move or crossed the wall: {before} -> {after}')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: editor prop creation/conversion, velocity, save/reopen, dynamic motion, movable push and wall collision', flush=True)
    finally:
        if engine.process.poll() is None: engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
