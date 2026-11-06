#!/usr/bin/env python3
"""Check that visible translucent slime is composited with Radiance in each OIT mode.

Runs in a disposable game directory with stock E1M1 and never alters saved
settings. The selected pose is over a known slime surface. Its opacity toggle
must visibly change the same paused scene in every renderer mode; this proves
that the test is exercising transparent geometry rather than only switching
pipelines. The captures are retained for visual inspection.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import tempfile
import time

from PIL import Image, ImageChops
import bench_meshlets as bench


def make_fixture(paks, destination):
    """Copy the stock map and add runtime lights around the target pool.

    Modifying only the entity lump leaves the BSP geometry and its PVS intact.
    The original pak and map are never modified.
    """
    data = None
    for pak in reversed(paks):
        with pak.open('rb') as stream:
            magic, offset, size = struct.unpack('<4sii', stream.read(12))
            if magic != b'PACK' or offset < 0 or size < 0 or size % 64:
                raise RuntimeError(f'invalid PAK directory: {pak}')
            stream.seek(offset)
            directory = stream.read(size)
            for entry in range(0, len(directory), 64):
                name, start, length = struct.unpack_from('<56sii', directory, entry)
                if name.split(b'\0', 1)[0].lower() == b'maps/e1m1.bsp':
                    stream.seek(start)
                    data = bytearray(stream.read(length))
                    break
        if data is not None:
            break
    if data is None:
        raise RuntimeError('E1M1 not present in the supplied PAKs')
    offset, size = struct.unpack_from('<ii', data, 4)
    entities = bytes(data[offset:offset + size]).rstrip(b'\0')
    entities += (b'\n{\n"classname" "light"\n"origin" "1200 2550 -360"\n"light" "650"\n}\n'
                 b'{\n"classname" "light"\n"origin" "1150 2500 -350"\n"light" "650"\n}\n\0')
    struct.pack_into('<ii', data, 4, len(data), len(entities))
    data.extend(entities)
    destination.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--backend', choices=('software', 'hardware'), default='software')
    parser.add_argument('--msaa', type=int, choices=(0, 4), default=0)
    parser.add_argument('--hdr', action='store_true', help='require the RGBA16F scene target and exercise native HUD/debug parity')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 640, 360, None
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-transparency-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / 'id1' / 'maps').mkdir()
    make_fixture(paks, work / 'id1' / 'maps' / 'rcslime.bsp')
    boot = bench.BOOT.replace('vid_fsaa 0', f'vid_fsaa {args.msaa}').replace(
        'map start', f'host_maxfps 30\nr_radiance 1\nr_radiance_hdr {int(args.hdr)}\nmap rcslime')
    (work / 'id1' / 'boot.cfg').write_text(boot)
    engine = bench.Engine(args, work)
    try:
        try:
            os.setpriority(os.PRIO_PROCESS, engine.process.pid, 15)
        except (AttributeError, OSError):
            pass
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.view('rcslime', '1200 2550 -280 60 0 0')
        backend = 1 if args.backend == 'software' else 2
        active_backend = 'software' if args.backend == 'software' else 'ray-query'
        engine.cmd(f'r_novis 1\nr_radiance_backend {backend}\nr_radiance 1\n'
                   f'r_radiance_hdr {int(args.hdr)}\nr_radiance_highlight 1\n'
                   'r_radiance_quality 0\nr_drawviewmodel 0\ncrosshair 0\n'
                   'r_wateralpha 0.25\nr_slimealpha 0.25\nwait\nwait')
        captures = []
        for mode in (0, 1, 2):
            engine.cmd(f'r_oit {mode}\nr_slimealpha 1\nwait\nwait')
            status = engine.cmd('radiance_stats')
            if f'Radiance: requested=1 active=1 backend={active_backend}' not in status:
                raise RuntimeError(f'Radiance unavailable at OIT {mode}: {status}')
            time.sleep(1)
            opaque = engine.photo(f'oit{mode}-opaque')
            engine.cmd('r_slimealpha 0.25\nwait\nwait')
            time.sleep(1)
            translucent = engine.photo(f'oit{mode}-translucent')
            captures.append(translucent)
            with Image.open(opaque) as a, Image.open(translucent) as b:
                # Exclude status bar, where player damage or other HUD state
                # would be unrelated to the slime opacity setting.
                a, b = a.convert('RGB').crop((0, 0, 640, 300)), b.convert('RGB').crop((0, 0, 640, 300))
                delta = ImageChops.difference(a, b).convert('L').histogram()
                changed = sum(delta[4:])
                if changed < 500:
                    raise RuntimeError(f'no clearly visible translucent surface at OIT {mode} ({changed} pixels); inspect {work}')
                print(f'OIT {mode}: {changed} scene pixels change with slime opacity', flush=True)
        # This scene has one visible translucent layer. The OIT pipelines
        # should reproduce the ordinary blend to within small rounding error.
        with Image.open(captures[0]) as baseline:
            baseline = baseline.convert('RGB').crop((0, 0, 640, 300))
            for mode, capture in enumerate(captures[1:], 1):
                with Image.open(capture) as image:
                    image = image.convert('RGB').crop((0, 0, 640, 300))
                    delta = ImageChops.difference(baseline, image).convert('L').histogram()
                    differing = sum(delta[6:])
                    if differing > 640 * 300 // 100:
                        raise RuntimeError(f'OIT {mode} differs from the single-layer reference at {differing} pixels; inspect {work}')
                    print(f'OIT {mode}: {differing} pixels differ from ordinary blending by >=6 levels', flush=True)
        if args.hdr:
            # Fullbright bypasses the HDR shoulder. Compare the whole paused
            # frame, including the native-resolution status bar, with only
            # that control changed. Force a visible bar and a stable HUD.
            engine.cmd('r_oit 0\nviewsize 100\nscr_showfps 0\nr_fullbright 1\nr_radiance_highlight 0\nwait\nwait')
            time.sleep(1)
            hud_off = engine.photo('hdr-fullbright-hud-shoulder-off')
            engine.cmd('r_radiance_highlight 1\nwait\nwait')
            time.sleep(1)
            hud_on = engine.photo('hdr-fullbright-hud-shoulder-on')
            with Image.open(hud_off) as a, Image.open(hud_on) as b:
                if ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox():
                    raise RuntimeError(f'HDR fullbright/HUD changed with the shoulder toggle; inspect {work}')
            print('HDR fullbright frame and native HUD: shoulder-toggle parity', flush=True)
            # Exercise the actual menu over the native HUD when toggling the
            # FP16 scene target. With fullbright, both scene paths should be
            # visually identical. Ignore small changes from the animated menu
            # cursor, while requiring the menu to visibly open.
            engine.cmd('menu_main\nwait\nwait')
            time.sleep(1)
            menu_hdr = engine.photo('hdr-main-menu')
            engine.cmd('r_radiance_hdr 0\nwait\nwait\nwait\nwait')
            time.sleep(1)
            menu_classic = engine.photo('unorm-main-menu')
            engine.cmd('r_radiance_hdr 1\nwait\nwait\nwait\nwait')
            time.sleep(1)
            menu_hdr_again = engine.photo('hdr-main-menu-after-toggle')
            with Image.open(hud_on) as hud, Image.open(menu_hdr) as a, Image.open(menu_classic) as b, Image.open(menu_hdr_again) as c:
                hud, a, b, c = (im.convert('RGB') for im in (hud, a, b, c))
                opened = sum(ImageChops.difference(hud, a).convert('L').histogram()[6:])
                if opened < 1000:
                    raise RuntimeError(f'main menu did not visibly open ({opened} pixels); inspect {work}')
                for name, image in (('UNORM', b), ('HDR after toggle', c)):
                    different = sum(ImageChops.difference(a, image).convert('L').histogram()[6:])
                    if different > 640 * 360 // 100:
                        raise RuntimeError(f'{name} menu differs from HDR at {different} pixels; inspect {work}')
                    print(f'{name} menu: {different} pixels differ by >=6 levels', flush=True)
    finally:
        engine.close()
    output = engine.text() + (work / 'output.log').read_text(errors='replace')
    expected_format = 'RGBA16F' if args.hdr else 'classic UNORM'
    formats = re.findall(r'Scene target format: ([^\r\n]+)', output)
    if not formats or formats[-1].strip() != expected_format:
        raise RuntimeError(f'expected {expected_format} target, got {formats}; inspect {work}')
    if args.hdr and [s.strip() for s in formats[-3:]] != ['RGBA16F', 'classic UNORM', 'RGBA16F']:
        raise RuntimeError(f'menu target toggles did not recreate HDR/UNORM/HDR resources: {formats}; inspect {work}')
    print(f'Scene target: {expected_format}', flush=True)
    if args.msaa and f'{args.msaa} AA Samples' not in output:
        raise RuntimeError(f'{args.msaa}x MSAA was not active; inspect {work}')
    if args.validation and ('Using VK_LAYER_KHRONOS_validation' not in output or
       re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output)):
        raise RuntimeError(f'validation unavailable or reported an error; inspect {work}')
    print(f'PASS: visible translucent slime with Radiance in all OIT modes; captures in {work}', flush=True)


if __name__ == '__main__':
    main()
