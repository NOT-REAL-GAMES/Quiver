#!/usr/bin/env python3
"""Exercise moving brush occluders and their real-time Radiance shadows.

All writable game state and the generated BSP are isolated; user PAKs are
read-only. This optional test needs a display, Vulkan, Pillow and map tools.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

from PIL import Image, ImageChops, ImageStat
import bench_meshlets as bench
from qbsp_smoke import brush


def make_maps(binary, work):
    maps = work / 'id1/maps'
    wad = Path(__file__).resolve().parents[1] / 'Misc/editor_defaults.wad'
    walls = [(-256,-200,-16,256,200,0), (-256,-200,144,256,200,160),
             (-256,-200,0,-240,200,144), (240,-200,0,256,200,144),
             (-240,-200,0,240,-184,144), (-240,184,0,240,200,144)]
    source = '{\n"classname" "worldspawn"\n"wad" "' + str(wad) + '"\n'
    source += ''.join(brush(*wall) for wall in walls) + '}\n'
    source += '{\n"classname" "info_player_start"\n"origin" "-192 0 32"\n"angle" "0"\n}\n'
    source += '{\n"classname" "light"\n"origin" "-140 -110 110"\n"light" "700"\n}\n'
    source += '{\n"classname" "prop_dynamic"\n"origin" "0 -80 64"\n"velocity" "0 20 0"\n'
    source += brush(-8,-16,-32,8,16,32) + '}\n'
    src, bsp = maps / 'moving_shadow.map', maps / 'moving_shadow.bsp'
    src.write_text(source)
    for cmd in (('qbsp', '-bsp2', '-nopercent', '-leaktest', src, bsp),
                ('vis', '-threads', '2', bsp), ('light', '-threads', '2', bsp)):
        result = subprocess.run([str(binary), '--quiver-map-tool', *map(str, cmd)], cwd=work,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise RuntimeError(f'{cmd[0]} failed:\n{result.stdout}')
    data = bytearray(bsp.read_bytes())
    offset, length = struct.unpack_from('<ii', data, 4)
    entities = re.findall(rb'\{[^{}]*\}', bytes(data[offset:offset+length]))
    props = [e for e in entities if b'"classname" "prop_dynamic"' in e]
    if len(props) != 1: raise RuntimeError('expected one dynamic brush')
    # Preserve all BSP geometry and submodels; only remove the entity instance.
    rest = [e for e in entities if e not in props]
    modified = bytearray(data)
    lump = b'\n'.join(rest) + b'\n\0'
    struct.pack_into('<ii', modified, 4, len(modified), len(lump))
    modified.extend(lump)
    (maps / 'moving_shadow_empty.bsp').write_bytes(modified)


def prop_origin(engine):
    edicts = engine.cmd('edicts')
    for item in re.finditer(r'EDICT\s+(\d+):\s*(.*?)(?=\n\s*EDICT\s+\d+:|\nFree-list:|\Z)', edicts, re.S):
        if re.search(r'(?m)^classname\s+prop_dynamic\s*$', item[2]):
            detail = engine.cmd(f'edict {item[1]}')
            origin = re.search(r"origin\s+'\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s*'", detail)
            if origin: return tuple(map(float, origin.groups()))
    raise RuntimeError('dynamic brush or its origin was not found')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--backend', choices=('software', 'hardware'), default='software')
    parser.add_argument('--msaa', type=int, choices=(0, 4), default=0)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    args.binary = args.binary.resolve(); args.width, args.height, args.game = 640, 360, None
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-moving-shadow-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    make_maps(args.binary, work)
    boot = bench.BOOT.replace('vid_fsaa 0', f'vid_fsaa {args.msaa}').replace(
        'map start', 'r_radiance 1\nr_radiance_hdr 1\nmap moving_shadow_empty')
    (work / 'id1/boot.cfg').write_text(boot)
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda t: 'CL_SignonReply: 4' in t)
        backend = 1 if args.backend == 'software' else 2
        active = 'software' if args.backend == 'software' else 'ray-query'
        engine.cmd(f'r_radiance_backend {backend}\nr_radiance 1\nr_radiance_hdr 1\n'
                   'r_radiance_quality 0\nr_radiance_bounce_gain 0\nr_radiance_emissive_gain 0\n'
                   'r_radiance_point_gain 1\nr_radiance_highlight 0\nr_drawviewmodel 0\ncrosshair 0\n'
                   'scr_showfps 0\nviewsize 100\nhost_maxfps 30\nwait\nwait')
        engine.view('moving_shadow_empty', '-192 0 64 0 0 0')
        status = engine.cmd('radiance_stats')
        if f'Radiance: requested=1 active=1 backend={active}' not in status:
            raise RuntimeError(f'Radiance unavailable: {status}')
        empty = engine.photo('empty')
        engine.cmd('pause\nwait')
        engine.view('moving_shadow', '-192 0 64 0 0 0')
        initial = prop_origin(engine)
        status = engine.cmd('radiance_stats')
        if (f'Radiance: requested=1 active=1 backend={active}' not in status or
                not re.search(r'\bdynamic=1\s+omitted=0\b', status)):
            raise RuntimeError(f'dynamic occluder unavailable: {status}')
        if not -75 < initial[1] < -45:
            raise RuntimeError(f'initial brush position outside expected shadow window: {initial}')
        before = engine.photo('before')
        engine.cmd('pause')
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            moving = prop_origin(engine)
            if moving[1] >= -8: break
            time.sleep(.05)
        else: raise RuntimeError('dynamic brush never reached the target position')
        engine.cmd('pause\nwait\nwait')
        final = prop_origin(engine)
        status = engine.cmd('radiance_stats')
        if (f'Radiance: requested=1 active=1 backend={active}' not in status or
                not re.search(r'\bdynamic=1\s+omitted=0\b', status)):
            raise RuntimeError(f'moved occluder unavailable: {status}')
        after = engine.photo('after')
        print(f'prop origin {initial} -> {final}; captures: {empty}, {before}, {after}', flush=True)
        if final[1] - initial[1] < 30 or final[1] > 8:
            raise RuntimeError('dynamic brush position is outside the target shadow window')
        with Image.open(empty) as e, Image.open(before) as b, Image.open(after) as a:
            e, b, a = (im.convert('L').crop((0, 0, 640, 300)) for im in (e, b, a))
            # These two small regions lie on the back wall and outside the
            # visible prop in either pose. Moving geometry alone cannot make
            # them dark; only the displaced shadow should do that.
            first, second = (280, 200, 300, 220), (210, 200, 240, 220)
            def darkening(image, box):
                return ImageStat.Stat(ImageChops.subtract(e.crop(box), image.crop(box))).mean[0]
            b_first, b_second = darkening(b, first), darkening(b, second)
            a_first, a_second = darkening(a, first), darkening(a, second)
            print(f'back-wall shadow darkening before={b_first:.2f}/{b_second:.2f} '
                  f'after={a_first:.2f}/{a_second:.2f}', flush=True)
            if b_first < 8 or a_second < 8 or a_first > 4 or b_second > 4:
                raise RuntimeError(f'shadow failed to move between back-wall regions; inspect {work}')
    finally:
        engine.close()
    output = engine.text() + (work / 'output.log').read_text(errors='replace')
    formats = re.findall(r'Scene target format: ([^\r\n]+)', output)
    if not formats or formats[-1].strip() != 'RGBA16F':
        raise RuntimeError(f'HDR scene target unavailable: {formats}; inspect {work}')
    if args.msaa and f'{args.msaa} AA Samples' not in output:
        raise RuntimeError(f'{args.msaa}x MSAA was not active; inspect {work}')
    if args.validation and ('Using VK_LAYER_KHRONOS_validation' not in output or
       re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output)):
        raise RuntimeError(f'validation unavailable or reported an error; inspect {work}')
    print(f'PASS: moving brush shadows; captures in {work}', flush=True)


if __name__ == '__main__': main()
