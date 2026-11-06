#!/usr/bin/env python3
"""Generate the original, tiny alias-model cube used by the ECS ripple demo."""
import struct
from pathlib import Path


def make_model():
    # MDL version 6; a 2-unit cube, one 8x8 indexed skin and one frame.
    vertices = [(x, y, z) for z in (0, 2) for y in (0, 2) for x in (0, 2)]
    triangles = [
        (0, 2, 3), (0, 3, 1), (4, 5, 7), (4, 7, 6),
        (0, 1, 5), (0, 5, 4), (2, 6, 7), (2, 7, 3),
        (0, 4, 6), (0, 6, 2), (1, 3, 7), (1, 7, 5),
    ]
    data = bytearray(struct.pack('<4si3f3ff3f8if', b'IDPO', 6,
                                 1., 1., 1., -1., -1., -1., 2.,
                                 0., 0., 0., 1, 8, 8, len(vertices),
                                 len(triangles), 1, 0, 0, 1.))
    data += struct.pack('<i', 0)  # single skin
    data += bytes(108 if (x + y) & 1 else 111 for y in range(8) for x in range(8))
    for x, y, z in vertices:
        data += struct.pack('<iii', 0, x * 3, y * 3)
    for a, b, c in triangles:
        # Quake alias models use clockwise winding when viewed from outside.
        data += struct.pack('<4i', 1, a, c, b)
    data += struct.pack('<i', 0)  # single frame
    data += bytes((0, 0, 0, 0, 2, 2, 2, 0))
    data += b'ripple\0'.ljust(16, b'\0')
    for x, y, z in vertices:
        data += bytes((x, y, z, 5))
    return data


if __name__ == '__main__':
    target = Path(__file__).parent / 'vq_pak/progs/quiver_ripple.mdl'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(make_model())
