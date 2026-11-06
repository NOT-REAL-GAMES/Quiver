#!/usr/bin/env python3
"""Run the embedded tools without engine assets and inspect their BSP output."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from qbsp_smoke import brush


def run(exe, cwd, *args, success=True):
    proc = subprocess.run([str(exe), '--quiver-map-tool', *map(str, args)], cwd=cwd,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if (proc.returncode == 0) != success:
        raise AssertionError(f'{args[0]} returned {proc.returncode}:\n{proc.stdout}')
    return proc.stdout


def main():
    exe, wad = map(lambda s: Path(s).resolve(), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='quiver embedded tools ') as temp:
        root = Path(temp)
        map_path = root / 'room with spaces.map'
        bsp_path = root / 'room with spaces.bsp'
        boxes = [(-144, -144, -16, 144, 144, 0), (-144, -144, 128, 144, 144, 144),
                 (-144, -144, 0, -128, 144, 128), (128, -144, 0, 144, 144, 128),
                 (-128, -144, 0, 128, -128, 128), (-128, 128, 0, 128, 144, 128),
                 # Divide the interior into two rooms connected by a doorway.
                 (-8, -128, 0, 8, -32, 128), (-8, 32, 0, 8, 128, 128)]
        map_path.write_text('{\n"classname" "worldspawn"\n"wad" "' + wad.as_posix() + '"\n'
                            + ''.join(brush(*box) for box in boxes)
                            + '}\n{\n"classname" "info_player_start"\n"origin" "0 0 32"\n}\n'
                            + '{\n"classname" "light"\n"origin" "0 0 80"\n"light" "400"\n}\n')
        run(exe, root, 'qbsp', '-nopercent', '-leaktest', map_path, bsp_path)
        portal_path = bsp_path.with_suffix('.prt')
        assert portal_path.is_file(), 'QBSP did not produce portal data'
        portal_header = portal_path.read_text().splitlines()
        assert portal_header[0] == 'PRT1' and int(portal_header[2]) > 0, 'fixture must exercise actual portal visibility'
        run(exe, root, 'vis', '-threads', '2', bsp_path)
        run(exe, root, 'light', '-threads', '2', bsp_path)
        data = bsp_path.read_bytes()
        assert len(data) >= 124 and struct.unpack_from('<i', data)[0] == 29
        lumps = [struct.unpack_from('<ii', data, 4 + 8 * i) for i in range(15)]
        assert all(off >= 0 and size >= 0 and off + size <= len(data) for off, size in lumps)
        visoff, vislen = lumps[4]
        lightoff, lightlen = lumps[8]
        assert vislen and data[visoff:visoff + vislen], 'VIS did not write visibility'
        assert lightlen and any(data[lightoff:lightoff + lightlen]), 'LIGHT did not write illuminated lightmaps'
        assert not (root / 'vis.log').exists() and not (root / 'light.log').exists()
        run(exe, root, 'vis', root / 'missing.bsp', success=False)
        run(exe, root, 'light', root / 'missing.bsp', success=False)
        print('embedded QBSP, VIS and LIGHT: OK')


if __name__ == '__main__':
    main()
