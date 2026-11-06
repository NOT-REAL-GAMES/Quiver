#!/usr/bin/env python3
"""Check 3D editor visibility against ray-box depth on the real Vulkan renderer.

Requires Linux, a working display/Vulkan driver, SDL development headers, Pillow
and stock Quake PAKs. All game data/settings are isolated in a temporary folder.
It renders crossing solid brushes separately and in both submission orders,
then compares each image with an independent ray/AABB visibility calculation.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import tempfile
import time

from PIL import Image
import bench_meshlets as bench
import smoke_editor  # Installs the SDL input shim used below.
from qbsp_smoke import brush

# Sys_Printf writes to a redirected file in this harness. Flush it while the
# process runs so we can verify the actual MSAA mode, not just its cvar.
bench.SHIM = bench.SHIM.replace('#include <stdlib.h>', '#include <stdlib.h>\n#include <stdio.h>')
bench.SHIM = bench.SHIM.replace('poll_result SDL_PollEvent(SDL_Event *event) {',
                                'poll_result SDL_PollEvent(SDL_Event *event) {\n    fflush(stdout);')
bench.SHIM = bench.SHIM.replace(
    'if (x) *x = mouse_mode == 1 ? atoi(getenv("SMOKE_WIDTH")) / 2 : -1000;',
    'if (x) *x = mouse_mode == 3 ? atoi(getenv("DEPTH_MOUSE_X")) : mouse_mode == 1 ? atoi(getenv("SMOKE_WIDTH")) / 2 : -1000;')
bench.SHIM = bench.SHIM.replace(
    'if (y) *y = mouse_mode == 1 ? atoi(getenv("SMOKE_HEIGHT")) / 2 : -1000;',
    'if (y) *y = mouse_mode == 3 ? atoi(getenv("DEPTH_MOUSE_Y")) : mouse_mode == 1 ? atoi(getenv("SMOKE_HEIGHT")) / 2 : -1000;')
bench.SHIM = bench.SHIM.replace('action = signal;', 'action = signal; if (action == \'s\') mouse_mode = 3;')
# Ignore physical input in this isolated process. A real WASD key arriving
# between captures moves the editor camera and invalidates the ray reference;
# synthetic test events above the real SDL poll remain unaffected.
bench.SHIM = bench.SHIM.replace('    return real_poll(event);', r'''    if (!event) return real_poll(event);
    poll_result result;
    while ((result = real_poll(event))) {
#ifdef USE_SDL3
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP ||
            event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_MOUSE_WHEEL) continue;
#else
        if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP || event->type == SDL_MOUSEMOTION ||
            event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP || event->type == SDL_MOUSEWHEEL) continue;
#endif
        return result;
    }
    return result;''')
bench.SHIM = bench.SHIM.replace('    if (event && step) {\n        int sc, down;', r'''    if (event && step && action == 's') {
        SDL_zero(*event);
#ifdef USE_SDL3
        event->type = step == 1 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event->button.down = step == 1;
#else
        event->type = step == 1 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        event->button.state = step == 1 ? SDL_PRESSED : SDL_RELEASED;
#endif
        event->button.button = SDL_BUTTON_LEFT;
        if (++step > 2) step = 0;
        return 1;
    }
    if (event && step) {
        int sc, down;''')

BOX_A = ((80, -120, 0), (230, 120, 20))
BOX_B = ((120, -100, -20), (150, 90, 140))


def intersect(camera, direction, box):
    near, far = 1.0, float('inf')
    for axis in range(3):
        if abs(direction[axis]) < 1e-12:
            if not box[0][axis] <= camera[axis] <= box[1][axis]:
                return None
            continue
        a = (box[0][axis] - camera[axis]) / direction[axis]
        b = (box[1][axis] - camera[axis]) / direction[axis]
        near, far = max(near, min(a, b)), min(far, max(a, b))
    return near if far >= near else None


def map_text(boxes, spawn):
    return ('{\n"classname" "worldspawn"\n'
            + ''.join(brush(*lo, *hi) for lo, hi in boxes)
            + '}\n{\n"classname" "info_player_start"\n"origin" "' + ' '.join(map(str, spawn)) + '"\n}\n')


def capture(engine, work, name, first, selected=False):
    engine.serial += 1
    marker = f'EDITOR_DEPTH_CAPTURE_{engine.serial}_DONE'
    command = 'editor_return' if selected else f'editor_open {name}'
    # Let each swapchain image see the new editor document before readback.
    (work / 'id1/control.tmp').write_text(command + '\n' + 'wait\n' * 8 + f'screenshot png\necho {marker}\n')
    (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
    start = len(engine.text())
    os.write(engine.key, b'k' if first else b'c')
    result = engine.wait(lambda text: text if marker in text and 'Wrote ' in text else None, start)
    match = re.search(r'Wrote [\x01\x02]?([^\n]+\.png)', result)
    if not match or ('Editing source map' if selected else 'Source map opened') not in result:
        raise RuntimeError(f'Could not capture {name}: {result}')
    path = next(work.rglob(match[1]), None)
    if not path:
        raise RuntimeError(f'Missing screenshot {match[1]}')
    target = work / (name + ('_selected' if selected else '') + '.png')
    shutil.copy2(path, target)
    with Image.open(target) as image:
        return image.convert('RGB')


def disconnect(engine, work):
    # The startup screen can consume the first injected F12 while signon is
    # finishing. Retry briefly; the command is safe to execute more than once.
    marker = 'EDITOR_DEPTH_DISCONNECTED'
    (work / 'id1/control.tmp').write_text('disconnect\necho ' + marker + '\n')
    (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
    start = len(engine.text())
    for _ in range(20):
        os.write(engine.key, b'k')
        deadline = time.monotonic() + .2
        while time.monotonic() < deadline:
            if marker in engine.text()[start:]: return
            if engine.process.poll() is not None:
                raise RuntimeError('engine exited while disconnecting')
            time.sleep(.025)
    raise RuntimeError('could not issue initial disconnect')


def check(images, camera, width, height):
    a, b, ab, ba = (images[name] for name in ('a', 'b', 'ab', 'ba'))
    focal = (height - 80) * .65
    center_y = (48 + height - 32) * .5
    count = [0, 0]
    errors = [0, 0]
    order_errors = 0
    selected_samples = selected_errors = outline_samples = outline_errors = 0
    # Skip edges and colors that cannot distinguish the two visible faces.
    for y in range(52, height - 36, 3):
        for x in range(4, width - 4, 3):
            ray = (1.0, -(x + .5 - width * .5) / focal, (center_y - y - .5) / focal)
            da, db = intersect(camera, ray, BOX_A), intersect(camera, ray, BOX_B)
            if da is None or db is None or abs(da - db) < 3:
                continue
            pa, pb = a.getpixel((x, y)), b.getpixel((x, y))
            if pa == pb:
                continue
            if any(a.getpixel((x + dx, y + dy)) != pa or b.getpixel((x + dx, y + dy)) != pb
                   for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))):
                continue
            nearest = 0 if da < db else 1
            expected = pa if nearest == 0 else pb
            count[nearest] += 1
            errors[nearest] += ab.getpixel((x, y)) != expected
            order_errors += ab.getpixel((x, y)) != ba.getpixel((x, y))
            if nearest == 0 and images['b_selected'].getpixel((x, y)) != pb:
                selected_samples += 1
                selected_errors += images['ab_selected'].getpixel((x, y)) != pa
    # Outlines can be only one pixel wide and align between the samples above.
    # Scan them at full resolution, away from the near brush's raster edges.
    for y in range(52, height - 36):
        for x in range(4, width - 4):
            selected_color = images['b_selected'].getpixel((x, y))
            if min(selected_color) < 180 or max(selected_color) - min(selected_color) > 35 or selected_color == b.getpixel((x, y)):
                continue
            ray = (1.0, -(x + .5 - width * .5) / focal, (center_y - y - .5) / focal)
            da, db = intersect(camera, ray, BOX_A), intersect(camera, ray, BOX_B)
            if da is None or db is None or db - da < 3:
                continue
            pa = a.getpixel((x, y))
            if any(a.getpixel((x + dx, y + dy)) != pa for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))):
                continue
            outline_samples += 1
            outline_errors += images['ab_selected'].getpixel((x, y)) != pa
    if min(count) < 100:
        raise AssertionError(f'Fixture did not exercise both depth orders: {count}')
    rates = [errors[i] / count[i] for i in range(2)]
    if max(rates) > .01 or order_errors / sum(count) > .01:
        raise AssertionError(f'Wrong depth visibility: samples={count}, wrong={errors}, order differences={order_errors}')
    if selected_samples < 100 or selected_errors / selected_samples > .01:
        raise AssertionError(f'Occluded selection shows through: {selected_errors}/{selected_samples}')
    if outline_samples < 2 or outline_errors / outline_samples > .05:
        raise AssertionError(f'Occluded selection outlines show through: {outline_errors}/{outline_samples}')
    print(f'  visibility OK: near A/B samples {count}, wrong {errors}, order differences {order_errors}', flush=True)
    print(f'  occluded selection OK: {selected_errors}/{selected_samples}, outlines {outline_errors}/{outline_samples}', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=45)
    p.add_argument('--width', type=int, default=800)
    p.add_argument('--height', type=int, default=600)
    p.add_argument('--validation', action='store_true')
    p.add_argument('--fsaa', type=int, nargs='+', default=[0, 4])
    p.add_argument('--render-width', type=int, help='optional scene render width below the window width')
    p.add_argument('--render-height', type=int, help='optional scene render height below the window height')
    args = p.parse_args()
    if bool(args.render_width) != bool(args.render_height):
        p.error('--render-width and --render-height must be supplied together')
    args.binary = args.binary.resolve()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source = source / 'id1'
    paks = sorted(f for f in source.iterdir() if re.fullmatch(r'pak\d+\.pak', f.name.lower()))
    if not paks: p.error(f'no Quake PAKs found in {source}')
    os.environ['SMOKE_WIDTH'], os.environ['SMOKE_HEIGHT'] = str(args.width), str(args.height)
    os.environ['DEPTH_MOUSE_X'], os.environ['DEPTH_MOUSE_Y'] = str(args.width // 2), str(args.height // 2 - 100)
    for fsaa in args.fsaa:
        for disconnected in (False, True):
            work = Path(tempfile.mkdtemp(prefix='quiver-editor-depth-'))
            print(f'FSAA {fsaa}, {"disconnected" if disconnected else "loaded world"}: {work}', flush=True)
            (work / 'id1/maps').mkdir(parents=True)
            (work / 'user').mkdir()
            for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
            # Restart after selecting sample count so the test exercises the
            # actual framebuffer, rather than only changing the cvar.
            render_size = f'r_width {args.render_width}\nr_height {args.render_height}\n' if args.render_width else ''
            boot = bench.BOOT.replace('vid_fsaa 0\n', f'vid_fsaa {fsaa}\n{render_size}vid_restart\n')
            # Keep unrelated ray-traced-shadow acceleration-structure updates
            # out of this editor depth synchronization test.
            (work / 'id1/boot.cfg').write_text(boot.replace('map start\n', 'r_rtshadows 0\nmap start\n'))
            cases = [('a', [BOX_A]), ('b', [BOX_B]), ('ab', [BOX_A, BOX_B]), ('ba', [BOX_B, BOX_A])]
            for camera_id, spawn in enumerate(((0, 0, 32), (25, 40, 12))):
                for suffix, boxes in cases:
                    (work / 'id1/maps' / f'depth{camera_id}{suffix}.map').write_text(map_text(boxes, spawn))
            engine = bench.Engine(args, work)
            try:
                engine.wait(lambda text: 'CL_SignonReply: 4' in text)
                log = (work / 'output.log').read_text(errors='replace')
                aa = re.findall(r'(?m)^(AA disabled|\d+ AA Samples)$', log)
                expected_aa = 'AA disabled' if fsaa < 2 else f'{fsaa} AA Samples'
                if not aa or aa[-1] != expected_aa:
                    raise RuntimeError(f'Requested FSAA {fsaa}, but active mode is {aa[-1:] or "unknown"}')
                if disconnected: disconnect(engine, work)
                first = True
                for camera_id, spawn in enumerate(((0, 0, 32), (25, 40, 12))):
                    images = {}
                    for suffix, _ in cases:
                        images[suffix] = capture(engine, work, f'depth{camera_id}{suffix}', first)
                        first = False
                        if suffix in ('b', 'ab'):
                            os.write(engine.key, b's')
                            images[suffix + '_selected'] = capture(engine, work, f'depth{camera_id}{suffix}', False, selected=True)
                    check(images, (spawn[0], spawn[1], spawn[2] + 48), args.width, args.height)
                if args.validation:
                    log = (work / 'output.log').read_text(errors='replace')
                    if 'Using VK_LAYER_KHRONOS_validation' not in log:
                        raise RuntimeError('Vulkan validation layer was not enabled')
                    if re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', log, re.I):
                        raise RuntimeError('Vulkan validation errors in output.log')
            finally:
                # The editor consumes F12, so terminate this isolated process
                # rather than asking the generic benchmark helper to bind quit.
                if engine.process.poll() is None:
                    engine.process.terminate()
                    try:
                        engine.process.wait(timeout=5)
                    except Exception:
                        engine.process.kill()
                        engine.process.wait()
                os.close(engine.key)
                os.close(engine.read_fd)
                engine.log.close()
    print('PASS: intersecting brushes, both draw orders and camera positions, loaded and disconnected worlds', flush=True)


if __name__ == '__main__': main()
