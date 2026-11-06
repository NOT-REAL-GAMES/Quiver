#!/usr/bin/env python3
"""Optional Radiance start-map spotlight A/B using a user-supplied Quake PAK.

The installed start map must contain the two specific compiler-authored lights
used by this fixture. Only an external entity override in a temporary game
directory is changed; the user's PAK and configuration are never modified.
This is a diagnostic for source attribution, not an image-quality benchmark.
"""
import argparse
from pathlib import Path
import re
import struct
import tempfile
import time

from PIL import Image
import bench_meshlets as bench


ORIGINS = {'392 704 96', '696 704 96'}


def bsp_entities(paks):
    # Later numbered PAKs have precedence. Validate all offsets before use.
    for pak in reversed(paks):
        data = pak.read_bytes()
        if len(data) < 12:
            continue
        magic, directory, length = struct.unpack_from('<4sii', data)
        if magic != b'PACK' or directory < 0 or length < 0 or length % 64 or directory + length > len(data):
            raise ValueError(f'invalid PAK directory: {pak}')
        for offset in range(directory, directory + length, 64):
            name, start, size = struct.unpack_from('<56sii', data, offset)
            if name.split(b'\0', 1)[0].lower() != b'maps/start.bsp':
                continue
            if start < 0 or size < 12 or start + size > len(data):
                raise ValueError('invalid start.bsp PAK entry')
            bsp = data[start:start + size]
            version, entoff, entsize = struct.unpack_from('<iii', bsp)
            if version != 29 or entoff < 0 or entsize < 1 or entoff + entsize > len(bsp):
                raise ValueError('fixture requires a version 29 start.bsp with valid entity data')
            return bsp[entoff:entoff + entsize].rstrip(b'\0').decode('latin1')
    raise ValueError('no maps/start.bsp found in supplied PAKs')


def remove_spotlights(entities):
    parts = re.split(r'(\{[^}]*\})', entities)
    found = set()
    for index, part in enumerate(parts):
        if not part.startswith('{'):
            continue
        fields = dict(re.findall(r'"([^"\n]*)"\s*"([^"\n]*)"', part))
        origin = fields.get('origin', '')
        if (fields.get('classname') == 'light' and origin in ORIGINS and
                fields.get('light') == '4000' and fields.get('delay') == '3' and
                fields.get('mangle') == '90 0 0'):
            if origin in found:
                raise ValueError(f'duplicate fixture light: {origin}')
            found.add(origin)
            parts[index] = ''
    if found != ORIGINS:
        raise ValueError('start.bsp does not have the expected pair of authored spotlight entities')
    return ''.join(parts)


def check_regions(on_path, off_path):
    with Image.open(on_path) as first, Image.open(off_path) as second:
        on, off = first.convert('RGB'), second.convert('RGB')
        if on.size != off.size:
            raise RuntimeError('captures have different sizes')
        width, height = on.size
        # Wide regions tolerate viewport aspect ratios and camera projection;
        # the two authored spotlights project on opposite sides of the doorway.
        for side, x0, x1 in [('left', .28, .49), ('right', .51, .72)]:
            box = (int(width*x0), int(height*.32), int(width*x1), int(height*.57))
            a, b = on.crop(box), off.crop(box)
            # A large positive red change distinguishes the warm, near-clipped
            # circles from incidental animated sprites and image noise.
            pixels_a = a.get_flattened_data() if hasattr(a, 'get_flattened_data') else a.getdata()
            pixels_b = b.get_flattened_data() if hasattr(b, 'get_flattened_data') else b.getdata()
            changed = sum(1 for x, y in zip(pixels_a, pixels_b)
                          if x[0] - y[0] >= 50 and x[0] >= 180)
            required = max(20, a.width*a.height//1000)
            if changed < required:
                raise RuntimeError(f'{side} spotlight region: only {changed} strongly brightened pixels (need {required})')
            print(f'{side} spotlight: {changed} strongly brightened pixels (minimum {required})', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True, help='directory containing id1 or the PAK files')
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height = 640, 360
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted((p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower())),
                  key=lambda p: int(re.search(r'\d+', p.name).group()))
    if not paks:
        parser.error('no Quake PAK files found')
    original = bsp_entities(paks)
    altered = remove_spotlights(original)
    work = Path(tempfile.mkdtemp(prefix='quiver-start-spots-'))
    print(f'Isolated test directory: {work}', flush=True)
    (work/'id1/maps').mkdir(parents=True)
    (work/'user').mkdir()
    for pak in paks:
        (work/'id1'/pak.name.lower()).symlink_to(pak)
    override = work/'id1/maps/start.ent'
    override.write_text(original, encoding='latin1')
    settings = ('host_maxfps 30\nr_radiance_backend 1\nr_radiance_quality 0\nr_radiance 1\n'
                'r_radiance_point_gain 7\nr_radiance_emissive_gain 0\nr_radiance_bounce_gain 0\n'
                'fov 90\nmap start')
    (work/'id1/boot.cfg').write_text(bench.BOOT.replace('map start', settings))
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.view('start', '544 288 32 0 90 0')
        time.sleep(2)
        state = engine.cmd('radiance_stats')
        if not re.search(r'Radiance: requested=1 active=1 backend=software\b', state):
            raise RuntimeError(f'software Radiance not active: {state}')
        on = engine.photo('spots-on')
        override.write_text(altered, encoding='latin1')
        engine.view('start', '544 288 32 0 90 0')
        time.sleep(2)
        state = engine.cmd('radiance_stats')
        if not re.search(r'Radiance: requested=1 active=1 backend=software\b', state):
            raise RuntimeError(f'software Radiance not active after reload: {state}')
        off = engine.photo('spots-off')
        check_regions(on, off)
        if args.validation:
            output = engine.text() + (work/'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in output or re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', output, re.I):
                raise RuntimeError(f'validation missing or reported errors; inspect {work}')
        print(f'PASS: authored spotlights identified; captures: {on}, {off}', flush=True)
    finally:
        engine.close()


if __name__ == '__main__':
    main()
