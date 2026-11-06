#!/usr/bin/env python3
"""Render overlapping transparent brush entities through each Radiance OIT path.

The maps share identical compiled geometry: only the entity lump changes. The
fixture proves both layers contribute in the same screen pixels, then compares
OIT images when the entity order is reversed. It requires the bundled map tools,
Quake PAK data, Pillow, SDL and a Vulkan display. All game state is disposable.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

from PIL import Image, ImageChops
import bench_meshlets as bench
from qbsp_smoke import brush


def make_maps(binary, work):
    maps = work / 'id1/maps'
    wad = Path(__file__).resolve().parents[1] / 'Misc/editor_defaults.wad'
    walls = [(-144,-144,-16,144,144,0), (-144,-144,128,144,144,144),
             (-144,-144,0,-128,144,128), (128,-144,0,144,144,128),
             (-128,-144,0,128,-128,128), (-128,128,0,128,144,128)]
    source = '{\n"classname" "worldspawn"\n"wad" "' + str(wad) + '"\n'
    source += ''.join(brush(*wall) for wall in walls) + '}\n'
    source += '{\n"classname" "info_player_start"\n"origin" "-80 0 32"\n"angle" "0"\n}\n'
    source += '{\n"classname" "light"\n"origin" "0 0 110"\n"light" "500"\n}\n'
    for x, shift in ((0, 0), (48, 12)):
        source += '{\n"classname" "prop_dynamic"\n"origin" "' + str(x) + ' 0 64"\n"alpha" "0.5"\n'
        source += brush(-2, -60, -48, 2, 60, 48).replace('stone1 0 0', f'stone1 {shift} {shift}') + '}\n'
    src, bsp = maps / 'overlap.map', maps / 'overlap.bsp'
    src.write_text(source)
    for cmd in (('qbsp','-bsp2','-nopercent','-leaktest',src,bsp),
                ('vis','-threads','2',bsp), ('light','-threads','2',bsp)):
        result = subprocess.run([str(binary), '--quiver-map-tool', *map(str, cmd)], cwd=work,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise RuntimeError(f'{cmd[0]} failed:\n{result.stdout}')
    data = bytearray(bsp.read_bytes())
    offset, length = struct.unpack_from('<ii', data, 4)
    entities = re.findall(rb'\{[^{}]*\}', bytes(data[offset:offset + length]))
    panes = [e for e in entities if b'"classname" "prop_dynamic"' in e]
    if len(panes) != 2:
        raise RuntimeError(f'expected two compiled pane entities, found {len(panes)}')
    rest = [e for e in entities if e not in panes]
    for name, selected in (('none', []), ('front', panes[:1]), ('back', panes[1:]),
                           ('both', panes), ('reverse', list(reversed(panes)))):
        modified = bytearray(data)
        lump = b'\n'.join(rest + selected) + b'\n\0'
        struct.pack_into('<ii', modified, 4, len(modified), len(lump))
        modified.extend(lump)
        (maps / ('overlap_' + name + '.bsp')).write_bytes(modified)


def difference(a, b):
    """Boolean mask for visible per-pixel RGB changes, excluding the HUD."""
    delta = ImageChops.difference(a, b)
    r, g, b = delta.split()
    return ImageChops.lighter(ImageChops.lighter(r, g), b).point(lambda n: 255 if n >= 6 else 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--backend', choices=('software', 'hardware'), default='software')
    parser.add_argument('--msaa', type=int, choices=(0, 4), default=0)
    args = parser.parse_args()
    args.binary = args.binary.resolve(); args.width, args.height, args.game = 640, 360, None
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-overlap-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    make_maps(args.binary, work)
    boot = bench.BOOT.replace('vid_fsaa 0', f'vid_fsaa {args.msaa}').replace(
        'map start', 'r_radiance 1\nr_radiance_hdr 1\nmap overlap_none')
    (work / 'id1/boot.cfg').write_text(boot)
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda t: 'CL_SignonReply: 4' in t)
        backend = 1 if args.backend == 'software' else 2
        active_backend = 'software' if args.backend == 'software' else 'ray-query'
        engine.cmd(f'r_radiance_backend {backend}\nr_radiance 1\nr_radiance_hdr 1\n'
                   'r_radiance_quality 0\nr_radiance_bounce_gain 0\nr_drawviewmodel 0\ncrosshair 0\nr_fullbright 0\n'
                   'scr_showfps 0\nviewsize 100\nhost_maxfps 30\nwait\nwait')
        images = {}
        for mode in (1, 2):
            engine.cmd(f'r_oit {mode}\nwait\nwait')
            for name in ('none', 'front', 'back', 'both', 'reverse'):
                engine.view('overlap_' + name, '-80 0 48 0 0 0')
                status = engine.cmd('radiance_stats')
                if f'Radiance: requested=1 active=1 backend={active_backend}' not in status:
                    raise RuntimeError(f'Radiance unavailable: {status}')
                time.sleep(0.4)
                path = engine.photo(f'oit{mode}-{name}')
                with Image.open(path) as image:
                    images[mode, name] = image.convert('RGB').crop((0, 0, 640, 300))
                # engine.view pauses; map change retains pause on some engines.
                engine.cmd('pause\nwait')
            none, front, back, both, reverse = (images[mode, n] for n in ('none', 'front', 'back', 'both', 'reverse'))
            frontmask, backmask = difference(none, front), difference(none, back)
            overlap = ImageChops.darker(frontmask, backmask)
            count = overlap.histogram()[255]
            if count < 500:
                raise RuntimeError(f'OIT {mode}: layers do not both visibly cover 500 common pixels ({count}); inspect {work}')
            # Both layers must visibly alter the overlapping pixels relative to
            # either single-layer capture; catches accidentally discarded layers.
            for name, single in (('front', front), ('back', back)):
                changed = ImageChops.darker(overlap, difference(single, both)).histogram()[255]
                if changed < 500:
                    raise RuntimeError(f'OIT {mode}: second layer missing vs {name} ({changed} pixels); inspect {work}')
            changed = difference(both, reverse).histogram()[255]
            if changed > 640 * 300 // 100:
                raise RuntimeError(f'OIT {mode}: reversing entities changed {changed} pixels; inspect {work}')
            print(f'OIT {mode}: {count} pixels covered by both layers; {changed} differ after entity reorder', flush=True)
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
    print(f'PASS: HDR overlapping transparent brush entities in OIT modes 1 and 2; captures in {work}', flush=True)


if __name__ == '__main__': main()
