#!/usr/bin/env python3
"""Linux in-engine sector editing and playtest smoke using injected SDL input.

Requires a Vulkan display, SDL development headers, and local Quake PAK data.
All writable game data lives in a disposable temporary directory.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

import bench_meshlets as bench
from PIL import Image, ImageStat


# Each byte names an input gesture. Mouse state changes before each injected
# button event, so the editor's own viewport picking and dragging are tested.
bench.SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef USE_SDL3
#include <SDL3/SDL.h>
typedef bool poll_result;
#define KDOWN SDL_EVENT_KEY_DOWN
#define KUP SDL_EVENT_KEY_UP
#define MDOWN SDL_EVENT_MOUSE_BUTTON_DOWN
#define MUP SDL_EVENT_MOUSE_BUTTON_UP
static float mx, my;
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) {
    if (x) *x = mx; if (y) *y = my; return 0;
}
#else
#include <SDL2/SDL.h>
typedef int poll_result;
#define KDOWN SDL_KEYDOWN
#define KUP SDL_KEYUP
#define MDOWN SDL_MOUSEBUTTONDOWN
#define MUP SDL_MOUSEBUTTONUP
static int mx, my;
Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = mx; if (y) *y = my; return 0;
}
#endif
poll_result SDL_PollEvent(SDL_Event *event) {
    static poll_result (*real_poll)(SDL_Event *);
    static int fd = -1, step = 0, action = 0, initialized = 0;
    if (!real_poll) { real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent"); fd = atoi(getenv("BENCH_KEY_FD")); }
    int width = atoi(getenv("SMOKE_WIDTH")), height = atoi(getenv("SMOKE_HEIGHT"));
    int cx = width / 2, cy = (height + 16) / 2;
    if (!initialized) { mx = cx; my = cy; initialized = 1; }
    char signal;
    if (!step && read(fd, &signal, 1) == 1) { action = signal; step = 1; }
    if (event && step) {
        int sc = 0, down = 0, mouse = 0, end = 2;
        if (action == 'c') {
            end = 6;
            sc = step == 1 || step == 4 ? SDL_SCANCODE_LSHIFT :
                 step == 2 || step == 3 ? SDL_SCANCODE_ESCAPE : SDL_SCANCODE_F12;
            down = step == 1 || step == 2 || step == 5;
        } else if (action == 'z') {
            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_Z;
            down = step <= 2;
        } else if (action == 'n') {
            end = 4; sc = SDL_SCANCODE_LCTRL;
            if (step == 2 || step == 3) { mouse = 1; mx = cx; my = cy; }
            down = step <= 2;
        } else if (action == 'r') {
            end = 4; sc = SDL_SCANCODE_LSHIFT;
            if (step == 2 || step == 3) {
                mouse = 1; mx = cx + (step == 2 ? 256 : 512); my = cy + (step == 2 ? -128 : 128);
            }
            down = step <= 2;
        } else if (action == 'd' || action == 'm' || action == 'h' || action == 'j') {
            mouse = 1; down = step == 1;
            if (action == 'd') { mx = cx; my = cy - (step == 2 ? 48 : 0); }
            else if (action == 'm') { mx = cx - 416 - (step == 2 ? 32 : 0); my = cy - 286; }
            else { mx = cx + (action == 'j' ? 2 : 0); my = cy + (action == 'j' ? 2 : 0); }
        } else {
            sc = action == 'a' ? SDL_SCANCODE_F : action == 'v' ? SDL_SCANCODE_V :
                 action == 't' ? SDL_SCANCODE_TAB : action == '4' ? SDL_SCANCODE_4 : action == 'l' ? SDL_SCANCODE_L :
                 action == 'f' ? SDL_SCANCODE_F9 : SDL_SCANCODE_F12;
            down = step == 1;
        }
        if (++step > end) step = 0;
        SDL_zero(*event);
        if (mouse) {
            event->type = down ? MDOWN : MUP; event->button.button = SDL_BUTTON_LEFT;
#ifdef USE_SDL3
            event->button.down = down; event->button.x = mx; event->button.y = my;
#else
            event->button.state = down ? SDL_PRESSED : SDL_RELEASED; event->button.x = mx; event->button.y = my;
#endif
        } else {
            event->type = down ? KDOWN : KUP;
#ifdef USE_SDL3
            event->key.scancode = sc; event->key.key = SDL_GetKeyFromScancode(sc, 0, 0); event->key.down = down;
#else
            event->key.keysym.scancode = sc; event->key.keysym.sym = SDL_GetKeyFromScancode(sc); event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
#endif
        }
        return 1;
    }
    return real_poll(event);
}
'''


def far_sector_source():
    """A standalone, normally serialized box sector at x=100000."""
    mins, maxs = (100000, 0, 0), (100128, 128, 128)
    vertices = [tuple(maxs[axis] if corner & (1 << axis) else mins[axis] for axis in range(3))
                for corner in range(8)]
    faces = ((0, 4, 6, 2), (1, 3, 7, 5), (0, 1, 5, 4),
             (2, 6, 7, 3), (0, 2, 3, 1), (4, 5, 7, 6))
    entities = ('{\n"classname" "worldspawn"\n}\n'
                '{\n"classname" "info_player_start"\n"origin" "100064 64 32"\n}\n'
                '{\n"classname" "light"\n"origin" "100064 64 80"\n"light" "400"\n}\n')
    result = 'QSECTOR 1\nVERTICES 8\n'
    result += ''.join('{} {} {}\n'.format(*vertex) for vertex in vertices)
    result += 'SECTORS 1\nSECTOR 6\n'
    result += ''.join('FACE 4 -1 -1 stone1 {} {} {} {}\n'.format(*face) for face in faces)
    result += f'ENTITIES {len(entities.encode("utf-8"))}\n' + entities
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=45)
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--verify-selection', action='store_true',
                        help='capture X11 editor window and verify both overlapping faces are highlighted')
    args = parser.parse_args()
    if args.verify_selection:
        if not all(shutil.which(command) for command in ('xdotool', 'xwd', 'ffmpeg')):
            parser.error('--verify-selection requires xdotool, xwd and ffmpeg')
        os.environ['SDL_VIDEODRIVER'] = 'x11'
    if args.width < 1154 or args.height < 620:
        parser.error('this fixture needs at least 1154x620 for its editor gestures')
    args.binary = args.binary.resolve()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-sector-ui-smoke-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1').mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    boot = bench.BOOT
    if args.validation:
        # Ray-query acceleration structures exercise unrelated validation paths.
        boot = boot.replace('map start', 'r_rtshadows 0\nmap start')
    (work / 'id1/boot.cfg').write_text(boot)
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)

    def gesture(chars, expected=None):
        start = len(engine.text())
        os.write(engine.key, chars.encode('ascii'))
        if expected: engine.wait(lambda text: expected in text, start)
        return start

    def editor_command(body):
        engine.serial += 1
        marker = f'SECTOR_CMD_{engine.serial}_DONE'
        start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c')
        engine.wait(lambda text: marker in text, start)
        return start

    def selection_image(name):
        # Read the editor window directly. Invoking the in-game screenshot
        # command would open the console and legitimately reset the click cycle.
        time.sleep(.3)
        ids = subprocess.check_output(['xdotool', 'search', '--pid', str(engine.process.pid), '--name', 'vkQuake'], text=True).split()
        if not ids: raise RuntimeError('could not find the X11 editor window')
        raw, png = work / (name + '.xwd'), work / (name + '.png')
        subprocess.run(['xwd', '-silent', '-id', ids[-1], '-out', str(raw)], check=True)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(raw), '-frames:v', '1', str(png)], check=True)
        with Image.open(png) as image:
            return image.convert('RGB')

    def is_highlight(image, x, y):
        # Palette 111 is bright yellow; sample a small area so an edge line
        # crossing one pixel doesn't make the assertion fragile.
        points = (image.getpixel((x + dx, y + dy)) for dx in range(-2, 3) for dy in range(-2, 3))
        return sum(r > 200 and g > 200 and b < 130 for r, g, b in points) >= 15

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        initial = engine.cmd('disconnect\neditor_new')
        if 'New 3D sector map' not in initial: raise RuntimeError('sector editor did not open')

        # Move one 3D face and one 3D vertex through mouse drags, undoing each
        # to keep the initial room unchanged for the portal test.
        gesture('ad', 'Sector face moved')
        gesture('z', 'Undid change')
        gesture('vm', 'Sector vertex moved')
        gesture('z', 'Undid change')

        # Create an adjacent smaller sector in the top view. Its near face is
        # coplanar with a subset of the starter wall, exercising partial links.
        gesture('vtr', 'Sector created')
        # Select the opposite, coplanar face with an ordinary second click
        # slightly offset from the first. No Ctrl modifier is held.
        if args.verify_selection:
            gesture('4ah')
            first = selection_image('selected-first-face')
            gesture('lj')
            second = selection_image('selected-second-face')
            center = (args.width // 2, (args.height + 16) // 2)
            outer = (center[0] - 300, center[1] - 150)
            if not is_highlight(first, *center) or not is_highlight(second, *center):
                raise RuntimeError('selected coincident face is not visibly highlighted at the click point')
            if not is_highlight(first, *outer) or is_highlight(second, *outer):
                raise RuntimeError('repeated click did not visibly switch to the smaller opposing face')
            gesture('l', 'Sector portal linked')
        else:
            gesture('4ahljl', 'Sector portal linked')

        # Save then reopen to verify that symmetric portal data survives the
        # normal source document workflow before using F9.
        saved = editor_command('editor_save sector_smoke\neditor_open sector_smoke')
        if 'Map saved' not in engine.text()[saved:] or 'Sector map opened' not in engine.text()[saved:]:
            raise RuntimeError('sector save or reopen failed')
        source_text = (work / 'id1/maps/sector_smoke.map').read_text()
        if not source_text.startswith('QSECTOR 1\n') or '\nSECTORS 2\n' not in source_text:
            raise RuntimeError('saved source is not the expected two-sector document')
        if len(re.findall(r'^FACE \d+ (?!-1\b)\d+ \d+ ', source_text, re.M)) < 2:
            raise RuntimeError('saved source did not retain paired portal faces')

        start = gesture('f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        output = engine.text()[start:]
        if not all(x in output for x in ('Calculating visibility', 'Calculating lighting')):
            raise RuntimeError('sector playtest skipped VIS or LIGHT')
        results = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(results) != 1: raise RuntimeError('expected one compiled playtest BSP')
        bsp = results[0].read_bytes()
        if len(bsp) < 124 or bsp[:4] != b'BSP2':
            raise RuntimeError('invalid compiled BSP')
        for lump in (4, 8):
            offset, length = struct.unpack_from('<ii', bsp, 4 + 8 * lump)
            if offset < 0 or length <= 0 or offset + length > len(bsp):
                raise RuntimeError('sector playtest missing visibility or lighting')

        # Open an independently authored sector far beyond the classic 16-bit
        # BSP bounds, then verify real client and server position after F9.
        far_source = work / 'id1/maps/far_sector.map'
        far_source.write_text(far_sector_source())
        opened = engine.cmd('editor_open far_sector')
        if 'Sector map opened' not in opened:
            raise RuntimeError('could not open far-coordinate sector: ' + opened)
        old_results = set(results)
        far_start = gesture('f')
        result = engine.wait(lambda text: 'Starting playtest' if 'Starting playtest' in text else
                             'failed' if re.search(r'Editor: .*failed|Editor: .*unavailable', text, re.I) else None,
                             far_start)
        if result != 'Starting playtest':
            raise RuntimeError('far-coordinate sector playtest failed:\n' + engine.text()[far_start:])
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, far_start)
        far_output = engine.text()[far_start:]
        if not all(x in far_output for x in ('Calculating visibility', 'Calculating lighting')):
            raise RuntimeError('far-coordinate sector playtest skipped VIS or LIGHT')
        new_results = set((work / 'id1/maps').glob('__quiver_editor_*.bsp')) - old_results
        if len(new_results) != 1: raise RuntimeError('far-coordinate playtest BSP not found')
        far_bsp = new_results.pop().read_bytes()
        if len(far_bsp) < 124 or far_bsp[:4] != b'BSP2':
            raise RuntimeError('far-coordinate playtest did not produce BSP2')
        for lump, name in ((4, 'visibility'), (8, 'lighting')):
            offset, length = struct.unpack_from('<ii', far_bsp, 4 + 8 * lump)
            if offset < 0 or length <= 0 or offset + length > len(far_bsp) or not any(far_bsp[offset:offset + length]):
                raise RuntimeError(f'far-coordinate playtest missing {name}')
        positions = engine.cmd('wait\nwait\nviewpos\nedict 1 origin')
        view = re.search(r'Viewpos:\s*\(\s*(-?\d+)\s+(-?\d+)\s+(-?\d+)', positions)
        server = re.search(r'Edict 1\.origin==\s*[\'\"]?\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)', positions)
        if not view or not server:
            raise RuntimeError('client or server position missing:\n' + positions)
        for name, match in (('client', view), ('server', server)):
            x, y, z = map(float, match.groups())
            if abs(x - 100064) > 2 or abs(y - 64) > 2 or not 22 <= z <= 40:
                raise RuntimeError(f'{name} spawned at the wrong position: {(x, y, z)}')
        screenshot = engine.photo('far-sector-playtest')
        with Image.open(screenshot) as image:
            scene = image.convert('RGB').crop((100, 60, args.width - 100, args.height - 100))
            means = ImageStat.Stat(scene).mean
            if max(means) < 8 or max(channel[1] - channel[0] for channel in scene.getextrema()) < 12:
                raise RuntimeError(f'far-coordinate scene appears blank or unlit: channel means {means}')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: repeated-click face selection, 3D edits, partial portal, save/reopen, and F9 BSP2 playtests at origin and x=100064', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
