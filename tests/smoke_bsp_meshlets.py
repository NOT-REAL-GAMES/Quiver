#!/usr/bin/env python3
"""Check meshlet rendering on stock id1 BSP special passes and resource transitions.

Uses bench_meshlets' isolated client/command injector. Requires the registered
id1 assets, SDL development headers, Pillow and a Vulkan display. This smoke
does not measure performance. Positive controls ensure the chosen world
surface/brush entity actually contributes to the reference image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile

from bench_meshlets import BOOT, Engine, STATE, delta


def validation_report(output):
    errors = [line for line in output.splitlines()
              if re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', line, re.I)]
    return dict(active='Using VK_LAYER_KHRONOS_validation' in output,
                sync_active='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT' in output,
                error_count=len(errors), errors=errors[:24])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--no-rt-shadows', action='store_true',
                        help='isolate BSP hazards from the stock acceleration-structure scratch path')
    parser.add_argument('--width', type=int, default=800)
    parser.add_argument('--height', type=int, default=600)
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    args.stock_only = False
    args.binary = args.binary.resolve()
    base = args.basedir.resolve()
    source = next((p for p in base.iterdir() if p.is_dir() and p.name.lower() == 'id1'), base)
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake pak files in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-meshlet-smoke-'))
    print(work, flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for p in paks:
        (work / 'id1' / p.name.lower()).symlink_to(p)
    # Load with clustering enabled, then switch live on the same map as well.
    rt_setting = 'r_rtshadows 0\n' if args.no_rt_shadows else ''
    (work / 'id1/boot.cfg').write_text(BOOT.replace('map start', rt_setting + 'r_meshlets 1\nmap start'))
    report = dict(success=False, work=str(work), binary=str(args.binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  rt_shadows_disabled=args.no_rt_shadows, checks={})
    engine = None

    def ab(label, fallback=False):
        entry = report['checks'][label] = {}
        for mode in (0, 1):
            result = engine.backend(mode, verify=not fallback)
            entry[f'backend_{mode}'] = result
            if fallback:
                state = STATE.search(result)
                if not state or tuple(map(int, state.groups())) != (0, mode, 0):
                    raise RuntimeError(f'{label}: expected explicit stock fallback, got {result}')
            path = engine.photo(label + ('_stock' if mode == 0 else '_meshlets'))
            if mode == 0:
                reference = path
                control = entry['stock_repeat'] = delta(reference, engine.photo(label + '_repeat'))
                if control['mean_rgb'] > .25 or control['percent_over_4'] > .6:
                    raise RuntimeError(f'{label}: stock camera did not freeze ({control})')
            else:
                change = entry['candidate'] = delta(reference, path)
                if (change['mean_rgb'] > control['mean_rgb'] + .10 or
                        change['percent_over_4'] > control['percent_over_4'] + .20):
                    raise RuntimeError(f'{label}: difference exceeds stock repeat ({change}, control {control})')
        return reference

    def control(label, reference, disable, restore):
        engine.cmd(disable + '\nwait\nwait')
        change = delta(reference, engine.photo(label + '_control'))
        report['checks'][label]['positive_control'] = change
        if change['mean_rgb'] <= .5:
            raise RuntimeError(f'{label}: selected feature not visible ({change})')
        engine.cmd(restore + '\nwait\nwait')

    try:
        engine = Engine(args, work)
        engine.wait(lambda t: 'CL_SignonReply: 4' in t)
        engine.cmd('host_maxfps 60\nr_flatlightstyles 1\nr_waterwarp 0')
        # This pose looks directly at the brush-model door04_2 in start.
        engine.view('start', '-400 2400 138 0 0 0')
        door = ab('brush_door')
        control('brush_door', door, 'r_drawentities 0', 'r_drawentities 1')
        engine.cmd('r_showtris 1\nwait\nwait')
        wire = ab('wireframe')
        control('wireframe', wire, 'r_showtris 0', 'r_showtris 1')
        engine.cmd('r_showtris 2\nwait\nwait')
        depth_wire = ab('depth_wireframe')
        change = delta(wire, depth_wire)
        report['checks']['depth_wireframe']['vs_xray'] = change
        if change['mean_rgb'] <= .1:
            raise RuntimeError('wire modes did not produce distinct depth-tested edges')
        engine.cmd('r_showtris 0')
        # Water and sky reference views are in different BSP leaves; reload
        # with meshlets enabled to exercise the baked buffers' map lifetime.
        engine.view('e1m1', '869 910 -190 59 180 0')
        engine.cmd('r_novis 1\nr_wateralpha 0.35\nwait\nwait')
        water = ab('transparent_water')
        control('transparent_water', water, 'r_wateralpha 1', 'r_wateralpha 0.35')
        engine.cmd('r_novis 0\nr_wateralpha 1')
        engine.view('e1m2', '150 -8 666 -59 180 0')
        sky = ab('sky_portal')
        control('sky_portal', sky, 'r_fastsky 1', 'r_fastsky 0')

        engine.cmd('vid_fsaa 4\nvid_restart\nwait\nwait')
        ab('msaa_restart')
        engine.cmd('r_indirect 0\nwait\nwait')
        ab('no_indirect', fallback=True)
        engine.cmd('r_indirect 1\nr_gpulightmapupdate 0\nwait\nwait')
        ab('no_gpu_lightmaps', fallback=True)
        engine.cmd('r_gpulightmapupdate 1\nvid_fsaa 0\nvid_restart\nwait\nwait')
        ab('resume_indirect')
        report['success'] = True
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        if engine:
            engine.close()
            report['exit_code'] = engine.process.returncode
            if report['exit_code'] != 0:
                report['success'] = False
            output = (work / 'output.log').read_text(errors='replace')
            report['msaa_4x_confirmed'] = '4 AA Samples' in output
            if not report['msaa_4x_confirmed']:
                report['success'] = False
        if args.validation:
            output = (work / 'output.log').read_text(errors='replace') if (work / 'output.log').exists() else ''
            v = report['validation'] = validation_report(output)
            if v['error_count'] or not v['active'] or not v['sync_active']:
                report['success'] = False
        (work / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
        print({k: report[k] for k in ('work', 'success', 'error') if k in report}, flush=True)
    if not report['success']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
