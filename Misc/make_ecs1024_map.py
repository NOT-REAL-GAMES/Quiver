#!/usr/bin/env python3
"""Derive the 1024-entity demo from the shipped 32768-entity BSP.

Only worldspawn text changes: geometry, visibility and lighting remain byte
identical, making the two workloads comparable. The source map is kept in
maps/ecs1024.map for editing and normal rebuilding.
"""
from pathlib import Path
import struct


def main():
    here = Path(__file__).resolve().parent
    data = bytearray((here / 'vq_pak/maps/ecs32768.bsp').read_bytes())
    if len(data) < 124 or data[:4] != b'BSP2':
        raise ValueError('expected the packaged BSP2 stress map')
    offset, length = struct.unpack_from('<ii', data, 4)
    if offset < 0 or length <= 0 or offset + length > len(data):
        raise ValueError('invalid entity lump')
    entities = bytes(data[offset:offset + length])
    for old, new in ((b'ECS Stress: 32768 entities', b'ECS Stress: 1024 entities'),
                     (b'"_quiver_ecs_stress" "32768"', b'"_quiver_ecs_stress" "1024"')):
        if entities.count(old) != 1:
            raise ValueError(f'expected exactly one {old!r}')
        entities = entities.replace(old, new, 1)
    # Append a new lump instead of moving the other lumps, then point the
    # entity directory at it. The old lump is unreferenced padding.
    while len(data) % 4:
        data.append(0)
    struct.pack_into('<ii', data, 4, len(data), len(entities))
    data.extend(entities)
    (here / 'vq_pak/maps/ecs1024.bsp').write_bytes(data)


if __name__ == '__main__':
    main()
