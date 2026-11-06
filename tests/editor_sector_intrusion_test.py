#!/usr/bin/env python3
"""Verify an angled portal never grows solid walls into the adjacent room."""
from pathlib import Path
import struct
import sys
import tempfile

from editor_sector_portal_compile_test import collision_reader, run


def render_contents(path):
    data = path.read_bytes()
    assert data[:4] == b'BSP2'
    lumps = [struct.unpack_from('<ii', data, 4 + i * 8) for i in range(15)]
    planes, psize = lumps[1]
    nodes, nsize = lumps[5]
    leaves, lsize = lumps[10]
    model, msize = lumps[14]
    assert psize % 20 == 0 and nsize % 44 == 0 and lsize % 44 == 0 and msize >= 64
    root = struct.unpack_from('<i', data, model + 36)[0]

    def contents(point):
        node = root
        for _ in range(nsize // 44 + 1):
            if node < 0:
                leaf = -node - 1
                assert leaf * 44 + 44 <= lsize
                return struct.unpack_from('<i', data, leaves + leaf * 44)[0]
            assert node * 44 + 44 <= nsize
            plane, front, back = struct.unpack_from('<iii', data, nodes + node * 44)
            assert 0 <= plane < psize // 20
            x, y, z, dist, _ = struct.unpack_from('<ffffi', data, planes + plane * 20)
            node = front if x * point[0] + y * point[1] + z * point[2] >= dist else back
        raise AssertionError('cycle in render BSP')
    return contents


def main():
    engine, fixture, wad = (Path(arg).resolve() for arg in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix='quiver-slanted-portal-') as temp:
        for name, mode, offset in (('slanted', 'box', 0), ('concave-far', 'concave', 100000)):
            source = Path(temp) / (name + '.map')
            bsp = source.with_suffix('.bsp')
            run([fixture, source, wad, mode, str(offset)])  # Also checks generated brush geometry.
            run([engine, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest', source, bsp])
            render = render_contents(bsp)
            player, _ = collision_reader(bsp)
            def point(x, y, z): return (x + offset, y, z)
            assert render(point(-37.565944975, 252.21702751, 87.7085137)) == -1, f'{name}: render shell protrudes into room'
            assert player(point(-48, 230, 48)) == -1, f'{name}: shell protrusion blocks the player in room'
            for p in (point(0, 0, 100), point(0, 280, 48), point(0, 300, 64)):
                assert render(p) == -1 and player(p) == -1, f'{name}: sector interior obstructed: {p}'
            assert render(point(-64, 260, 64)) == -2 and player(point(-64, 260, 64)) == -2, f'{name}: wall around door is missing'
        print('slanted portal: generated brush, render BSP, and player hull stay out of rooms')


if __name__ == '__main__':
    main()
