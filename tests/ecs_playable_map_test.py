#!/usr/bin/env python3
"""Validate the packaged stress map's spawn, embedded assets and central path."""
from pathlib import Path
import struct

from editor_sector_compile_test import contents_hull1, read_lumps

ROOT = Path(__file__).resolve().parents[1]
def check_map(count):
    data = (ROOT / f'Misc/vq_pak/maps/ecs{count}.bsp').read_bytes()
    lumps = read_lumps(data)
    offset, size = lumps[0]
    entities = data[offset:offset + size]
    for marker in (b'worldspawn', b'info_player_start', b'light', f'ECS Stress: {count} entities'.encode()):
        assert marker in entities, f'missing map entity data: {marker!r}'
    assert f'"_quiver_ecs_stress" "{count}"'.encode() in entities, 'missing marker for editor playtest copies'
    for index, name in ((4, 'visibility'), (8, 'lighting')):
        offset, size = lumps[index]
        assert size and any(data[offset:offset + size]), f'missing {name}'
    texoffset, texsize = lumps[2]
    assert texsize >= 48 and struct.unpack_from('<i', data, texoffset)[0] >= 1
    relative = struct.unpack_from('<i', data, texoffset + 4)[0]
    assert relative >= 0 and data[texoffset + relative:texoffset + relative + 6] == b'stone1'
    assert struct.unpack_from('<I', data, texoffset + relative + 24)[0] > 0, 'texture data not embedded'
    # The crouch-free player hull must be open at the start and along the
    # central route, including both small steps onto the platform.
    for point in ((-480, 0, 32), (-400, 0, 32), (-160, 0, 32),
                  (-96, 0, 48), (-32, 0, 64), (96, 0, 64), (320, 0, 32)):
        assert contents_hull1(data, lumps, point) == -1, f'blocked player hull at {point}'
    manifest = (ROOT / 'Misc/vq_pak/vq_pak_contents.txt').read_text().splitlines()
    assert f'maps/ecs{count}.bsp' in manifest, 'map is not packaged in the engine'
    return data, lumps


def main():
    big, a = check_map(32768)
    small, b = check_map(1024)
    for i in range(1, len(a)):
        offset, size = a[i]; other_offset, other_size = b[i]
        assert big[offset:offset + size] == small[other_offset:other_offset + other_size], f'non-entity BSP lump {i} differs'
    print('PASS: packaged stress maps share geometry and have texture, lighting, visibility, spawn and traversal')


if __name__ == '__main__':
    main()
