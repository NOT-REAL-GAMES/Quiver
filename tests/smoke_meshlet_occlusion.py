#!/usr/bin/env python3
"""Test independent meshlet visibility against a no-occlusion image oracle.

Requires the registered id1 data, SDL headers, Pillow and a Vulkan display.
All images and the deliberately invalid PVS fixture are private to a new /tmp
game directory. No game data, profiling instrumentation or special BSP compiler
is required in the source tree.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile
import time

from bench_meshlets import BOOT, Engine, FRUSTUM, STATE, VISIBILITY, MESH_SHADER, TASK_MESH, TASK_SHADER, delta
from smoke_bsp_meshlets import validation_report


COUNTERS = re.compile(r'Meshlet culling: available (\d+) tested (\d+) frustum (\d+) occluded (\d+) visible (\d+) triangles (\d+)')


def poison_pvs(paks, destination):
    """Copy a stock BSP29, changing only its visibility row and leaf vis offsets."""
    for pak in reversed(paks):
        with pak.open('rb') as f:
            magic, at, size = struct.unpack('<4sII', f.read(12))
            if magic != b'PACK' or size % 64:
                raise ValueError(f'invalid Quake pak: {pak}')
            f.seek(at)
            for name, off, length in struct.iter_unpack('<56sII', f.read(size)):
                if name.rstrip(b'\0').lower() != b'maps/e1m1.bsp':
                    continue
                f.seek(off)
                data = bytearray(f.read(length))
                if struct.unpack_from('<i', data)[0] != 29:
                    raise ValueError('the poisoned-PVS fixture requires original id1 BSP29')
                vis, vis_size = struct.unpack_from('<II', data, 4 + 4 * 8)
                leaves, leaf_size = struct.unpack_from('<II', data, 4 + 10 * 8)
                model, _ = struct.unpack_from('<II', data, 4 + 14 * 8)
                visleafs, = struct.unpack_from('<I', data, model + 52)
                if leaf_size % 28 or leaves + leaf_size > len(data) or vis + vis_size > len(data):
                    raise ValueError('invalid BSP29 leaf or vis lump')
                row = (visleafs + 31) // 8
                packed = b''.join(bytes((0, min(row - i, 255))) for i in range(0, row, 255))
                if not row or len(packed) > vis_size:
                    raise ValueError('BSP visibility lump too small')
                data[vis:vis + len(packed)] = packed
                for i in range(1, leaf_size // 28):
                    struct.pack_into('<i', data, leaves + i * 28 + 4, 0)
                destination.parent.mkdir(exist_ok=True)
                destination.write_bytes(data)
                return dict(source=str(pak), faces_bsp='e1m1', leaf_count=leaf_size // 28,
                            pvs_row_bytes=row, zero_row_compressed_bytes=len(packed))
    raise ValueError('could not find maps/e1m1.bsp in local paks')


def check_delta(entry, a, b, repeat=None):
    change = entry['difference'] = delta(a, b)
    control = repeat or dict(mean_rgb=0, percent_over_4=0)
    if change['mean_rgb'] > control['mean_rgb'] + .1 or change['percent_over_4'] > control['percent_over_4'] + .2:
        raise RuntimeError(f'visible geometry changed: {change}, control {control}')


def cutout_copy(source, destination):
    """Keep the geometry and texture pixels; only change opaque material names."""
    data = bytearray(source.read_bytes())
    tex, size = struct.unpack_from('<II', data, 4 + 2 * 8)
    count, = struct.unpack_from('<i', data, tex)
    if tex + size > len(data) or not 0 < count <= (size - 4) // 4:
        raise ValueError('invalid BSP29 miptex directory')
    changed = 0
    for i, offset in enumerate(struct.unpack_from(f'<{count}i', data, tex + 4)):
        if offset < 0 or offset + 40 > size:
            raise ValueError('no-occluder fixture requires embedded BSP textures')
        start = tex + offset
        name = data[start:start + 16].lower()
        if name.startswith((b'{', b'*', b'!', b'sky')):
            continue
        struct.pack_into('<16s', data, start, f'{{test{i}'.encode('ascii'))
        changed += 1
    if not changed:
        raise ValueError('no opaque materials to convert')
    destination.write_bytes(data)
    return changed


def photo_burst(engine, cmd, label, frames=7):
    # Capture consecutive host frames, including the first one after the
    # server transmits the new camera. Issuing separate commands from Python
    # would skip the very frames most likely to expose stale depth.
    start = len(engine.text())
    # The screenshot flag belongs to the asynchronous end-render task.
    # meshlet_stats joins that task, preventing a previous frame from
    # clearing the next request. This is a correctness test, not a timing.
    engine.cmd(cmd + '\n' + 'meshlet_stats\nscreenshot png\nwait\n' * frames)
    paths = engine.wait(lambda t: matches if len(matches := re.findall(r'Wrote [\x01\x02]?([^\n]+\.png)', t)) >= frames else None, start)
    out = []
    for i, name in enumerate(paths[:frames]):
        dest = engine.work / f'{label}_{i}.png'
        shutil.copyfile(next(engine.work.rglob(name)), dest)
        out.append(dest)
    return out


def find_armor_edict(engine):
    # An existing server entity gives us a real, networked dynamic light
    # without a test-only renderer command or a ray-tracing dependency.
    output = engine.cmd('edicts')
    entries = re.split(r'\nEDICT\s+(\d+):\s*\n', output)
    candidates = []
    for i in range(1, len(entries) - 1, 2):
        number, body = int(entries[i]), entries[i + 1]
        if not re.search(r'(?m)^classname\s+item_armor1\s*$', body):
            continue
        origin = re.search(r"(?m)^origin\s+'?\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", body)
        if origin:
            xyz = tuple(map(float, origin.groups()))
            candidates.append((sum((a - b) ** 2 for a, b in zip(xyz, (688, 480, 80))), number))
    if not candidates or min(candidates)[0] > 256 ** 2:
        raise RuntimeError('could not find the nearby e1m1 armor entity')
    return min(candidates)[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--mesh-shader', action='store_true', help='compare mode 3 against mode 2')
    parser.add_argument('--task-shader', action='store_true', help='also compare task culling against mode 3')
    parser.add_argument('--width', type=int, default=800)
    parser.add_argument('--height', type=int, default=600)
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    if args.task_shader:
        args.mesh_shader = True
    args.stock_only = False
    args.binary = args.binary.resolve()
    base = args.basedir.resolve()
    source = next((p for p in base.iterdir() if p.is_dir() and p.name.lower() == 'id1'), base)
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake game pak files at {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-occlusion-smoke-'))
    print(work, flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    fixture = poison_pvs(paks, work / 'id1/maps/pvs_void.bsp')
    fixture['cutout_materials'] = cutout_copy(work / 'id1/maps/pvs_void.bsp', work / 'id1/maps/pvs_cutout.bsp')
    config = BOOT.replace('map start', 'r_rtshadows 0\nr_meshlets 2\nr_meshlet_occlusion_stats 1\nr_flatlightstyles 1\nr_waterwarp 0\nmap start')
    (work / 'id1/boot.cfg').write_text(config)
    report = dict(work=str(work), success=False, fixture=fixture, checks={}, stats={})
    engine = None
    saw_rejection = False

    def snapshot(label, hiz):
        result = engine.cmd(f'r_meshlet_occlusion {int(hiz)}\nr_meshlets 2\nwait\nwait\nwait\nmeshlet_stats')
        mode = VISIBILITY.search(result)
        if not mode or mode[1] != 'independent' or int(mode[2]) != int(hiz):
            raise RuntimeError(f'{label}: wrong visibility mode: {result}')
        match = COUNTERS.search(result)
        if not match or int(match[1]) != 1:
            raise RuntimeError(f'{label}: no completed GPU cull counters: {result}')
        stats = dict(zip(('available', 'tested', 'frustum', 'occluded', 'visible', 'triangles'), map(int, match.groups())))
        if not stats['tested'] or (not hiz and stats['occluded']):
            raise RuntimeError(f'{label}: invalid counts {stats}')
        report['stats'][label] = stats
        reference = engine.photo(label + ('_compute' if args.mesh_shader else ''))
        if not args.mesh_shader:
            return reference, stats
        result = engine.cmd(f'r_meshlets 3\nr_meshlet_task 0\nwait\nwait\nwait\nmeshlet_stats')
        mesh = MESH_SHADER.search(result)
        expected = 0 if 'xray' in label else 1
        if not mesh or int(mesh[1]) != expected or (expected and int(mesh[2]) == 0):
            raise RuntimeError(f'{label}: mesh shader active state expected {expected}: {result}')
        actual = engine.photo(label + ('_mesh' if args.task_shader else ''))
        check_delta(report['checks'].setdefault(label + '_mesh_vs_compute', {}), reference, actual)
        if args.task_shader:
            result = engine.cmd('r_meshlet_task 1\nwait\nwait\nwait\nmeshlet_stats')
            task = TASK_SHADER.search(result)
            batches = re.search(r'Mesh shader batches: (\d+) partial task batches (\d+)', result)
            if not task or int(task[1]) != expected:
                raise RuntimeError(f'{label}: task shader active state expected {expected}: {result}')
            if expected and (not batches or int(batches[1]) == 0 or int(batches[2]) == 0):
                raise RuntimeError(f'{label}: no real partial task groups exercised: {result}')
            task_image = engine.photo(label)
            check_delta(report['checks'].setdefault(label + '_task_vs_mesh', {}), actual, task_image)
            actual = task_image
        return actual, stats

    def ab(label, xray=False):
        nonlocal saw_rejection
        reference, off = snapshot(label + '_off', False)
        repeat = delta(reference, engine.photo(label + '_repeat'))
        actual, on = snapshot(label + '_on', True)
        entry = report['checks'][label] = dict(reference=str(reference), actual=str(actual), repeat=repeat)
        if repeat['mean_rgb'] > .25 or repeat['percent_over_4'] > .6:
            raise RuntimeError(f'{label}: unstable image oracle {repeat}')
        if not xray:
            check_delta(entry, reference, actual, repeat)
            if repeat['max_channel'] == 0 and entry['difference']['max_channel']:
                raise RuntimeError(f'{label}: a frozen reference changed pixels with occlusion enabled')
        else:
            # Unoccluded x-ray wire and occlusion-submitted x-ray wire may
            # intentionally differ behind the opaque walls.
            entry['difference'] = delta(reference, actual)
        if (on['tested'], on['frustum']) != (off['tested'], off['frustum']) or on['triangles'] > off['triangles']:
            raise RuntimeError(f'{label}: same-camera frustum/count mismatch: {off}, {on}')
        saw_rejection |= on['occluded'] > 0 and on['triangles'] < off['triangles']
        return actual

    try:
        engine = Engine(args, work)
        engine.wait(lambda t: 'CL_SignonReply: 4' in t)
        engine.view('start', '-400 2400 138 0 0 0')
        door = ab('door')
        engine.cmd('r_drawentities 0\nwait\nwait')
        if delta(door, engine.photo('no_door'))['mean_rgb'] <= .5:
            raise RuntimeError('selected door not visible')
        engine.cmd('r_drawentities 1\nr_showtris 1\nwait\nwait')
        ab('xray', xray=True)
        engine.cmd('r_showtris 0')
        engine.view('e1m1', '1312 880 -248 0 90 0')
        corridor = ab('corridor')
        if args.mesh_shader:
            engine.backend(2)
            engine.cmd('r_drawworld 0\nwait\nwait')
            hidden = engine.photo('drawworld_off_compute')
            if delta(corridor, hidden)['mean_rgb'] <= 2:
                raise RuntimeError('r_drawworld 0 did not hide the world')
            result = engine.cmd(f'r_meshlets 3\nr_meshlet_task {int(args.task_shader)}\nwait\nwait\nmeshlet_stats')
            mesh = MESH_SHADER.search(result)
            if not mesh or int(mesh[1]) != 1 or int(mesh[2]) == 0:
                raise RuntimeError(f'mesh shader inactive with r_drawworld 0: {result}')
            if args.task_shader and (not (task := TASK_SHADER.search(result)) or int(task[1]) != 1):
                raise RuntimeError(f'task shader inactive with r_drawworld 0: {result}')
            check_delta(report['checks'].setdefault('drawworld_off', {}), hidden, engine.photo('drawworld_off_mesh'))
            engine.cmd('r_drawworld 1\nwait\nwait')
            check_delta(report['checks'].setdefault('drawworld_restored', {}), corridor, engine.photo('drawworld_restored'))
            engine.cmd('fog 0.06 0.2 0.3 0.4\nwait\nwait')
            fogged = ab('fog')
            if delta(corridor, fogged)['mean_rgb'] <= .5:
                raise RuntimeError('fog positive control did not change the scene')
            engine.cmd('fog 0\nviewsize 80\nwait\nwait')
            ab('partial_viewport')
            engine.cmd('viewsize 130\nwait\nwait')
        engine.view('e1m1', '869 910 -190 59 180 0')
        engine.cmd('r_novis 1\nr_wateralpha 0.35\nwait\nwait')
        water = ab('transparent_water')
        engine.cmd('r_wateralpha 1\nwait\nwait')
        if delta(water, engine.photo('opaque_water'))['mean_rgb'] <= .5:
            raise RuntimeError('selected water not visible')
        engine.cmd('r_novis 0')
        engine.view('e1m2', '150 -8 666 -59 180 0')
        sky = ab('sky')
        engine.cmd('r_fastsky 1\nwait\nwait')
        if delta(sky, engine.photo('flat_sky'))['mean_rgb'] <= .5:
            raise RuntimeError('selected sky not visible')
        engine.cmd('r_fastsky 0\nvid_fsaa 4\nvid_restart\nwait\nwait')
        ab('msaa_restart')
        engine.cmd('vid_fsaa 0\nvid_restart\nwait\nwait')
        for name, cvar in [('fallback_indirect', 'r_indirect'), ('fallback_lightmap', 'r_gpulightmapupdate')]:
            engine.cmd(cvar + ' 0\nwait\nwait')
            engine.backend(0, verify=False)
            stock = engine.photo(name + '_stock')
            result = engine.backend(2, verify=False)
            state = STATE.search(result)
            if not state or tuple(map(int, state.groups())) != (0, 2, 0):
                raise RuntimeError(f'{name}: stock fallback not active: {result}')
            check_delta(report['checks'].setdefault(name, {}), stock, engine.photo(name + '_candidate'))
            if args.task_shader:
                result = engine.backend(TASK_MESH, verify=False)
                task = TASK_SHADER.search(result)
                state = STATE.search(result)
                if not state or tuple(map(int, state.groups())) != (0, 3, 0) or not task or int(task[1]) != 0:
                    raise RuntimeError(f'{name}: task stock fallback not active: {result}')
                check_delta(report['checks'].setdefault(name + '_task', {}), stock, engine.photo(name + '_task'))
            engine.cmd(cvar + ' 1\nwait\nwait')
        ab('resumed')

        # Isolate static faces and eliminate light/PVS and entity dependencies.
        engine.cmd('r_drawentities 0\nr_fullbright 1')
        engine.view('pvs_void', '480 -352 88 0 90 0')
        engine.cmd('r_novis 0')
        engine.backend(1)
        empty = engine.photo('pvs_hybrid_empty')
        engine.cmd('r_novis 1\nwait\nwait')
        visible = engine.photo('pvs_hybrid_all')
        control = delta(empty, visible)
        if control['mean_rgb'] <= 2:
            raise RuntimeError(f'poisoned PVS failed to suppress hybrid world: {control}')
        engine.cmd('r_novis 0\nwait\nwait')
        independent = ab('poisoned_pvs')
        entry = report['checks']['poisoned_pvs']
        entry['pvs_control'] = control
        check_delta(entry.setdefault('matches_all_faces', {}), visible, independent)
        # The engine has distinct scalar, SIMD, serial and parallel leaf-mark paths.
        # None may mask the independently drawn world when PVS is empty.
        for tasks, simd in ((0, 0), (0, 1), (1, 0)):
            engine.cmd(f'r_tasks {tasks}\nr_simd {simd}\nwait\nwait')
            label = f'poisoned_pvs_tasks{tasks}_simd{simd}'
            check_delta(entry.setdefault(label, {}), visible, engine.photo(label))
        engine.cmd('r_tasks 1\nr_simd 1\nwait\nwait')
        engine.cmd('r_novis 1\nwait\nwait')
        check_delta(entry.setdefault('independent_of_r_novis', {}), independent, engine.photo('independent_all'))

        # Disable OIT to exercise sorted, translucent water. The hybrid
        # all-visible reference includes static surfaces in other BSP leaves.
        engine.view('pvs_void', '869 910 -190 59 180 0')
        engine.cmd('r_oit 0\nr_alphasort 1\nr_wateralpha 0.35\nr_novis 1\nwait\nwait')
        engine.backend(1)
        sorted_water = engine.photo('sorted_water_all_visible')
        engine.cmd('r_wateralpha 1\nwait\nwait')
        control = delta(sorted_water, engine.photo('sorted_water_opaque'))
        if control['mean_rgb'] <= .5:
            raise RuntimeError(f'poisoned water view has no translucent contribution: {control}')
        engine.cmd('r_wateralpha 0.35\nr_novis 0')
        water = ab('poisoned_sorted_water')
        entry = report['checks']['poisoned_sorted_water']
        entry['opacity_control'] = control
        check_delta(entry.setdefault('matches_all_faces', {}), sorted_water, water)

        # Stock e1m1 faces 4842/4843 have lightstyle 10 on the floor. Freeze
        # the map at one instant and explicitly change lightstyle values while
        # its BSP PVS is empty. This catches stale GPU lightmap updates even
        # when all previously uploaded pages happened to start out correct.
        engine.view('pvs_void', '480 208 64 56 90 0')
        engine.cmd('r_fullbright 0\nr_novis 1\nr_flatlightstyles 2')
        engine.backend(1)
        peak = engine.photo('style_peak_all_visible')
        engine.cmd('r_flatlightstyles 1\nwait\nwait')
        average = engine.photo('style_average_all_visible')
        control = delta(peak, average)
        if control['mean_rgb'] <= .2:
            raise RuntimeError(f'animated lightstyle control did not change: {control}')
        engine.cmd('r_flatlightstyles 0\nwait\nwait')
        animated = engine.photo('style_animated_all_visible')
        engine.cmd('r_novis 0\nr_flatlightstyles 2')
        peak_actual = ab('poisoned_lightstyles')
        entry = report['checks']['poisoned_lightstyles']
        entry['peak_vs_average'] = control
        check_delta(entry.setdefault('peak', {}), peak, peak_actual)
        engine.cmd('r_flatlightstyles 1\nwait\nwait')
        check_delta(entry.setdefault('average', {}), average, engine.photo('style_average_occlusion'))
        engine.cmd('r_flatlightstyles 0\nwait\nwait')
        check_delta(entry.setdefault('animated', {}), animated, engine.photo('style_animated_occlusion'))

        # EF_BRIGHTLIGHT on an ordinary map entity creates a real transient
        # client dlight. Switch rendering modes with it present, remove it
        # through r_dynamic, then revisit the affected atlas in mode 2. This
        # catches early-outs that leave stale lighting after the previous
        # frame's light list becomes empty. Radius has a small random jitter,
        # so lit comparisons use repeated samples as the noise control.
        engine.view('e1m1', '688 400 80 56 90 0')
        engine.cmd('pause\nhost_framerate 0.000001\nr_drawentities 0\nr_fullbright 0\nr_flatlightstyles 1\nr_novis 1\nr_dynamic 0')
        light_edict = find_armor_edict(engine)
        engine.backend(0)
        unlit = engine.photo('dynamic_stock_off')
        engine.cmd(f'edict {light_edict} effects 4\nr_dynamic 1' + '\nwait' * 8)
        # Bright-light radius is randomly selected from 32 integer values in
        # CL_RelinkEntities. Collect multiple frames so the comparison can
        # match a radius independently of renderer and frame timing.
        stock_lit = [engine.photo(f'dynamic_stock_on_{i}') for i in range(32)]
        positive = min((delta(unlit, image) for image in stock_lit), key=lambda item: item['mean_rgb'])
        if positive['mean_rgb'] <= .5:
            raise RuntimeError(f'dynamic-light positive control did not illuminate geometry: {positive}')
        stock_noise = [delta(stock_lit[0], image) for image in stock_lit[1:]]
        envelope = {key: max(sample[key] for sample in stock_noise) for key in stock_noise[0]}
        engine.backend(TASK_MESH if args.task_shader else (3 if args.mesh_shader else 2))
        candidate_lit = [engine.photo(f'dynamic_meshlet_on_{i}') for i in range(12)]
        nearest = [min((delta(a, b) for a in stock_lit), key=lambda item: item['mean_rgb']) for b in candidate_lit]
        entry = report['checks']['dynamic_lights'] = {
            'edict': light_edict, 'positive': positive, 'stock_jitter_envelope': envelope, 'lit_mode_switch': nearest}
        for sample in nearest:
            if (sample['mean_rgb'] > envelope['mean_rgb'] + .01 or
                    sample['percent_over_4'] > envelope['percent_over_4'] + .1 or
                    sample['max_channel'] > envelope['max_channel'] + 1):
                raise RuntimeError(f'lit frame outside stock jitter envelope: {sample}, stock {envelope}')
        engine.cmd('r_dynamic 0' + '\nwait' * 8)
        check_delta(entry.setdefault('removed_mode2', {}), unlit, engine.photo('dynamic_meshlet_off'))
        engine.backend(0)
        engine.cmd('r_dynamic 1' + '\nwait' * 8)
        engine.backend(TASK_MESH if args.task_shader else (3 if args.mesh_shader else 2))
        engine.cmd('r_dynamic 0' + '\nwait' * 8)
        check_delta(entry.setdefault('removed_after_switch', {}), unlit, engine.photo('dynamic_removed_after_switch'))
        engine.cmd(f'edict {light_edict} effects 0\nr_dynamic 1\nhost_framerate 0')

        # Every ordinary texture in this PVS-zero world is now a cutout.
        # Disable diagnostic counters: the actual fallback must dispatch the
        # regular independent shader without a depth set or a readback.
        engine.cmd('r_meshlet_occlusion_stats 0\nr_fullbright 1\nr_novis 0')
        engine.view('pvs_cutout', '480 -352 88 0 90 0')
        engine.backend(1)
        empty = engine.photo('cutout_hybrid_empty')
        engine.cmd('r_novis 1\nwait\nwait')
        visible = engine.photo('cutout_all_visible')
        entry = report['checks']['no_occluders'] = {'pvs_control': delta(empty, visible)}
        if entry['pvs_control']['mean_rgb'] <= 2:
            raise RuntimeError('cutout fixture did not suppress visible geometry with hybrid PVS')
        engine.cmd('r_novis 0')
        engine.backend(FRUSTUM)
        reference = engine.photo('cutout_frustum')
        result = engine.backend(2, verify=False)
        state, mode, counts = STATE.search(result), VISIBILITY.search(result), COUNTERS.search(result)
        if (not state or state.groups() != ('1', '2', '1') or not mode or mode.groups() != ('independent', '0', '0')
                or not counts or counts[1] != '0'):
            raise RuntimeError(f'no-occluder fallback failed to stay independent without Hi-Z: {result}')
        entry['renderer'] = result
        fallback = engine.photo('cutout_fallback')
        for label, expected in [('matches_all_faces', visible), ('matches_frustum', reference)]:
            change = entry[label] = delta(expected, fallback)
            if change['max_channel']:
                raise RuntimeError(f'cutout {label}: {change}')
        engine.cmd('r_meshlet_occlusion_stats 1')

        # A real teleport to a different room with a 180-degree turn. Compare
        # each frame with reference frames from the same move; a one-frame
        # variation in delivery of the viewangles packet is harmless.
        engine.view('e1m1', '480 -352 88 0 90 0')
        engine.cmd('r_novis 1\nr_fullbright 1\nr_fastsky 1\ngod\nnotarget\npause\ncl_nolerp 1\nhost_maxfps 60\nhost_framerate 0.0166667')
        bursts = []
        for hiz in (0, 1):
            engine.cmd(f'r_meshlet_occlusion {hiz}\nsetpos 480 -352 88 0 90 0' + '\nwait' * 8)
            bursts.append(photo_burst(engine, 'setpos 1312 880 -248 0 270 0', f'teleport_{hiz}'))
        new_view = delta(bursts[0][0], bursts[0][-1])
        # Server command can take effect on the first frame; compare against
        # a separately captured original camera for a positive motion control.
        engine.cmd('setpos 480 -352 88 0 90 0' + '\nwait' * 8)
        start = engine.photo('teleport_origin')
        if delta(start, bursts[0][-1])['mean_rgb'] <= 3:
            raise RuntimeError('teleport did not expose a different room')
        distances = [min(delta(frame, ref)['mean_rgb'] for ref in bursts[0]) for frame in bursts[1]]
        report['checks']['teleport'] = dict(frame_count=len(distances), closest_reference_rgb=distances,
                                           endpoint_change=new_view)
        if max(distances) != 0:
            raise RuntimeError(f'teleport exposed bad transient geometry: {distances}')
        if not saw_rejection:
            raise RuntimeError('no view rejected meshlets AND reduced drawn triangles relative to frustum-only')
        report['success'] = True
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        if engine:
            engine.close()
            report['exit_code'] = engine.process.returncode
            if engine.process.returncode:
                report['success'] = False
        output = (work / 'output.log').read_text(errors='replace') if (work / 'output.log').exists() else ''
        report['msaa_4x_confirmed'] = '4 AA Samples' in output
        if not report['msaa_4x_confirmed']:
            report['success'] = False
        if args.validation:
            report['validation'] = v = validation_report(output)
            if not v['sync_active'] or not v['active'] or v['error_count']:
                report['success'] = False
        (work / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
        print({k: report[k] for k in ('work', 'success', 'error') if k in report}, flush=True)
    if not report['success']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
