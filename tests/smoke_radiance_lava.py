#!/usr/bin/env python3
"""Isolated stock start-map lava-emission regression (software Vulkan).

Requires an installed Quake PAK, X11 and a Vulkan device. Creates temporary
BSP variants; never edits the installed map or user configuration. Other
fullbright textures and point entities are removed from both variants. In the
second variant, only lava fullbright palette indices are also removed. The
camera and receiver are fixed, so wall-light differences come from lava. A
second pair of maps uses an identical external RGBA lava replacement at twice
the BSP texture dimensions to exercise recovery of its original emission mask.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import time

from PIL import Image, ImageChops
import bench_meshlets as bench


def extract_start(paks):
    for pak in reversed(paks):
        data = pak.read_bytes()
        if len(data) < 12:
            continue
        magic, directory, length = struct.unpack_from('<4sii', data)
        if magic != b'PACK' or directory < 0 or length < 0 or length % 64 or directory + length > len(data):
            raise ValueError(f'invalid PAK directory: {pak}')
        for offset in range(directory, directory + length, 64):
            name, start, size = struct.unpack_from('<56sii', data, offset)
            if name.split(b'\0', 1)[0].lower() == b'maps/start.bsp':
                if start < 0 or size < 124 or start + size > len(data):
                    raise ValueError('invalid start.bsp PAK entry')
                return data[start:start + size]
    raise ValueError('no maps/start.bsp found in supplied PAKs')


def make_variant(source, lava_emits, lava_alpha):
    data = bytearray(source)
    if len(data) < 124 or struct.unpack_from('<i', data)[0] != 29:
        raise ValueError('test requires a version 29 start.bsp')
    start, length = struct.unpack_from('<ii', data, 4 + 2 * 8)
    if start < 0 or length < 4 or start + length > len(data):
        raise ValueError('invalid BSP texture lump')
    count = struct.unpack_from('<i', data, start)[0]
    if count < 0 or 4 + 4 * count > length:
        raise ValueError('invalid miptex directory')
    removed_lava = 0
    for index in range(count):
        rel = struct.unpack_from('<i', data, start + 4 + 4 * index)[0]
        if rel < 0:
            continue
        if rel + 40 > length:
            raise ValueError('invalid miptex header')
        base = start + rel
        name, width, height, *mips = struct.unpack_from('<16s6I', data, base)
        name = name.split(b'\0', 1)[0].lower()
        lava = name.startswith(b'*lava')
        for level, mip in enumerate(mips):
            pixels = (width >> level) * (height >> level)
            if mip == 0:  # Texture payload may be supplied externally.
                continue
            if mip > length - rel or pixels > length - rel - mip:
                raise ValueError('invalid miptex pixels')
            if lava and lava_emits:
                continue
            first = base + mip
            for pos in range(first, first + pixels):
                if data[pos] >= 224:
                    removed_lava += int(lava)
                    data[pos] = 1
    if not lava_emits and not removed_lava:
        raise ValueError('start map has no fullbright lava pixels to isolate')
    # Eliminate point lights, dynamic entities and worldspawn sun/fog inputs.
    entities = (f'{{\n"classname" "worldspawn"\n"_lavaalpha" "{lava_alpha}"\n}}\n'
                '{\n"classname" "info_player_start"\n"origin" "1056 2224 96"\n"angle" "90"\n}\n\0').encode('ascii')
    struct.pack_into('<ii', data, 4, len(data), len(entities))
    data.extend(entities)
    return data


def lava_texture(source):
    start, length = struct.unpack_from('<ii', source, 4 + 2 * 8)
    count = struct.unpack_from('<i', source, start)[0]
    for index in range(count):
        rel = struct.unpack_from('<i', source, start + 4 + 4 * index)[0]
        if rel < 0 or rel + 40 > length:
            continue
        name, width, height, *_ = struct.unpack_from('<16s6I', source, start + rel)
        name = name.split(b'\0', 1)[0].lower()
        if name == b'*lava1':
            return name.decode('ascii'), width, height
    raise ValueError('start map has no *lava1 texture')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    args.binary = args.binary.resolve(); args.width, args.height, args.game = 320, 240, None
    args.stock_only = False
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted((p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower())),
                  key=lambda p: int(re.search(r'\d+', p.name).group()))
    if not paks: parser.error('no Quake PAK files found')
    original = extract_start(paks)
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-lava-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    # Keep the executable stable if the developer rebuilds the checkout while
    # the smoke test is running. Configuration is already under this temp dir.
    binary_copy = work / 'vkquake-test'
    shutil.copy2(args.binary, binary_copy)
    binary_copy.chmod(0o555)
    args.binary = binary_copy
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    for alpha in (1.0, 0.5):
        for name, enabled in (('lava_on', True), ('lava_off', False)):
            (work / 'id1/maps' / (name + '_' + str(alpha).replace('.', '_') + '.bsp')).write_bytes(
                make_variant(original, enabled, alpha))
    _, lava_width, lava_height = lava_texture(original)
    for name, enabled in (('lava_external_on', True), ('lava_external_off', False)):
        (work / 'id1/maps' / (name + '.bsp')).write_bytes(make_variant(original, enabled, 1.0))
        texture_dir = work / 'id1/textures' / name
        texture_dir.mkdir(parents=True)
        # Same replacement for both variants: only the original BSP mask differs.
        Image.new('RGBA', (lava_width * 2, lava_height * 2), (180, 45, 12, 255)).save(texture_dir / '#lava1.png')
    boot = ('r_radiance_backend 1\nr_radiance_quality 0\nr_radiance 1\n'
            'r_radiance_point_gain 0\nr_radiance_emissive_gain 1\nr_radiance_bounce_gain 0\n'
            'r_radiance_highlight 0\nr_dynamic 0\nr_flatlightstyles 1\n'
            'r_lavaalpha 1\nr_drawparticles 0\nfov 90\nmap lava_off_1_0')
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', boot))
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        transparent_reference = {}
        classic_reference = {}
        for alpha in (1.0, 0.5):
            images = {}
            engine.cmd(f'r_lavaalpha {alpha}\nr_radiance 1\nwait\nwait')
            for name in ('lava_off', 'lava_on'):
                engine.view(name + '_' + str(alpha).replace('.', '_'), '1544 2304 64 15 90 0')
                status = engine.cmd('radiance_stats')
                if not re.search(r'Radiance: requested=1 active=1 backend=software\b', status):
                    raise RuntimeError(f'software Radiance unavailable: {status}')
                emitters = re.search(r'lights=(\d+) emissive=(\d+)', status)
                if not emitters or (int(emitters.group(2)) == 0) != (name == 'lava_off'):
                    raise RuntimeError(f'unexpected emissive count for {name}: {status}')
                image = engine.photo(f'{name}-{alpha}')
                with Image.open(image) as shot:
                    if shot.size != (320, 240):
                        raise RuntimeError(f'expected 320x240 offscreen render, got {shot.size}')
                    images[name] = shot.convert('RGB')
                print(name, alpha, image, flush=True)
                if name == 'lava_on':
                    # Identify ordinary non-lava surfaces using classic lighting.
                    # Resume to ensure the renderer switches mode, then pause at
                    # the same camera for the reference capture.
                    engine.cmd('r_radiance 0\npause\nwait\nwait\nwait')
                    time.sleep(.3)
                    engine.cmd('pause\nwait')
                    reference = engine.photo(f'classic-{alpha}')
                    with Image.open(reference) as shot:
                        images['classic'] = shot.convert('RGB')
                engine.cmd('pause\nwait')
            transparent_reference[alpha] = images['lava_on']
            classic_reference[alpha] = images['classic']
            off, on, classic = (images[n] for n in ('lava_off', 'lava_on', 'classic'))
            pixels = lambda image: (image.get_flattened_data() if hasattr(image, 'get_flattened_data') else image.getdata())
            changed = 0
            for index, (before, after, ref) in enumerate(zip(pixels(off), pixels(on), pixels(classic))):
                x, y = index % 320, index // 320
                # Upper image only; require a visible, mostly neutral classic
                # wall/ceiling pixel, excluding red lava and the floor below.
                if y >= 140 or max(ref) < 32 or ref[0] > ref[1] * 1.45 + 12:
                    continue
                delta = [after[i] - before[i] for i in range(3)]
                changed += delta[0] >= 4 and delta[0] > delta[1] and delta[0] > delta[2]
            if changed < 100:
                raise RuntimeError(f'alpha {alpha}: only {changed} neutral wall pixels received red lava light; inspect {work}')
            print(f'PASS: alpha {alpha}: {changed} neutral wall pixels received red lava emission', flush=True)
        alpha_diff = ImageChops.difference(transparent_reference[1.0], transparent_reference[0.5]).crop((0, 140, 320, 240))
        alpha_pixels = alpha_diff.get_flattened_data() if hasattr(alpha_diff, 'get_flattened_data') else alpha_diff.getdata()
        if sum(max(pixel) > 4 for pixel in alpha_pixels) < 100:
            raise RuntimeError(f'opaque and translucent lava captures do not differ in the lava region: {work}')
        external = {}
        engine.cmd('r_lavaalpha 1\nr_radiance 1\nwait\nwait')
        for name in ('lava_external_off', 'lava_external_on'):
            engine.view(name, '1544 2304 64 15 90 0')
            listing = engine.cmd('imagelist')
            if not re.search(rf'\b{lava_width * 2}\s+x\s*{lava_height * 2}\s+textures/{name}/#lava1\b', listing):
                raise RuntimeError(f'external lava replacement was not loaded: {listing}')
            status = engine.cmd('radiance_stats')
            emitters = re.search(r'lights=(\d+) emissive=(\d+)', status)
            if not re.search(r'Radiance: requested=1 active=1 backend=software\b', status) or not emitters or \
                    (int(emitters.group(2)) == 0) != (name == 'lava_external_off'):
                raise RuntimeError(f'unexpected external-replacement emitters for {name}: {status}')
            image = engine.photo(name)
            with Image.open(image) as shot:
                external[name] = shot.convert('RGB')
            print(name, image, flush=True)
            engine.cmd('pause\nwait')
        changed = 0
        for index, (before, after, ref) in enumerate(zip(pixels(external['lava_external_off']),
                pixels(external['lava_external_on']), pixels(classic_reference[1.0]))):
            _, y = index % 320, index // 320
            if y >= 140 or max(ref) < 32 or ref[0] > ref[1] * 1.45 + 12:
                continue
            delta = [after[i] - before[i] for i in range(3)]
            changed += delta[0] >= 4 and delta[0] > delta[1] and delta[0] > delta[2]
        if changed < 100:
            raise RuntimeError(f'external lava: only {changed} neutral wall pixels received red light; inspect {work}')
        print(f'PASS: external {lava_width * 2}x{lava_height * 2} lava replacement: '
              f'{changed} neutral wall pixels received red emission', flush=True)
        if args.validation:
            output = engine.text() + (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in output or re.search(
                    r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output):
                raise RuntimeError(f'validation unavailable or reported errors; inspect {work}')
        print('PASS: opaque and translucent lava paths differ as expected', flush=True)
    finally:
        engine.close()


if __name__ == '__main__': main()
