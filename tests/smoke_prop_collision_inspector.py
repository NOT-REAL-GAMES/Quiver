#!/usr/bin/env python3
"""Exercise the static-prop inspector and compiled player collision.

Requires Vulkan, SDL headers, and local Quake PAKs. Runs in an isolated temporary
basedir. Supply an absolute or relative path to the engine and Quake data:
  python3 tests/smoke_prop_collision_inspector.py --binary build/vkquake --basedir /path/to/quake
"""
import argparse
import os
from pathlib import Path
import re
import tempfile
import time

import bench_meshlets as bench
import smoke_sector_props_editor  # Installs the real SDL editor-input shim.
from smoke_sector_props_runtime import origin

# Add a click on the prop directly ahead of the player and on the inspector,
# plus Ctrl+Y / Ctrl+D. Each action is injected through the normal SDL path.
shim = bench.SHIM
old = "} else if (action == 'z') {\n            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_Z;"
assert old in shim
shim = shim.replace(old, "} else if (action == 'z' || action == 'w' || action == 'b') {\n"
                    "            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : "
                    "(action == 'z' ? SDL_SCANCODE_Z : action == 'w' ? SDL_SCANCODE_Y : SDL_SCANCODE_D);")
old = "action == 'd' || action == 'm' || action == 'h' || action == 'j'"
assert old in shim
shim = shim.replace(old, old + " || action == 'p' || action == 'i'")
old = "else if (action == 'h') { mx = cx + 128; my = cy - 96; }"
assert old in shim
shim = shim.replace(old, old + "\n            else if (action == 'p') { mx = cx + 112; my = cy; }\n"
                         "            else if (action == 'i') { mx = width - 110; my = 105; }")
bench.SHIM = shim


def collision_flags(path):
    data = path.read_text()
    count = re.search(r'(?m)^SECTORS (\d+)$', data)
    section = re.search(r'(?ms)^SECTOR_COLLISION\s+(.*?)^ENTITIES\s+', data)
    if not count:
        raise RuntimeError('source file is missing its sector count')
    if not section:
        # Files without the optional section use the backwards-compatible default.
        return [1] * int(count.group(1))
    flags = [int(v) for v in section.group(1).split()]
    if len(flags) != int(count.group(1)):
        raise RuntimeError(f'invalid collision section: {flags}')
    return flags


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=45)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--capture-inspector', action='store_true', help='save a screenshot of the inspector in the temporary test directory')
    args = parser.parse_args()
    args.width, args.height = 1280, 720
    args.binary = args.binary.resolve()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-prop-collision-inspector-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True)
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    options = 'max_edicts 32000\n' + ('r_rtshadows 0\n' if args.validation else '')
    boot = bench.BOOT.replace('map start', options + 'map start')
    (work / 'id1/boot.cfg').write_text(boot)
    os.environ['SMOKE_WIDTH'] = str(args.width)
    os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)

    def gesture(keys, status):
        start = len(engine.text())
        os.write(engine.key, keys)
        engine.wait(lambda text: status in text, start)

    def command(body):
        engine.serial += 1
        marker = f'INSPECTOR_{engine.serial}'
        start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c')
        return engine.wait(lambda text: text if marker in text else None, start)

    def save_expect(name, expected):
        output = command('editor_save ' + name + '\neditor_return')
        if 'Map saved' not in output:
            raise RuntimeError('could not save source:\n' + output)
        flags = collision_flags(work / 'id1/maps' / (name + '.map'))
        if flags != expected:
            raise RuntimeError(f'{name}: collision flags {flags}, expected {expected}')

    def play_and_walk():
        start = len(engine.text())
        os.write(engine.key, b'f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        engine.cmd('host_maxfps 72')
        time.sleep(.3)
        before = origin(engine.cmd('edict 1'))
        engine.cmd('+forward')
        time.sleep(2)
        engine.cmd('-forward')
        time.sleep(.2)
        after = origin(engine.cmd('edict 1'))
        return before, after

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        if 'New 3D sector map' not in engine.cmd('disconnect\neditor_new'):
            raise RuntimeError('could not start sector editor')
        # Room, then a static box at x=96..128 directly in the spawn's path.
        gesture(b'tgs', 'Sector prop created')
        save_expect('collision_default', [1, 1])
        before, blocked = play_and_walk()
        if not (blocked[0] > before[0] + 5 and blocked[0] < 95):
            raise RuntimeError(f'default solid prop did not block player: {before} -> {blocked}')
        if 'Editing source map' not in engine.cmd('editor_return'):
            raise RuntimeError('could not return to editor')
        # Select the prop and click its checkbox, then verify the source rather
        # than relying solely on the inspector's displayed value.
        os.write(engine.key, b'pi')
        save_expect('collision_off', [1, 0])
        if args.capture_inspector:
            start = len(engine.text())
            command('editor_return\nwait\nwait\nscreenshot png')
            match = engine.wait(lambda text: re.search(r'Wrote [\x01\x02]?([^\n]+\.png)', text), start)
            print(f'inspector screenshot: {match.group(1)} under {work}', flush=True)
        gesture(b'z', 'Undid change')
        save_expect('collision_undo', [1, 1])
        gesture(b'w', 'Redid change')
        save_expect('collision_redo', [1, 0])
        # Undo/redo clears the selection, so explicitly pick the prop again.
        os.write(engine.key, b'p')
        gesture(b'b', 'Entity duplicated')
        save_expect('collision_duplicate', [1, 0, 0])
        gesture(b'z', 'Undid change')
        save_expect('collision_duplicate_undo', [1, 0])
        gesture(b'w', 'Redid change')
        save_expect('collision_duplicate_redo', [1, 0, 0])
        output = command('editor_open collision_duplicate_redo')
        if 'Sector map opened' not in output:
            raise RuntimeError('could not reopen saved map:\n' + output)
        save_expect('collision_reopened', [1, 0, 0])
        before, passed = play_and_walk()
        if passed[0] <= blocked[0] + 30:
            raise RuntimeError(f'player did not pass noncolliding prop: {before} -> {passed}; solid stopped at {blocked}')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print(f'PASS: inspector, undo/redo, duplicate, save/reopen, and compiled player collision '
              f'(solid x={blocked[0]:.1f}; nonsolid x={passed[0]:.1f})', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate()
            engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__':
    main()
