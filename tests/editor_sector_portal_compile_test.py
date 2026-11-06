#!/usr/bin/env python3
"""Compile real partial sector portals, then inspect Quake player collision hulls."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def run(args, **kwargs):
    proc = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kwargs)
    if proc.returncode:
        raise AssertionError(f'{args!r} failed ({proc.returncode}):\n{proc.stdout}')
    return proc.stdout


def collision_reader(path):
    data = path.read_bytes()
    if len(data) < 124 or data[:4] != b'BSP2':
        raise AssertionError('not a Quake BSP2 map')
    lumps = [struct.unpack_from('<ii', data, 4 + i * 8) for i in range(15)]
    if any(offset < 0 or size < 0 or offset + size > len(data) for offset, size in lumps):
        raise AssertionError('invalid BSP lump bounds')
    planes, plane_bytes = lumps[1]
    nodes, node_bytes = lumps[9]
    model, model_bytes = lumps[14]
    if model_bytes < 64 or plane_bytes % 20 or node_bytes % 12:
        raise AssertionError('invalid BSP2 model or collision lumps')
    root = struct.unpack_from('<4i', data, model + 36)[1]  # player hull

    def contents(point):
        node = root
        for _ in range(node_bytes // 12 + 1):
            if node < 0: return node
            if node * 12 + 12 > node_bytes: raise AssertionError('clip node out of bounds')
            plane, front, back = struct.unpack_from('<iii', data, nodes + node * 12)
            if plane < 0 or plane * 20 + 20 > plane_bytes: raise AssertionError('plane out of bounds')
            x, y, z, dist, _ = struct.unpack_from('<ffffi', data, planes + plane * 20)
            node = front if x * point[0] + y * point[1] + z * point[2] >= dist else back
        raise AssertionError('cycle in collision hull')

    return contents, lumps


def check_large_clipnode_index(path):
    # A valid synthetic BSP2 whose player-hull root is above BSP29's signed
    # 16-bit limit. This guards the test itself against truncating indices.
    count = 32769
    header_size = 124
    planes = header_size
    nodes = planes + 20
    model = nodes + count * 12
    data = bytearray(model + 64)
    data[:4] = b'BSP2'
    for index, offset, length in ((1, planes, 20), (9, nodes, count * 12), (14, model, 64)):
        struct.pack_into('<ii', data, 4 + 8 * index, offset, length)
    struct.pack_into('<ffffi', data, planes, 1.0, 0.0, 0.0, 0.0, 0)
    struct.pack_into('<iii', data, nodes + 32768 * 12, 0, -1, -2)
    struct.pack_into('<i', data, model + 40, 32768)
    path.write_bytes(data)
    contents, _ = collision_reader(path)
    assert contents((1, 0, 0)) == -1 and contents((-1, 0, 0)) == -2, 'BSP2 clipnode index was truncated'


def main():
    engine, fixture, wad = (Path(arg).resolve() for arg in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix='quiver partial portal ') as tmp:
        root = Path(tmp)
        for mode in ('fixture', 'closed', 'stacked', 'triangulated'):
            source, bsp = root / f'{mode}.map', root / f'{mode}.bsp'
            run([fixture, '--write-' + mode, source, wad])
            run([engine, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest', source, bsp])
            contents, _ = collision_reader(bsp)
            empty, solid = -1, -2
            if mode in ('fixture', 'triangulated'):
                assert all(contents((x, 0, 32)) == empty for x in range(-128, 129, 8)), 'door blocks player hull'
                assert contents((0, 30, 32)) == contents((0, -30, 32)) == empty, 'door narrower than expected'
                assert contents((0, 34, 32)) == contents((0, -34, 32)) == solid, 'door surrounding wall missing'
                assert contents((0, 0, 100)) == solid, 'door lintel does not block player hull'
                if mode == 'triangulated':
                    for y in (-28, -14, 0, 14, 28):
                        for z in (32, 48, 64, 80):
                            assert contents((0, y, z)) == empty, f'triangulated wall blocked part of quad at {y},{z}'
            elif mode == 'closed':
                assert contents((0, 0, 32)) == solid, 'unlinked portal still open'
            else:
                assert all(contents((0, 0, z)) == empty for z in range(-80, 81, 8)), 'stacked opening blocks player hull'
                assert contents((20, 0, 0)) == contents((-20, 0, 0)) == solid, 'stacked floor missing beside opening'
            run([engine, '--quiver-map-tool', 'vis', '-threads', '2', bsp])
            run([engine, '--quiver-map-tool', 'light', '-threads', '2', bsp])
            _, lumps = collision_reader(bsp)
            if lumps[4][1] == 0 or lumps[8][1] == 0: raise AssertionError('missing VIS or LIGHT output')
        check_large_clipnode_index(root / 'large-index.bsp')
        print('partial, closed, stacked, and triangulated-wall portal collision with QBSP/VIS/LIGHT: OK')


if __name__ == '__main__': main()
