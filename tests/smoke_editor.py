#!/usr/bin/env python3
"""Linux smoke test for source-map save, async QBSP play and rebuild.

Requires stock Quake data, SDL development headers, a display, and a QBSP
executable. Files and settings go to a temporary directory printed on stdout.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile

import bench_meshlets as bench

# The editor intentionally captures ordinary game bindings. Open the console
# with Shift+Escape before injecting a command, and use P for a real UI edit.
bench.SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef USE_SDL3
#include <SDL3/SDL.h>
typedef bool poll_result;
#define DOWN SDL_EVENT_KEY_DOWN
#define UP SDL_EVENT_KEY_UP
#else
#include <SDL2/SDL.h>
typedef int poll_result;
#define DOWN SDL_KEYDOWN
#define UP SDL_KEYUP
#endif
static int mouse_mode;
#ifdef USE_SDL3
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) {
    static SDL_MouseButtonFlags (*real_mouse)(float *, float *);
#else
Uint32 SDL_GetMouseState(int *x, int *y) {
    static Uint32 (*real_mouse)(int *, int *);
#endif
    if (!real_mouse) real_mouse = dlsym(RTLD_NEXT, "SDL_GetMouseState");
    if (!mouse_mode) return real_mouse(x, y);
    if (x) *x = mouse_mode == 1 ? atoi(getenv("SMOKE_WIDTH")) / 2 : -1000;
    if (y) *y = mouse_mode == 1 ? atoi(getenv("SMOKE_HEIGHT")) / 2 : -1000;
    return 0;
}
poll_result SDL_PollEvent(SDL_Event *event) {
    static poll_result (*real_poll)(SDL_Event *);
    static int fd = -1, step = 0, action = 0;
    if (!real_poll) { real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent"); fd = atoi(getenv("BENCH_KEY_FD")); }
    char signal;
    if (!step && read(fd, &signal, 1) == 1) {
        action = signal;
        if (action == 'i' || action == 'o') { mouse_mode = action == 'i' ? 1 : 2; action = 'p'; }
        step = 1;
    }
    if (event && step) {
        int sc, down;
        if (action == 'c' && step <= 4) {
            sc = (step == 1 || step == 4) ? SDL_SCANCODE_LSHIFT : SDL_SCANCODE_ESCAPE;
            down = step <= 2;
        } else {
            int last = action == 'c' ? step - 4 : step;
            sc = action == 'p' ? SDL_SCANCODE_P : action == 'f' ? SDL_SCANCODE_F9 : action == 't' ? SDL_SCANCODE_TAB : SDL_SCANCODE_F12;
            down = last == 1;
        }
        int end = action == 'c' ? 6 : 2;
        if (++step > end) step = 0;
        SDL_zero(*event); event->type = down ? DOWN : UP;
#ifdef USE_SDL3
        event->key.scancode = sc; event->key.key = SDL_GetKeyFromScancode(sc, 0, 0); event->key.down = down;
#else
        event->key.keysym.scancode = sc; event->key.keysym.sym = SDL_GetKeyFromScancode(sc); event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
#endif
        return 1;
    }
    return real_poll(event);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--qbsp', type=Path, help='override compiler (default: bundled compiler discovery)')
    p.add_argument('--timeout', type=int, default=60)
    p.add_argument('--width', type=int, default=800)
    p.add_argument('--height', type=int, default=600)
    p.add_argument('--validation', action='store_true')
    args = p.parse_args()
    args.binary = args.binary.resolve()
    compiler_setting = str(args.qbsp.resolve()) if args.qbsp else ''
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source = source / 'id1'
    paks = sorted(f for f in source.iterdir() if re.fullmatch(r'pak\d+\.pak', f.name.lower()))
    if not paks: p.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-editor-smoke-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / 'id1/boot.cfg').write_text(bench.BOOT)
    os.environ['SMOKE_WIDTH'] = str(args.width)
    os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)
    def inject(body, signal):
        engine.serial += 1
        marker = f'EDITOR_CMD_{engine.serial}_DONE'
        start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, signal)
        engine.wait(lambda t: marker in t, start)
        return start

    def capture_editor():
        start = inject('editor_return\nwait\nwait\nscreenshot png', b'c')
        path = engine.wait(lambda t: re.search(r'Wrote [\x01\x02]?([^\n]+\.png)', t), start).group(1)
        source = next(work.rglob(path), None)
        if not source: raise RuntimeError('editor screenshot missing')
        shutil.copy2(source, work / 'editor.png')

    try:
        engine.wait(lambda t: 'CL_SignonReply: 4' in t)
        engine.cmd('bind F9 "echo F9_GAME_BINDING"')
        start = len(engine.text())
        os.write(engine.key, b'f')
        engine.wait(lambda t: 'F9_GAME_BINDING' in t, start)
        output = engine.cmd(f'disconnect\neditor_qbsp "{compiler_setting}"\neditor_new\neditor_save smoke')
        if 'Map saved' not in output: raise RuntimeError('new/save failed: ' + output)
        save = work / 'id1/maps/smoke.map'
        if not save.exists(): raise RuntimeError('source map missing')
        capture_editor()
        # The perspective viewport is the default; retain the existing
        # deterministic top-down cursor coordinates for spawn/leak checks.
        os.write(engine.key, b't')
        start = len(engine.text())
        os.write(engine.key, b'f')
        engine.wait(lambda t: 'Compiling map for playtest' in t, start)
        engine.wait(lambda t: 'Starting playtest' in t, start)
        engine.wait(lambda t: 'CL_SignonReply: 4' in t, start)
        # A successful playtest must have finished every embedded stage, not
        # merely the geometry compiler.
        if not all(message in engine.text()[start:] for message in ('Calculating visibility', 'Calculating lighting')):
            raise RuntimeError('playtest skipped VIS or LIGHT')
        if re.search(r'Texture stone1 not found|texture\(s\) missing from BSP', engine.text()[start:], re.I):
            raise RuntimeError('starter texture not embedded in playtest BSP')
        engine.photo('playtest')
        # An unchanged document must reuse its existing compiled output.
        first_builds = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(first_builds) != 1: raise RuntimeError('expected exactly one initial BSP')
        bsp = first_builds[0].read_bytes()
        if len(bsp) < 124 or bsp[:4] != b'BSP2':
            raise RuntimeError('compiled BSP header is invalid')
        for lump, name in ((4, 'visibility'), (8, 'lighting')):
            offset, length = struct.unpack_from('<ii', bsp, 4 + 8 * lump)
            if offset < 0 or length <= 0 or offset + length > len(bsp):
                raise RuntimeError(f'compiled BSP has no valid {name} lump')
        start = len(engine.text())
        os.write(engine.key, b'f')
        engine.wait(lambda t: 'Editing source map' in t, start)
        start = len(engine.text())
        os.write(engine.key, b'f')
        engine.wait(lambda t: 'Starting playtest' in t, start)
        engine.wait(lambda t: 'CL_SignonReply: 4' in t, start)
        if 'Compiling map for playtest' in engine.text()[start:]:
            raise RuntimeError('unchanged play unnecessarily recompiled')
        if len(list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))) != 1:
            raise RuntimeError('unchanged play generated another BSP')
        # A failed compiler must leave the edited document recoverable.
        engine.cmd('editor_return')
        inject('editor_qbsp "/nonexistent/quiver-smoke-qbsp"\neditor_return', b'c')
        start = len(engine.text())
        os.write(engine.key, b'i')
        engine.wait(lambda t: 'Player spawn placed' in t, start)
        start = inject('editor_play', b'c')
        engine.wait(lambda t: 'Could not start QBSP' in t or 'QBSP failed' in t, start)
        recover = inject(f'editor_save recovered\neditor_qbsp "{compiler_setting}"\neditor_return', b'k')
        if 'Map saved' not in engine.text()[recover:]:
            raise RuntimeError('could not save edited document after compiler failure')
        if (work / 'id1/maps/recovered.map').read_bytes() == save.read_bytes():
            raise RuntimeError('recovered map lost its edit')
        start = inject('editor_play', b'c')
        engine.wait(lambda t: 'Starting playtest' in t, start)
        engine.wait(lambda t: 'CL_SignonReply: 4' in t, start)
        generated = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(generated) < 2: raise RuntimeError('modified play did not generate a new BSP')
        sources = list((work / 'id1/maps').glob('__quiver_editor_*.map'))
        if len(sources) < 2 or sources[0].read_bytes() == sources[1].read_bytes():
            raise RuntimeError('edited source did not change')
        if not args.qbsp:
            # Move the player outside the sealed starter room. Bundled QBSP's
            # -leaktest must reject it, retaining the in-memory document.
            engine.cmd('editor_return')
            leak_start = len(engine.text())
            os.write(engine.key, b'o')
            engine.wait(lambda t: 'Player spawn placed' in t, leak_start)
            leak_start = inject('editor_play', b'c')
            engine.wait(lambda t: 'QBSP failed' in t, leak_start)
            if 'Aborting because -leaktest was used' not in engine.text()[leak_start:]:
                raise RuntimeError('bundled compiler failure did not report the expected leak')
            recovered = inject('editor_save leaked', b'k')
            if 'Map saved' not in engine.text()[recovered:]:
                raise RuntimeError('could not save document after leaktest failure')
        if args.validation:
            output = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in output:
                raise RuntimeError('Vulkan validation layer was not enabled')
            if re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', output, re.I):
                raise RuntimeError('Vulkan validation errors in output.log')
        print('PASS: F9 play/return, save, reuse, edit, compiler failure recovery, recompile, play'
              + (', bundled leaktest and recovery' if not args.qbsp else ''), flush=True)
    finally:
        engine.close()

if __name__ == '__main__': main()
