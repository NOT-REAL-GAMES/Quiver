#!/usr/bin/env python3
"""Compile a generated sealed room and check the BSP and embedded WAD texture."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def brush(x0, y0, z0, x1, y1, z1):
    return '''{{
( {x1} {y1} {z0} ) ( {x1} {y0} {z0} ) ( {x1} {y1} {z1} ) stone1 0 0 0 1 1
( {x0} {y0} {z0} ) ( {x0} {y0} {z1} ) ( {x1} {y0} {z0} ) stone1 0 0 0 1 1
( {x0} {y1} {z1} ) ( {x0} {y1} {z0} ) ( {x1} {y1} {z1} ) stone1 0 0 0 1 1
( {x0} {y0} {z1} ) ( {x0} {y1} {z1} ) ( {x1} {y0} {z1} ) stone1 0 0 0 1 1
( {x0} {y1} {z0} ) ( {x0} {y0} {z0} ) ( {x1} {y1} {z0} ) stone1 0 0 0 1 1
( {x0} {y1} {z1} ) ( {x0} {y0} {z1} ) ( {x0} {y1} {z0} ) stone1 0 0 0 1 1
}}
'''.format(**locals())


def main():
    compiler, wad = (Path(path).resolve() for path in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix='quiver qbsp test ') as temp:
        folder = Path(temp)
        wad_copy = folder / 'editor defaults.wad'
        wad_copy.write_bytes(wad.read_bytes())
        boxes = [(-144, -144, -16, 144, 144, 0), (-144, -144, 128, 144, 144, 144),
                 (-144, -144, 0, -128, 144, 128), (128, -144, 0, 144, 144, 128),
                 (-128, -144, 0, 128, -128, 128), (-128, 128, 0, 128, 144, 128)]
        source = folder / 'room source.map'
        result = folder / 'room output.bsp'
        source.write_text('{\n"classname" "worldspawn"\n"wad" "' + wad_copy.as_posix() + '"\n'
                          + ''.join(brush(*box) for box in boxes)
                          + '}\n{\n"classname" "info_player_start"\n"origin" "0 0 32"\n}\n')
        run = subprocess.run([str(compiler), '-nopercent', '-leaktest', str(source), str(result)],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if run.returncode:
            raise AssertionError('QBSP failed:\n' + run.stdout)
        data = result.read_bytes()
        if len(data) < 124 or struct.unpack_from('<i', data)[0] != 29:
            raise AssertionError('not a Quake BSP29')
        lumps = [struct.unpack_from('<ii', data, 4 + 8 * i) for i in range(15)]
        if any(offset < 0 or length < 0 or offset + length > len(data) for offset, length in lumps):
            raise AssertionError('BSP lump outside file')
        entoff, entlen = lumps[0]
        if b'info_player_start' not in data[entoff:entoff + entlen]:
            raise AssertionError('player start missing')
        texoff, texlen = lumps[2]
        if texlen < 48 or struct.unpack_from('<i', data, texoff)[0] < 1:
            raise AssertionError('missing texture lump')
        texrel = struct.unpack_from('<i', data, texoff + 4)[0]
        if texrel < 0 or data[texoff + texrel:texoff + texrel + 6] != b'stone1':
            raise AssertionError('stone1 was not embedded from fallback WAD')
        if not lumps[5][1] or not lumps[7][1] or not lumps[9][1] or not lumps[14][1]:
            raise AssertionError('missing BSP geometry or collision')
        leaky_source = folder / 'leaky.map'
        leaky_source.write_text('{\n"classname" "worldspawn"\n"wad" "' + wad_copy.as_posix() + '"\n'
                                + ''.join(brush(*box) for box in boxes[:-1])
                                + '}\n{\n"classname" "info_player_start"\n"origin" "0 0 32"\n}\n')
        leaky = subprocess.run([str(compiler), '-nopercent', '-leaktest', str(leaky_source), str(folder / 'leaky.bsp')],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if leaky.returncode == 0 or 'leak' not in leaky.stdout.lower():
            raise AssertionError('leaking map was not rejected by -leaktest')
        missing = subprocess.run([str(compiler), str(folder / 'missing.map'), str(folder / 'bad.bsp')],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if missing.returncode == 0:
            raise AssertionError('missing source was reported as success')


if __name__ == '__main__':
    main()
