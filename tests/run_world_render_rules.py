#!/usr/bin/env python3
"""Exercise Quake's world/sky controls on a stationary streamed-world camera.

Requires game paks, SDL2, a Vulkan display, and Pillow. Configs, world, images
and logs are isolated in /tmp. Image comparisons exclude the HUD/console.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageStat
from run_terrain_lod_smoke import Run, WRAPPER, parse_stats
from run_terrain_smoothing_smoke import camera


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--multiplayer', action='store_true', help='check the two-slot listen-server cheat restrictions')
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--stage-timeout', type=int, default=90)
    parser.add_argument('--width', type=int, default=800)
    parser.add_argument('--height', type=int, default=600)
    args = parser.parse_args()
    source = args.basedir.resolve()
    paks = sorted((source / 'id1' if (source / 'id1').is_dir() else source).glob('pak*.pak'))
    if not paks:
        parser.error('basedir contains no pak assets')
    work = Path(tempfile.mkdtemp(prefix='quiver-render-rules-'))
    print(work, flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name).symlink_to(pak)
    shutil.copy2(args.binary.resolve(), work / 'vkquake')
    (work / 'autokey.c').write_text(WRAPPER)
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', str(work / 'autokey.c'), '-o', str(work / 'autokey.so'), '-ldl'], check=True)
    (work / 'id1/startup.cfg').write_text(f'''vid_fullscreen 0
vid_vsync 0
host_maxfps 60
r_tasks 1
r_indirect 1
r_fastsky 1
r_clouds 0
r_clouds_speed 0
r_clouds_coverage 0.78
r_clouds_height 2000
r_drawviewmodel 0
r_drawentities 0
r_fastclear 1
cl_bob 0
cl_rollangle 0
crosshair 0
viewsize 130
cl_nocsqc 1
developer 1
scr_conspeed 100000
con_notifytime -1
world_view_chunks 1
r_octree_distance 2048
r_octree_lod_distance 16384
r_sun_angle 40
maxplayers {2 if args.multiplayer else 1}
deathmatch 0
bind F12 "exec smoke_command.cfg"
world_create render_rules terrain 4242
''')
    (work / 'id1/vkQuake.cfg').write_text(f'vid_width {args.width}\nvid_height {args.height}\nvid_fsaa 0\n')
    report = dict(success=False, work=str(work), checks={}, captures={}, cloud_paths={})
    run = None

    def take(label, commands=''):
        if commands:
            run.command(commands + '\nwait\nwait')
        run.delay(.18)
        path = run.photograph(label)
        report['captures'][label] = path
        with Image.open(path) as image:
            return image.convert('RGB').crop((8, image.height // 5, image.width - 8, image.height - 28))

    def compare(label, a, b, minimum=None, maximum=None):
        value = sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3
        report['checks'][label] = value
        if (minimum is not None and value < minimum) or (maximum is not None and value > maximum):
            raise RuntimeError(f'{label}: RGB difference {value:.4f} outside [{minimum}, {maximum}]')

    try:
        run = Run(args, work)
        run.wait(lambda t: 'CL_SignonReply: 4' in t, 'initial terrain signon')
        run.command('noclip 1\ngod 1')
        camera(run, '128 128 768 15 45 0')
        run.settle(True, True, idle_uploads=True)
        normal = take('normal')
        if args.multiplayer:
            # Cheats must be ignored even when only one client joined the two-slot server.
            if '"maxplayers" is "2"' not in run.command('maxplayers'):
                raise RuntimeError('server did not start with two slots')
            locked = take('protected', 'r_showtris 1\nr_lightmap 1\nr_fullbright 1\nr_drawworld 0')
            compare('multiplayer_matches_normal', normal, locked, maximum=.2)
            stats = parse_stats(run.command('octree_render_stats'))
            if not stats['near_draws'] or not stats['far_draws']:
                raise RuntimeError('multiplayer world toggle hid geometry')
        else:
            xray = take('show_all', 'r_showtris 1')
            visible = take('show_visible', 'r_showtris 2')
            compare('all_triangle_overlay', normal, xray, minimum=1)
            compare('visible_triangle_overlay', normal, visible, minimum=1)
            compare('overlay_modes_differ', visible, xray, minimum=.05)
            serial = take('show_visible_serial', 'r_tasks 0')
            compare('serial_matches_parallel', serial, visible, maximum=.3)
            fullbright = take('fullbright', 'r_tasks 1\nr_showtris 0\nr_fullbright 1')
            lightmap = take('lightmap', 'r_lightmap 1')
            compare('fullbright_response', normal, fullbright, minimum=.3)
            compare('light_only_response', normal, lightmap, minimum=2)
            compare('lightmap_precedence', lightmap, fullbright, minimum=2)
            hidden = take('world_hidden', 'r_lightmap 0\nr_fullbright 0\nr_drawworld 0\nr_fastsky 0\nr_clouds 1\nr_showtris 1')
            if max(high for low, high in hidden.getextrema()) > 4:
                raise RuntimeError('hidden world still has sky, clouds or triangle overlays')
            counts = parse_stats(run.command('octree_render_stats'))
            if counts['near_draws'] or counts['far_draws']:
                raise RuntimeError('hidden terrain still submitted draws')
            short = take('terrain_clip_512', 'r_drawworld 1\nr_showtris 0\nr_fastsky 1\nr_clouds 0\ngl_farclip 512')
            compare('terrain_far_clip', normal, short, minimum=2)
            run.command('gl_farclip 16384')
            run.settle(True, True, idle_uploads=True)
            compare('terrain_clip_restored', normal, take('terrain_restored'), maximum=.3)
            camera(run, '128 128 256 55 45 0')
            smooth = take('terrain_filtered', 'vid_filter 0')
            compare('terrain_texture_filter', smooth, take('terrain_point', 'vid_filter 1'), minimum=.05)
            run.command('vid_filter 0')

            # Below the cloud shell looking up: 512 cannot reach it, 16384 can.
            camera(run, '128 128 320 -55 45 0')
            run.command('r_fastsky 0\nr_atmosphere 1')
            for path, divisor, cascades in (('reduced', 2, 0), ('full', 1, 0), ('cascades_msaa', 2, 1)):
                if cascades:
                    run.command('vid_fsaa 4\nvid_restart')
                    run.delay(.5)
                run.command(f'r_clouds_resolution {divisor}\nr_clouds_cascades {cascades}')
                for clip, clipped in ((512, True), (16384, False)):
                    off = take(f'{path}_{clip}_off', f'gl_farclip {clip}\nr_clouds 0')
                    on = take(f'{path}_{clip}_on', 'r_clouds 1')
                    compare(f'cloud_far_clip_{path}_{clip}', off, on, maximum=.05 if clipped else None, minimum=None if clipped else 2)
                stats = run.command('sky_stats')
                report['cloud_paths'][path] = stats.strip()
                expected = 'full' if divisor == 1 else 'reduced'
                if f'Sky clouds: path {expected} ' not in stats or f'Sky bands: cascades {cascades} ' not in stats:
                    raise RuntimeError(f'{path} cloud path unavailable: {stats}')
            # With geometry out of the way the synthetic sky still has real
            # dome triangles, not a wire outline of the fullscreen cloud pass.
            dome = take('clouds_wire', 'r_showtris 2')
            compare('sky_wire_overlay', on, dome, minimum=.15)
            fast = take('fast_sky', 'r_showtris 0\nr_fastsky 1')
            compare('fast_sky_disables_clouds', fast, take('fast_sky_no_clouds', 'r_clouds 0'), maximum=.05)
            # Looking up at a thin layer, the base lies inside the far plane
            # but farther than 2100 units along the rays near either side of
            # this wide view. A spherical distance cap would clear both strips.
            camera(run, '128 128 320 -90 45 0')
            edge_off = take('off_axis_off', 'r_fastsky 0\nfov 110\ngl_farclip 2100\nr_clouds_height 2200\nr_clouds_thickness 256\nr_clouds_coverage 1')
            edge_on = take('off_axis_on', 'r_clouds 1')
            for name, x in (('left', 0), ('right', edge_on.width * 9 // 10)):
                area = (x, edge_on.height // 3, x + edge_on.width // 10, edge_on.height * 2 // 3)
                compare('off_axis_clouds_' + name, edge_off.crop(area), edge_on.crop(area), minimum=2)
            run.command('gl_farclip 16384\nfov 90')
            run.map_command('map e1m1')
            run.command('r_fastsky 0\nr_clouds 1\nr_showtris 2')
            take('bsp_wire')
            counts = parse_stats(run.command('octree_render_stats'))
            if counts['near_draws'] or counts['far_draws']:
                raise RuntimeError('octree overlay survived return to BSP')
        run.command('bind F12 quit')
        os.write(run.write_fd, b'k')
        run.process.wait(timeout=15)
        if run.process.returncode:
            raise RuntimeError(f'exit status {run.process.returncode}')
        if not args.multiplayer and '4 AA Samples' not in (work / 'process.log').read_text(errors='replace'):
            raise RuntimeError('4x MSAA unavailable')
        report['success'] = True
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        if run:
            run.close()
        if args.validation:
            log = (work / 'process.log').read_text(errors='replace')
            active = 'Using VK_LAYER_KHRONOS_validation' in log
            errors = re.findall(r'^.*(?:VUID-|Validation Error).*$', log, re.M)
            report['validation'] = dict(active=active, errors=errors[:16])
            if not active or errors:
                report['success'] = False
        (work / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: report[k] for k in ('work', 'success', 'error') if k in report}), flush=True)
    if not report['success']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
