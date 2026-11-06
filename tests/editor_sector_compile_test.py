#!/usr/bin/env python3
"""Compile sloped stacked sectors with a traversable nonvertical portal."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def read_lumps(data):
    if len(data) < 124 or data[:4] != b'BSP2':
        raise AssertionError('not a float-bounds BSP2')
    lumps = [struct.unpack_from('<ii', data, 4 + 8 * i) for i in range(15)]
    if any(offset < 0 or size < 0 or offset + size > len(data) for offset, size in lumps):
        raise AssertionError('invalid lump bounds')
    return lumps


def contents_hull1(data, lumps, xyz):
    planes, planes_len = lumps[1]
    clips, clips_len = lumps[9]
    models, models_len = lumps[14]
    if models_len < 64 or planes_len % 20 or clips_len % 12:
        raise AssertionError('invalid BSP model or collision lumps')
    node = struct.unpack_from('<i', data, models + 40)[0]
    for _ in range(clips_len // 12 + 1):
        if node < 0:
            return node
        if node * 12 + 12 > clips_len:
            raise AssertionError('invalid clipnode')
        plane, front, back = struct.unpack_from('<iii', data, clips + node * 12)
        if plane < 0 or plane * 20 + 20 > planes_len:
            raise AssertionError('invalid plane')
        x, y, z, distance, _ = struct.unpack_from('<ffffi', data, planes + plane * 20)
        node = front if x * xyz[0] + y * xyz[1] + z * xyz[2] - distance >= 0 else back
    raise AssertionError('cyclic BSP clipnodes')


def main():
    test, exe, wad = [Path(arg).resolve() for arg in sys.argv[1:]]
    with tempfile.TemporaryDirectory(prefix='quiver 3d sectors ') as tmp:
        folder = Path(tmp)
        for fixture in ('sloped', 'concave', 'far'):
            source, bsp = folder / f'{fixture} stacked.map', folder / f'{fixture} stacked.bsp'
            create = [str(test), str(source), str(wad)] + ([fixture] if fixture in ('concave', 'far') else [])
            subprocess.run(create, check=True, capture_output=True)
            for args in (('qbsp', '-bsp2', '-nopercent', '-leaktest', source, bsp),
                         ('vis', '-threads', '2', bsp), ('light', '-threads', '2', bsp)):
                result = subprocess.run([str(exe), '--quiver-map-tool', *map(str, args)], cwd=folder,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                if result.returncode:
                    raise AssertionError(f'{fixture} {args[0]} failed ({result.returncode}):\n{result.stdout}')
            data = bsp.read_bytes()
            lumps = read_lumps(data)
            for index, name in ((4, 'visibility'), (8, 'lighting')):
                offset, size = lumps[index]
                if not size or not any(data[offset:offset + size]):
                    raise AssertionError(f'{fixture} {name} was not populated')
            # Hull 1 expands brushes to player dimensions. Check the complete
            # path across each portal, not just each room's origin.
            x, y = (100064, 64) if fixture == 'far' else (32, 32)
            zs = range(32, 93, 4) if fixture == 'far' else range(64, 217, 4)
            for z in zs:
                contents = contents_hull1(data, lumps, (x, y, z))
                if contents != -1:
                    raise AssertionError(f'{fixture} player hull blocked at z={z} (contents {contents})')
    print('sloped, concave and far-coordinate BSP2 sectors: QBSP/VIS/LIGHT and player-hull traversal OK')


if __name__ == '__main__':
    main()
