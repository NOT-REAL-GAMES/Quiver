#!/usr/bin/env python3
"""Compile sector props and inspect the world BSP and moving submodels."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def run(args):
    proc = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode:
        raise AssertionError(f'{args!r} failed ({proc.returncode}):\n{proc.stdout}')
    return proc.stdout


def two_texture_wad(source, target):
    data = source.read_bytes()
    magic, count, directory = struct.unpack_from('<4sii', data)
    assert magic == b'WAD2' and count >= 1
    offset, size, raw_size, kind, compression, pad, _ = struct.unpack_from('<iiiBBH16s', data, directory)
    first = bytearray(data[offset:offset + size]); second = bytearray(first)
    first[:16] = b'stone1\0'.ljust(16, b'\0'); second[:16] = b'stone2\0'.ljust(16, b'\0')
    end = 12 + 2 * size
    entries = b''.join(struct.pack('<iiiBBH16s', pos, size, raw_size, kind, compression, pad, name)
                       for pos, name in ((12, b'stone1'), (12 + size, b'stone2')))
    target.write_bytes(struct.pack('<4sii', b'WAD2', 2, end) + first + second + entries)


class BSP:
    def __init__(self, path):
        self.data = path.read_bytes(); d = self.data
        assert d[:4] == b'BSP2' and len(d) >= 124
        self.lumps = [struct.unpack_from('<ii', d, 4 + i * 8) for i in range(15)]
        assert all(o >= 0 and n >= 0 and o + n <= len(d) for o, n in self.lumps)
        assert self.lumps[1][1] % 20 == 0 and self.lumps[5][1] % 44 == 0
        assert self.lumps[9][1] % 12 == 0 and self.lumps[10][1] % 44 == 0
        assert self.lumps[14][1] % 64 == 0

    def entities(self):
        offset, size = self.lumps[0]
        body = self.data[offset:offset + size].decode('ascii').rstrip('\0')
        return [dict(re.findall(r'"([^"\n]*)"\s*"([^"\n]*)"', block))
                for block in re.findall(r'\{([^{}]*)\}', body)]

    def contents(self, model, point, hull=0):
        d = self.data; models, size = self.lumps[14]
        assert 0 <= model < size // 64 and hull in (0, 1)
        node = struct.unpack_from('<i', d, models + 64 * model + 36 + hull * 4)[0]
        planes, np = self.lumps[1]; index, length = self.lumps[5 if hull == 0 else 9]
        stride = 44 if hull == 0 else 12
        for _ in range(length // stride + 1):
            if node < 0:
                if hull: return node
                leaves, nleaves = self.lumps[10]; leaf = -node - 1
                assert leaf * 44 + 44 <= nleaves
                return struct.unpack_from('<i', d, leaves + leaf * 44)[0]
            assert node * stride + stride <= length
            plane, front, back = struct.unpack_from('<iii', d, index + node * stride)
            assert 0 <= plane < np // 20
            x, y, z, dist, _ = struct.unpack_from('<ffffi', d, planes + plane * 20)
            node = front if x * point[0] + y * point[1] + z * point[2] >= dist else back
        raise AssertionError('cycle in BSP node tree')

    def textures_for_model(self, model, height=None):
        d = self.data; models, _ = self.lumps[14]; faces, face_len = self.lumps[7]
        texinfo, tex_len = self.lumps[6]; textures, texture_len = self.lumps[2]; planes, plane_len = self.lumps[1]
        first, count = struct.unpack_from('<ii', d, models + model * 64 + 56)
        names = set()
        for face in range(first, first + count):
            assert face * 28 + 28 <= face_len
            if height is not None:
                plane = struct.unpack_from('<i', d, faces + face * 28)[0]
                assert 0 <= plane < plane_len // 20
                x, y, z, dist, _ = struct.unpack_from('<ffffi', d, planes + plane * 20)
                if abs(z) < 0.9999 or abs(dist / z - height) > 1e-4:
                    continue
            ti = struct.unpack_from('<i', d, faces + face * 28 + 16)[0]
            assert 0 <= ti < tex_len // 40
            mip = struct.unpack_from('<i', d, texinfo + ti * 40 + 32)[0]
            total = struct.unpack_from('<i', d, textures)[0]
            assert 0 <= mip < total
            offset = struct.unpack_from('<i', d, textures + 4 + 4 * mip)[0]
            assert offset >= 0 and offset + 16 <= texture_len
            names.add(d[textures + offset:textures + offset + 16].split(b'\0')[0].decode('ascii'))
        return names

    def world_nodes(self):
        """Count structural nodes reachable from the world's render hull."""
        d = self.data; models, _ = self.lumps[14]; nodes, length = self.lumps[5]
        pending = [struct.unpack_from('<i', d, models + 36)[0]]; seen = set()
        while pending:
            node = pending.pop()
            if node < 0 or node in seen: continue
            assert node * 44 + 44 <= length
            seen.add(node)
            _, front, back = struct.unpack_from('<iii', d, nodes + node * 44)
            pending.extend((front, back))
        return len(seen)

    def world_leaf(self, point):
        d = self.data; models, _ = self.lumps[14]; nodes, length = self.lumps[5]; planes, nplanes = self.lumps[1]
        node = struct.unpack_from('<i', d, models + 36)[0]
        for _ in range(length // 44 + 1):
            if node < 0: return -node - 1
            assert node * 44 + 44 <= length
            plane, front, back = struct.unpack_from('<iii', d, nodes + node * 44)
            assert 0 <= plane < nplanes // 20
            x, y, z, dist, _ = struct.unpack_from('<ffffi', d, planes + plane * 20)
            node = front if x * point[0] + y * point[1] + z * point[2] >= dist else back
        raise AssertionError('cycle in world BSP')

    def pvs_visible(self, source, target):
        d = self.data; models, _ = self.lumps[14]; leaves, nleaves = self.lumps[10]; vis, nvis = self.lumps[4]
        first, second = self.world_leaf(source), self.world_leaf(target)
        assert 0 < first < nleaves // 44 and 0 < second < nleaves // 44
        offset = struct.unpack_from('<i', d, leaves + first * 44 + 4)[0]
        assert 0 <= offset < nvis, 'visibility data missing for world leaf'
        bits = (struct.unpack_from('<i', d, models + 52)[0] + 7) // 8
        row = bytearray(); cursor = offset
        while len(row) < bits:
            assert cursor < nvis
            byte = d[vis + cursor]; cursor += 1
            if byte:
                row.append(byte)
            else:
                assert cursor < nvis and d[vis + cursor] > 0
                row.extend(b'\0' * d[vis + cursor]); cursor += 1
        return bool(row[(second - 1) // 8] & (1 << ((second - 1) % 8)))


def main():
    engine, fixture, wad = (Path(arg).resolve() for arg in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix='quiver-sector-props-') as tmp:
        root = Path(tmp); two_wad = root / 'props.wad'; two_texture_wad(wad, two_wad)
        source, bsp = root / 'props.map', root / 'props.bsp'
        run([fixture, source, two_wad])
        qbsp = run([engine, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest', source, bsp])
        assert 'Texture stone2 not found' not in qbsp
        for tool in ('vis', 'light'):
            run([engine, '--quiver-map-tool', tool, '-threads', '2', bsp])
        compiled = BSP(bsp); entities = compiled.entities()
        props = [entity for entity in entities if entity.get('classname', '').startswith('prop_')]
        assert len(props) == 2 and all(e['classname'] != 'prop_static' for e in entities)
        expected = {'dynamic': ('prop_dynamic', (64., -96., 48.)), 'movable': ('prop_movable', (-80., 80., 48.))}
        indices = {}
        for name, (classname, origin) in expected.items():
            matches = [e for e in props if e['classname'] == classname and tuple(map(float, e['origin'].split())) == origin]
            assert len(matches) == 1
            entity = matches[0]
            assert re.fullmatch(r'\*[1-9][0-9]*', entity.get('model', ''))
            indices[name] = int(entity['model'][1:])
        assert len(set(indices.values())) == 2
        dynamic = next(e for e in props if e['classname'] == 'prop_dynamic')
        assert tuple(map(float, dynamic['velocity'].split())) == (12., -8., 4.)
        empty, solid = -1, -2
        for point in ((0, 0, 32), (64, -96, 48), (-112, 104, 48)):
            assert compiled.contents(0, point) == empty, f'moving prop became part of world model: {point}'
        assert compiled.contents(0, (260, 0, 48)) == solid, 'room wall missing'
        for point in ((-128, -96, 48), (128, 64, 48)):
            assert compiled.contents(0, point) == solid, f'static prop missing from world BSP: {point}'
            assert compiled.contents(0, point, 1) == solid, f'static prop missing from world collision: {point}'
        assert compiled.contents(0, (97, 33, 17)) == empty, 'sloped static prop corner was filled'
        model = indices['dynamic']
        assert compiled.contents(model, (0, 0, 0)) == solid
        assert compiled.contents(model, (48, 0, 0)) == empty
        assert compiled.contents(model, (0, 0, 0), 1) == solid
        model = indices['movable']
        for point in ((-32, 24, 0), (32, -32, 0)):
            assert compiled.contents(model, point) == solid, f'concave prop loses solid area: {point}'
        assert compiled.contents(model, (32, 24, 0)) == empty, 'concave notch was filled'
        assert compiled.contents(model, (32, 24, 0), 1) == empty, 'concave notch collision was filled'
        assert {'stone1', 'stone2'} <= compiled.textures_for_model(model), 'concave prop material boundary lost'
        assert compiled.textures_for_model(model, 32) == {'stone2'}, 'top face material changed'
        assert compiled.textures_for_model(model, -32) == {'stone1'}, 'bottom face material changed'
        assert compiled.lumps[4][1] > 0 and compiled.lumps[8][1] > 0, 'VIS/LIGHT missing'
        # Recompile exactly the same geometry with the two static props as
        # dynamic entities. The extra nodes in the baked version demonstrate
        # that their surfaces participate in structural world BSP/visibility.
        control_source, control_bsp = root / 'control.map', root / 'control.bsp'
        run([fixture, control_source, two_wad, 'compare-dynamic'])
        run([engine, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest', control_source, control_bsp])
        control = BSP(control_bsp)
        assert control.contents(0, (-128, -96, 48)) == empty
        assert compiled.world_nodes() > control.world_nodes(), (compiled.world_nodes(), control.world_nodes())
        # A full-height static divider with a gap creates separate visibility
        # leaves and occludes the far side. An identical dynamic brush remains
        # a submodel and cannot contribute to the baked PVS.
        visibility = {}
        for mode in ('vis-static', 'vis-dynamic', 'vis-nocollision'):
            src, target = root / (mode + '.map'), root / (mode + '.bsp')
            run([fixture, src, two_wad, mode])
            run([engine, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest', src, target])
            run([engine, '--quiver-map-tool', 'vis', '-threads', '2', target])
            visibility[mode] = BSP(target)
        left, right, around_gap = (-128, -128, 64), (128, -128, 64), (128, 200, 64)
        baked, moving = visibility['vis-static'], visibility['vis-dynamic']
        assert baked.contents(0, (0, -128, 64)) == solid
        assert moving.contents(0, (0, -128, 64)) == empty
        assert not baked.pvs_visible(left, right) and not baked.pvs_visible(right, left), 'static wall did not occlude PVS'
        assert baked.pvs_visible(left, around_gap), 'static wall incorrectly blocked the gap'
        assert moving.pvs_visible(left, right), 'dynamic control unexpectedly occluded PVS'
        noncolliding = visibility['vis-nocollision']
        inside = (0, -128, 64)
        assert noncolliding.contents(0, inside) == empty and noncolliding.contents(0, inside, 1) == empty
        assert all(e.get('classname') != 'func_detail_illusionary' for e in noncolliding.entities())
        assert noncolliding.lumps[14][1] == baked.lumps[14][1], 'non-solid prop became a submodel'
        assert noncolliding.textures_for_model(0) >= {'stone1'}
        assert noncolliding.pvs_visible(inside, left) and noncolliding.pvs_visible(inside, right), 'visibility lost inside non-solid prop'
        models, _ = noncolliding.lumps[14]
        moving_models, _ = moving.lumps[14]
        assert struct.unpack_from('<i', noncolliding.data, models + 60)[0] > struct.unpack_from('<i', moving.data, moving_models + 60)[0], 'non-solid prop surfaces missing from world'
    print('sector props: static world BSP/occlusion/collision, dynamic submodels, materials and concave geometry OK')


if __name__ == '__main__': main()
