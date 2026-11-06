#!/usr/bin/env python3
"""Render an old fragmented portal and check its outline on the real GPU.

Requires a Vulkan display, SDL development headers, Pillow, and Quake PAKs.
All writable game data is isolated in a temporary directory.
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
import editor_depth_visual  # Supplies a deterministic SDL injector.

# Add the editor's V key to the existing isolated injector.
bench.SHIM = bench.SHIM.replace(
    "sc = action == 'p' ? SDL_SCANCODE_P",
    "sc = action == 'v' ? SDL_SCANCODE_V : action == 'p' ? SDL_SCANCODE_P")


def capture(engine, work, command, first):
    engine.serial += 1
    marker = f'PORTAL_OUTLINE_{engine.serial}'
    (work / 'id1/control.tmp').write_text(command + '\n' + 'wait\n' * 8 + 'screenshot png\necho ' + marker + '\n')
    (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
    start = len(engine.text()); os.write(engine.key, b'k' if first else b'c')
    text = engine.wait(lambda s: s if marker in s and 'Wrote ' in s else None, start)
    match = re.search(r'Wrote [\x01\x02]?([^\n]+\.png)', text)
    if not match: raise RuntimeError('screenshot path missing: ' + text)
    path = next(work.rglob(match[1]), None)
    if not path: raise RuntimeError('screenshot missing: ' + match[1])
    image = Image.open(path).convert('RGB')
    image.save(work / f'{marker}.png')
    return image


def check(image):
    # The fixture's two linked rooms share x=256.  Camera is (0,0,80),
    # yaw 0; its old portal consists of three coplanar fragments.  Yellow
    # seam edges used to cross the opening along (y,z)=(128,0)->(-128,128)
    # and (128,64)->(0,128), while the four outer edges are real boundaries.
    width, height = image.size
    focal = (height - 80) * 0.65
    center_y = (48 + height - 32) / 2
    def screen(y, z):
        return round(width / 2 - y * focal / 256), round(center_y - (z - 80) * focal / 256)
    def yellow(x, y):
        return sum(1 for yy in range(y-2, y+3) for xx in range(x-2, x+3)
                   if 0 <= xx < width and 0 <= yy < height and
                   (lambda c: c[0] > 180 and c[1] > 180 and c[2] < 100)(image.getpixel((xx, yy))))
    inner = [screen(128 - 256 * t, 128 * t) for t in (0.2, 0.35, 0.5, 0.7, 0.85)]
    inner += [screen(128 - 128 * t, 64 + 64 * t) for t in (0.2, 0.4, 0.6, 0.8)]
    edges = [screen(-128, 32), screen(128, 32), screen(-64, 128), screen(64, 128),
             screen(-64, 0), screen(64, 0)]
    bad = [(p, yellow(*p)) for p in inner if yellow(*p) > 0]
    missing = [(p, yellow(*p)) for p in edges if yellow(*p) == 0]
    if bad or missing:
        raise AssertionError(f'portal seams/boundaries incorrect: interior={bad}; missing={missing}')


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
    work = Path(tempfile.mkdtemp(prefix='quiver-portal-outline-'))
    print(f'visual directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    shutil.copyfile(Path(__file__).parent / 'fixtures/sector_link_fragmented.map', work / 'id1/maps/portal.map')
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start\n', 'r_rtshadows 0\nmap start\n'))
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.cmd('disconnect\neditor_open portal')
        time.sleep(.25)
        check(capture(engine, work, 'editor_return', False))
        os.write(engine.key, b'v'); time.sleep(.2)
        check(capture(engine, work, 'editor_return', False))
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: old fragmented portal outlines in perspective and vertex modes', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
