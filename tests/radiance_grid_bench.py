#!/usr/bin/env python3
"""Optional read-only grid benchmark against BSP light entities in a PAK.

Build a shared library first, e.g.:
  cc -O3 -shared -fPIC -std=c11 -IQuake Quake/radiance_lights.c -lm -o /tmp/radiance-grid.so
  python3 tests/radiance_grid_bench.py /tmp/radiance-grid.so /path/to/id1/pak0.pak
This measures candidate counts and CPU build time, not actual GPU frame cost.
"""
import ctypes as c
import math
import re
import statistics
import struct
import sys
import time
from pathlib import Path

F4 = c.c_float * 4
U4 = c.c_uint32 * 4


class Light(c.Structure):
    _fields_ = [('position', F4), ('color', F4), ('direction', F4), ('metadata', U4), ('area', F4)]


class ParseStats(c.Structure):
    _fields_ = [(x, c.c_size_t) for x in ('entities', 'found', 'written', 'dropped', 'malformed', 'unresolved')] + [
        ('upper', F4), ('lower', F4)]


class Grid(c.Structure):
    _fields_ = [('origin', c.c_float * 3), ('cell_size', c.c_float * 3), ('dimensions', c.c_uint32 * 3)]


class GridStats(c.Structure):
    _fields_ = [('cell_count', c.c_size_t), ('required_indices', c.c_size_t), ('indexed_lights', c.c_size_t)]


def read_lump(path, name):
    data = Path(path).read_bytes()
    magic, offset, size = struct.unpack_from('<4sii', data)
    if magic != b'PACK' or size % 64:
        raise ValueError('invalid PAK')
    for index in range(offset, offset + size, 64):
        raw, start, length = struct.unpack_from('<56sii', data, index)
        if raw.split(b'\0', 1)[0].decode() == name:
            bsp = data[start:start + length]
            version, ent_start, ent_length = struct.unpack_from('<iii', bsp)
            if version != 29:
                raise ValueError('unexpected BSP version')
            return bsp[ent_start:ent_start + ent_length]
    raise ValueError(f'{name} not in PAK')


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    lib = c.CDLL(str(Path(sys.argv[1]).resolve()))
    lib.Radiance_ParseMapLights.argtypes = [c.c_char_p, c.c_size_t, c.POINTER(Light), c.c_size_t, c.POINTER(ParseStats)]
    lib.Radiance_ParseMapLights.restype = c.c_bool
    lib.Radiance_BuildLightGrid.argtypes = [c.POINTER(Grid), c.POINTER(Light), c.c_size_t, c.POINTER(c.c_uint32),
                                           c.c_size_t, c.POINTER(c.c_uint32), c.c_size_t, c.POINTER(GridStats)]
    lib.Radiance_BuildLightGrid.restype = c.c_bool
    assert c.sizeof(Light) == 80
    for mapname in ('start', 'e1m1', 'e1m2'):
        text = read_lump(sys.argv[2], f'maps/{mapname}.bsp')
        lights = (Light * 16384)()
        info = ParseStats()
        assert lib.Radiance_ParseMapLights(text, len(text), lights, len(lights), c.byref(info)) and not info.dropped
        entities = re.findall(rb'\{([^}]*)\}', text)
        spawn = (0., 0., 0.)
        for entity in entities:
            fields = dict(re.findall(rb'"([^"]*)"\s*"([^"]*)"', entity))
            if fields.get(b'classname') == b'info_player_start' and b'origin' in fields:
                spawn = tuple(float(v) for v in fields[b'origin'].split())
                break
        for side, step in ((16, 512), (32, 256), (32, 128)):
            origin = [math.floor(spawn[i] / step) * step - (side // 2) * step for i in range(3)]
            grid = Grid((c.c_float * 3)(*origin), (c.c_float * 3)(step, step, step), (c.c_uint32 * 3)(side, side, side))
            count = side ** 3
            offsets = (c.c_uint32 * (count + 1))()
            indices = (c.c_uint32 * (count * max(info.written, 1)))()
            stats = GridStats()
            times = []
            for _ in range(6):
                started = time.perf_counter_ns()
                assert lib.Radiance_BuildLightGrid(c.byref(grid), lights, info.written, offsets, len(offsets), indices,
                                                  len(indices), c.byref(stats))
                times.append((time.perf_counter_ns() - started) / 1e6)
            x, y, z = (int((spawn[i] - origin[i]) / step) for i in range(3))
            center = (z * side + y) * side + x
            print(f'{mapname} {info.written} lights {side}^3/{step}: entries={stats.required_indices} '
                  f'spawn={offsets[center+1]-offsets[center]} build_median_ms={statistics.median(times[1:]):.3f}')


if __name__ == '__main__':
    main()
