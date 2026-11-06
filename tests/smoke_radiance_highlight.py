#!/usr/bin/env python3
"""Capture same-frame Radiance highlight on/off and classic parity in an isolated game."""
import argparse
from pathlib import Path
import re
import shutil
import tempfile
import time

from PIL import Image, ImageChops
import bench_meshlets as bench


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height = 640, 360
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error('no registered Quake PAK files found')
    work = Path(tempfile.mkdtemp(prefix='quiver-highlight-smoke-'))
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    (work / 'id1' / 'boot.cfg').write_text(bench.BOOT.replace('map start', 'host_maxfps 30\nmap start'))
    engine = bench.Engine(args, work)

    def photo(label):
        start = len(engine.text())
        engine.cmd('screenshot png')
        deadline = time.monotonic() + args.timeout
        retry = time.monotonic() + 3
        while time.monotonic() < deadline:
            match = re.search(r'Wrote [\x01\x02]?([^\n]+\.png)', engine.text()[start:])
            if match:
                image = next(work.rglob(match[1]), None)
                if image:
                    target = work / (label + '.png')
                    shutil.copy2(image, target)
                    return target
            if engine.process.poll() is not None:
                raise RuntimeError(f'engine exited; inspect {work}')
            if time.monotonic() >= retry:
                engine.cmd('screenshot png')
                retry = time.monotonic() + 3
            time.sleep(.1)
        raise RuntimeError(f'screenshot timed out; inspect {work}')

    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.view('e1m1', '480 -352 88 0 90 0')
        engine.cmd('r_radiance_backend 1\nr_radiance_quality 0\nr_radiance_point_gain 10\n'
                   'r_radiance_emissive_gain 0\nr_radiance_bounce_gain 0\nr_radiance_highlight 0\nr_radiance 1')
        status = engine.cmd('wait\nwait\nradiance_stats')
        if not re.search(r'Radiance: requested=1 active=1\b', status):
            raise RuntimeError(f'radiance not active: {status}')
        time.sleep(2)
        off = photo('radiance-highlight-off')
        engine.cmd('r_radiance_highlight 1\nwait\nwait')
        time.sleep(1)
        on = photo('radiance-highlight-on')
        engine.cmd('r_radiance 0\nr_radiance_highlight 0\nwait\nwait')
        time.sleep(1)
        classic_off = photo('classic-highlight-off')
        engine.cmd('r_radiance_highlight 1\nwait\nwait')
        time.sleep(1)
        classic_on = photo('classic-highlight-on')
        engine.cmd('r_radiance 1\nr_fullbright 1\nr_radiance_highlight 0\nwait\nwait')
        time.sleep(1)
        fullbright_off = photo('fullbright-highlight-off')
        engine.cmd('r_radiance_highlight 1\nwait\nwait')
        time.sleep(1)
        fullbright_on = photo('fullbright-highlight-on')
        # Force a persistent contents tint while the HDR pass is running.
        # It must disappear completely when gl_polyblend is disabled.
        engine.cmd('r_fullbright 0\ngl_polyblend 0\nv_cshift 255 0 0 160\nwait\nwait')
        time.sleep(1)
        tint_disabled = photo('hdr-polyblend-disabled-red')
        engine.cmd('v_cshift 0 0 0 0\nwait\nwait')
        time.sleep(1)
        tint_clear = photo('hdr-polyblend-disabled-clear')
        engine.cmd('gl_polyblend 1\nv_cshift 255 0 0 160\nwait\nwait')
        time.sleep(1)
        tint_enabled = photo('hdr-polyblend-enabled-red')
        engine.cmd('v_cshift 0 0 0 0\nvid_palettize 1\nwait\nwait')
        time.sleep(1)
        palette = photo('hdr-palettized')
        with Image.open(off) as a, Image.open(on) as b:
            a, b = a.convert('RGB'), b.convert('RGB')
            old, new = list(a.getdata()), list(b.getdata())
            clipped = sum(max(x) >= 250 for x in old)
            reduced = sum(max(x) >= 250 and max(y) <= max(x) - 4 for x, y in zip(old, new))
            print(f'Radiance image: {clipped} near-clipped pixels, {reduced} reduced by >=4 levels', flush=True)
            if reduced < 10:
                raise RuntimeError(f'no measurable clipped-highlight improvement; inspect {work}')
        with Image.open(classic_off) as a, Image.open(classic_on) as b:
            if ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox():
                raise RuntimeError(f'classic frame changed when highlight setting changed; inspect {work}')
        with Image.open(fullbright_off) as a, Image.open(fullbright_on) as b:
            if ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox():
                raise RuntimeError(f'fullbright frame changed when highlight setting changed; inspect {work}')
        with Image.open(off) as a, Image.open(fullbright_off) as b:
            if not ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox():
                raise RuntimeError(f'fullbright fixture did not change the rendered scene; inspect {work}')
        with Image.open(tint_disabled) as a, Image.open(tint_clear) as b:
            if ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox():
                raise RuntimeError(f'disabled polyblend changed HDR frame; inspect {work}')
        with Image.open(tint_enabled) as a, Image.open(tint_clear) as b:
            if not ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox():
                raise RuntimeError(f'polyblend tint fixture did not affect HDR frame; inspect {work}')
        with Image.open(palette) as a, Image.open(tint_clear) as b:
            if not ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox():
                raise RuntimeError(f'palettization fixture did not affect HDR frame; inspect {work}')
        if args.validation:
            output = engine.text() + (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in output or re.search(r'VUID-|Validation Error|SYNC-HAZARD|hazard detected', output, re.I):
                raise RuntimeError(f'validation missing or reported errors; inspect {work}')
        print(f'PASS: {off}, {on}; classic and debug-fullbright toggle parity', flush=True)
    finally:
        engine.close()


if __name__ == '__main__':
    main()
