#!/usr/bin/env python3
"""Verify the packed demo model is deterministic and has valid MDL indices."""
import runpy
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
model = root / 'Misc/vq_pak/progs/quiver_ripple.mdl'
data = model.read_bytes()
assert data == runpy.run_path(str(root / 'Misc/make_ripple_model.py'))['make_model']()
assert 'progs/quiver_ripple.mdl' in (root / 'Misc/vq_pak/vq_pak_contents.txt').read_text().splitlines()
header = struct.unpack_from('<4si3f3ff3f8if', data)
assert header[0:2] == (b'IDPO', 6)
numskins, skinwidth, skinheight, numverts, numtris, numframes = header[12:18]
assert (numskins, skinwidth, skinheight, numverts, numtris, numframes) == (1, 8, 8, 8, 12, 1)
triangles = 84 + 4 + skinwidth * skinheight + numverts * 12
frame_vertices = triangles + numtris * 16 + 4 + 24
vertices = [struct.unpack_from('<4B', data, frame_vertices + i * 4)[:3]
            for i in range(numverts)]
for i in range(numtris):
    front, a, b, c = struct.unpack_from('<4i', data, triangles + i * 16)
    assert front == 1 and len({a, b, c}) == 3 and all(0 <= n < numverts for n in (a, b, c))
    p, q, r = (vertices[n] for n in (a, b, c))
    u = tuple(q[j] - p[j] for j in range(3))
    v = tuple(r[j] - p[j] for j in range(3))
    cross = (u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0])
    # Quake MDL faces wind clockwise from outside, so their cross product
    # points toward the cube's center (1, 1, 1).
    assert sum(cross[j] * (p[j] - 1) for j in range(3)) < 0
assert len(data) == triangles + numtris * 16 + 4 + 24 + numverts * 4
print('PASS: packaged ripple model')
