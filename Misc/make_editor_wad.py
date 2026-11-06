#!/usr/bin/env python3
"""Generate the small original placeholder WAD used by new editor maps."""
import struct
import sys
from pathlib import Path


def texture(name):
    size = 32
    levels = []
    for level in range(4):
        side = size >> level
        pixels = bytearray()
        for y in range(side):
            for x in range(side):
                # A deterministic stone-brick pattern using Quake palette indices.
                xx, yy = x << level, y << level
                mortar = yy % 8 < 2 or (xx + (8 if (yy // 8) % 2 else 0)) % 16 < 2
                pixels.append(17 if mortar else 24 + ((xx * 7 + yy * 11) % 8))
        levels.append(pixels)
    offsets = []
    offset = 40
    for level in levels:
        offsets.append(offset)
        offset += len(level)
    return struct.pack('<16s6I', name.encode('ascii'), size, size, *offsets) + b''.join(levels)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: make_editor_wad.py OUTPUT.wad')
    lump = texture('stone1')
    lump_offset = 12
    directory_offset = lump_offset + len(lump)
    directory = struct.pack('<iiiBBH16s', lump_offset, len(lump), len(lump), 68, 0, 0, b'stone1')
    Path(sys.argv[1]).write_bytes(struct.pack('<4sii', b'WAD2', 1, directory_offset) + lump + directory)


if __name__ == '__main__':
    main()
