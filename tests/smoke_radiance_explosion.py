#!/usr/bin/env python3
"""Exercise TE_EXPLOSION dynamic lighting in a disposable game directory.

The fixture broadcasts explosions at one fixed point while a test cvar is
set. It checks the actual client light and compares paused captures with the
explosions enabled and disabled. No game or user configuration is modified.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

from PIL import Image, ImageChops

import bench_meshlets as bench
import bench_ecs_ripple as bench_ripple
import smoke_ecs_batch_dynamic_light as old_fixture

ROOT = Path(__file__).resolve().parents[1]
POSITION = (-128., 0., 160.)
PROBE = r'''
entity explosion_probe;
void() explosion_probe_think = {
    if (cvar("r_test_explosion")) {
        WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        WriteByte(MSG_BROADCAST, TE_EXPLOSION);
        WriteCoord(MSG_BROADCAST, -128);
        WriteCoord(MSG_BROADCAST, 0);
        WriteCoord(MSG_BROADCAST, 160);
    }
    self.nextthink = time + 0.1;
};
void() quiver_map_start = {
    lightstyle(0, "c");
    explosion_probe = spawn();
    explosion_probe.classname = "explosion_probe";
    explosion_probe.think = explosion_probe_think;
    explosion_probe.nextthink = time + 0.1;
};
'''


def make_map(path):
    data = bytearray((ROOT / 'Misc/vq_pak/maps/ecs1024.bsp').read_bytes())
    offset, length = struct.unpack_from('<ii', data, 4)
    entities = bytes(data[offset:offset + length]).rstrip(b'\0')
    first = entities.index(b'{') + 1
    entities = entities[:first] + b'\n"_quiver_script" "scripts/light_test.qs"\n' + entities[first:] + b'\0'
    struct.pack_into('<ii', data, 4, len(data), len(entities))
    data.extend(entities)
    path.write_bytes(data)


def build_game(work, tool, compiler, source):
    qc = work / 'qc'
    shutil.copytree(source, qc)
    manifest = qc / 'progs.src'
    lines = manifest.read_text().splitlines()
    lines[0] = '../quiver_explosion/progs.dat'
    manifest.write_text('\n'.join(lines) + '\nripple.qc\nexplosion_probe.qc\n')
    subprocess.run([str(tool), 'transpile', str(ROOT / 'Misc/scripts/ecs_ripple.qs'), str(qc / 'ripple.qc')], check=True)
    (qc / 'explosion_probe.qc').write_text(PROBE)
    result = subprocess.run([str(compiler), '-O0'], cwd=qc, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError('QuakeC fixture build failed:\n' + result.stdout)


def find_explosion(engine, binary):
    counter = bench_ripple.FrameCounter(engine.process.pid, binary)
    try:
        base = counter.address - old_fixture.get_symbol(binary, 'host_framecount')
        address = base + old_fixture.get_symbol(binary, 'cl_dlights')
        data = os.pread(counter.fd, 64 * 64, address)
        if len(data) != 64 * 64:
            raise RuntimeError('could not read complete client light array')
        for index in range(64):
            values = struct.unpack_from('=7fi8f', data, index * 64)
            if (all(abs(a - b) < .01 for a, b in zip(values[:3], POSITION)) and
                    200.0 < values[3] <= 350.0 and max(values[8:11]) > 0):
                return dict(index=index, radius=values[3], die=values[4], color=values[8:11])
        return None
    finally:
        counter.close()


def brightness_difference(off_path, on_path):
    with Image.open(off_path) as off, Image.open(on_path) as on:
        before, after = off.convert('RGB'), on.convert('RGB')
        if before.size != after.size:
            raise RuntimeError('capture sizes differ')
        # Exclude HUD and crosshair. Both images are paused; positive changes
        # must come from the fixture light on scene surfaces, not particles.
        width, height = before.size
        box = (0, 0, width, int(height * .85))
        before, after = before.crop(box), after.crop(box)
        positive = ImageChops.subtract(after.convert('L'), before.convert('L'))
        hist = positive.histogram()
        return sum(hist[8:]), sum(value * count for value, count in enumerate(hist))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--tool', type=Path, default=ROOT / 'build/quiverscript')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'build/fteqcc')
    parser.add_argument('--quakec-source', type=Path, default=ROOT / 'ThirdParty/id1-quakec')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.width, args.height, args.game = 640, 360, 'quiver_explosion'
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-radiance-explosion-')).resolve()
    print(f'Isolated test directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / args.game).mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    make_map(work / 'id1/maps/light_test.bsp')
    build_game(work, args.tool.resolve(), args.compiler.resolve(), args.quakec_source.resolve())
    boot = bench.BOOT.replace('map start', 'r_particles 0\nr_fteparticles 0\nset r_test_explosion 0\n'
        'r_rtshadows 0\nmax_edicts 32800\nhost_maxfps 20\nmap light_test')
    (work / 'id1/boot.cfg').write_text(boot)
    engine = bench.Engine(args, work)
    try:
        os.setpriority(os.PRIO_PROCESS, engine.process.pid, 15)
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        engine.cmd('r_radiance_backend 1\nr_radiance 1\nr_radiance_bounce_gain 0\n'
                   'r_radiance_emissive_gain 0\nr_radiance_point_gain 1\n'
                   'r_drawviewmodel 0\ncrosshair 0\ncl_bob 0\ncl_rollangle 0\nsetpos -480 0 32 0 0 0')
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if re.search(r'Radiance: requested=1 active=1\b', engine.cmd('radiance_stats')): break
            time.sleep(.1)
        else: raise RuntimeError('radiance path did not activate')
        for name, mode in (('radiance', 1), ('classic', 0)):
            engine.cmd(f'r_radiance {mode}\nset r_test_explosion 0')
            time.sleep(2)  # allow previously emitted lights to expire
            engine.cmd('pause')
            off = engine.photo(name + '-off')
            engine.cmd('pause\nset r_test_explosion 1')
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                light = find_explosion(engine, args.binary)
                if light: break
                time.sleep(.05)
            else: raise RuntimeError('TE_EXPLOSION client light did not appear')
            engine.cmd('pause')
            on = engine.photo(name + '-on')
            count, energy = brightness_difference(off, on)
            print(f'{name}: client explosion light {light}; brightened pixels={count} energy={energy}', flush=True)
            if count < 100 or energy < 1000:
                raise RuntimeError(f'{name} explosion did not visibly illuminate the scene: {off}, {on}')
            engine.cmd('set r_test_explosion 0\npause')
        if args.validation:
            output = engine.text() + (work / 'output.log').read_text(errors='replace')
            if ('Using VK_LAYER_KHRONOS_validation' not in output or
                    re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', output)):
                raise RuntimeError(f'validation missing or reported errors; inspect {work}')
        print('PASS: classic and radiance paths visibly receive deterministic TE_EXPLOSION light', flush=True)
    finally:
        engine.close()


if __name__ == '__main__': main()
