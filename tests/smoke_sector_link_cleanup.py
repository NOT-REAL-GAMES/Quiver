#!/usr/bin/env python3
"""Test automatic portal cleanup through the real sector editor and playtest."""
import argparse
import os
from pathlib import Path
import re
import shutil
import tempfile

import bench_meshlets as bench
import smoke_sector_editor as smoke
from editor_sector_portal_compile_test import collision_reader


# Add redo and a drag at the upper corner of the newly linked doorway.
shim = smoke.bench.SHIM
old = "} else if (action == 'z') {\n            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_Z;"
assert old in shim
shim = shim.replace(old, "} else if (action == 'z' || action == 'w') {\n            end = 4; sc = step == 1 || step == 4 ? SDL_SCANCODE_LCTRL : (action == 'z' ? SDL_SCANCODE_Z : SDL_SCANCODE_Y);")
old = "action == 'd' || action == 'm' || action == 'h' || action == 'j'"
assert old in shim
shim = shim.replace(old, old + " || action == 'q'")
old = "else if (action == 'm') { mx = cx - 416 - (step == 2 ? 32 : 0); my = cy - 286; }"
assert old in shim
shim = shim.replace(old, old + "\n            else if (action == 'q') { mx = cx + 208; my = cy - 78 - (step == 2 ? 26 : 0); }")
bench.SHIM = shim


def read_sector_source(path):
    lines = iter(path.read_text().splitlines())
    assert next(lines) == 'QSECTOR 1'
    nvertices = int(next(lines).split()[1])
    vertices = [tuple(map(float, next(lines).split())) for _ in range(nvertices)]
    nsectors = int(next(lines).split()[1]); sectors = []
    for _ in range(nsectors):
        nfaces = int(next(lines).split()[1]); faces = []
        for _ in range(nfaces):
            fields = next(lines).split()
            assert fields[0] == 'FACE'
            count, peer, peerface = map(int, fields[1:4])
            faces.append((peer, peerface, fields[4], tuple(map(int, fields[5:5 + count]))))
        sectors.append(faces)
    return vertices, sectors


def check_link(path, moved=False):
    vertices, sectors = read_sector_source(path)
    assert len(sectors) == 2
    portals = [[(i, face) for i, face in enumerate(sector) if face[0] >= 0] for sector in sectors]
    if not moved and any(len(p) != 1 for p in portals):
        raise RuntimeError(f'expected one cleaned portal on each room: {portals}')
    if not portals[0] or len(portals[0]) != len(portals[1]):
        raise RuntimeError('portal fragments are missing or unmatched')
    indices = set()
    for a, aface in portals[0]:
        if aface[0] != 1 or aface[1] < 0 or aface[1] >= len(sectors[1]):
            raise RuntimeError('portal references are invalid')
        bface = sectors[1][aface[1]]
        if bface[0:2] != (0, a) or set(aface[3]) != set(bface[3]):
            raise RuntimeError('portal fragments are not reciprocal or do not share vertex identities')
        indices.update(aface[3])
    coords = {vertices[i] for i in indices}
    expected = {(256., -128., 0.), (256., 128., 0.), (256., -128., 128. + (16 if moved else 0)), (256., 128., 128.)}
    if not expected.issubset(coords):
        raise RuntimeError(f'missing portal corners: {coords}, expected {expected}')
    # A boundary may retain a collinear point where another source face
    # actually terminates. Such a branch is necessary to keep the wall sewn;
    # an unbranched extra point is an unnecessary subdivision.
    if not moved:
        for idx in indices:
            if vertices[idx] in expected:
                continue
            branched = False
            for sector in sectors:
                for face in sector:
                    ring = face[3]
                    if idx not in ring: continue
                    k = ring.index(idx); a, p, b = (vertices[ring[(k-1) % len(ring)]], vertices[idx], vertices[ring[(k+1) % len(ring)]])
                    u = [a[i] - p[i] for i in range(3)]; v = [b[i] - p[i] for i in range(3)]
                    cross = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
                    if sum(x*x for x in cross) > 1e-12: branched = True
            if not branched:
                raise RuntimeError(f'unnecessary unbranched portal point {vertices[idx]}')
    # Every portal endpoint must be shared with incident shell faces on both
    # sides. Otherwise a subsequent edit can pull the room boundary apart.
    for idx in (i for i in indices if vertices[i] in expected):
        for sector in sectors:
            if sum(idx in face[3] for face in sector) < 2:
                raise RuntimeError(f'portal endpoint {vertices[idx]} is not stitched into incident faces')
    for idx in indices:
        p = vertices[idx]
        for sector in sectors:
            for face in sector:
                ring = face[3]
                for i, first in enumerate(ring):
                    second = ring[(i+1) % len(ring)]
                    if idx in (first, second): continue
                    a, b = vertices[first], vertices[second]
                    edge = [b[k]-a[k] for k in range(3)]; rel = [p[k]-a[k] for k in range(3)]
                    length = sum(x*x for x in edge)
                    if length < 1e-12: continue
                    t = sum(rel[k]*edge[k] for k in range(3))/length
                    if 1e-8 < t < 1-1e-8 and sum((rel[k]-t*edge[k])**2 for k in range(3)) < 1e-12:
                        raise RuntimeError(f'portal endpoint {p} forms a T-junction')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=45)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args(); args.width, args.height = 1280, 720
    args.binary = args.binary.resolve(); source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-sector-link-cleanup-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    shutil.copyfile(Path(__file__).parent / 'fixtures/sector_link_fragmented.map', work / 'id1/maps/legacyfixture.map')
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nmap start') if args.validation else bench.BOOT
    (work / 'id1/boot.cfg').write_text(boot)
    os.environ['SMOKE_WIDTH'] = str(args.width); os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)

    def gesture(chars, status):
        start = len(engine.text()); os.write(engine.key, chars.encode('ascii'))
        engine.wait(lambda text: status in text, start)

    def command(body):
        engine.serial += 1; marker = f'LINK_CLEANUP_{engine.serial}'; start = len(engine.text())
        (work / 'id1/control.tmp').write_text(body + '\neditor_return\necho ' + marker + '\n')
        (work / 'id1/control.tmp').replace(work / 'id1/control.cfg')
        os.write(engine.key, b'c'); engine.wait(lambda text: marker in text, start)
        return engine.text()[start:]

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        opened = engine.cmd('disconnect\neditor_open legacyfixture\neditor_cleanup_links')
        if 'Sector links cleaned up' not in opened:
            raise RuntimeError('could not clean fragmented legacy portal: ' + opened)
        out = command('editor_save legacy_clean\neditor_cleanup_links')
        if 'No sector link cleanup changes made' not in out:
            raise RuntimeError('cleanup of an already-clean portal was not a no-op: ' + out)
        check_link(work / 'id1/maps/legacy_clean.map')
        gesture('z', 'Undid change')
        command('editor_save legacy_undo')
        _, old = read_sector_source(work / 'id1/maps/legacy_undo.map')
        if sum(face[0] >= 0 for face in old[0]) != 3:
            raise RuntimeError('undo did not restore the original legacy fragments')
        gesture('w', 'Redid change')
        command('editor_save legacy_redo\neditor_open legacy_redo')
        check_link(work / 'id1/maps/legacy_redo.map')
        if 'New 3D sector map' not in command('editor_new force'):
            raise RuntimeError('could not open editor')
        gesture('tr', 'Sector created')
        gesture('4ahljl', 'Sector portal linked')
        command('editor_save linked')
        check_link(work / 'id1/maps/linked.map')
        gesture('z', 'Undid change')
        command('editor_save unlinked')
        _, sectors = read_sector_source(work / 'id1/maps/unlinked.map')
        if any(face[0] >= 0 for sector in sectors for face in sector):
            raise RuntimeError('undo did not remove portal')
        gesture('w', 'Redid change')
        command('editor_save relinked\neditor_open relinked')
        check_link(work / 'id1/maps/relinked.map')

        # Move a shared corner after reload. Both rooms must still refer to
        # the same vertex, including the incident wall/floor/ceiling faces.
        gesture('vq', 'Sector vertex moved')
        command('editor_save moved')
        check_link(work / 'id1/maps/moved.map', moved=True)

        start = len(engine.text()); os.write(engine.key, b'f')
        engine.wait(lambda text: 'Starting playtest' in text, start)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text, start)
        bsps = list((work / 'id1/maps').glob('__quiver_editor_*.bsp'))
        if len(bsps) != 1 or bsps[0].read_bytes()[:4] != b'BSP2':
            raise RuntimeError('missing compiled BSP2 playtest')
        player, _ = collision_reader(bsps[0])
        for x in range(220, 305, 4):
            if player((x, 0, 32)) != -1:
                raise RuntimeError(f'player hull blocked at doorway x={x}')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print('PASS: cleaned portal, shared vertex edit, undo/redo, save/reopen and player-hull traversal', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate(); engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__': main()
