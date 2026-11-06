#!/usr/bin/env python3
"""Compare instanced and legacy ripple rendering with an active dynamic light.

Uses a disposable QuakeC game and BSP derived from the packaged stress map.
The test-only QuakeC spawns a stationary rocket, whose model effect supplies
a stable-radius dynamic light. Linux /proc reads verify that light actually
reached the client; no engine memory is modified. Requires an unstripped
engine, bundled build tools, local Quake PAKs, Pillow, and a Vulkan display.
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

import bench_ecs_ripple as bench_ripple
import bench_meshlets as bench

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
entity ripple_test_light;
void() quiver_map_start = {
    lightstyle(0, "c");
    ripple_test_light = spawn();
    ripple_test_light.classname = "ripple_test_light";
    setmodel(ripple_test_light, "progs/missile.mdl");
    setorigin(ripple_test_light, '-128 0 160');
    ripple_test_light.movetype = MOVETYPE_NONE;
    ripple_test_light.solid = SOLID_NOT;
};
'''


def add_script_marker(path):
    data = bytearray((ROOT / 'Misc/vq_pak/maps/ecs32768.bsp').read_bytes())
    offset, length = struct.unpack_from('<ii', data, 4)
    entities = bytes(data[offset:offset + length]).rstrip(b'\0')
    first = entities.index(b'{') + 1
    entities = entities[:first] + b'\n"_quiver_script" "scripts/light_test.qs"\n' + entities[first:] + b'\0'
    struct.pack_into('<ii', data, 4, len(data), len(entities))
    data.extend(entities)
    path.write_bytes(data)


def build_game(work, tool, qcc, source):
    qc = work / 'qc'
    shutil.copytree(source, qc)
    manifest = qc / 'progs.src'
    lines = manifest.read_text().splitlines()
    lines[0] = '../quiver_light/progs.dat'
    manifest.write_text('\n'.join(lines) + '\nripple.qc\nlight_probe.qc\n')
    subprocess.run([str(tool), 'transpile', str(ROOT / 'Misc/scripts/ecs_ripple.qs'), str(qc / 'ripple.qc')], check=True)
    (qc / 'light_probe.qc').write_text(PROBE)
    result = subprocess.run([str(qcc), '-O0'], cwd=qc, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError('QuakeC fixture build failed:\n' + result.stdout)


def get_symbol(binary, name):
    result = subprocess.run(['nm', '-an', str(binary)], check=True, text=True, stdout=subprocess.PIPE)
    matches = re.findall(r'^([0-9a-fA-F]+)\s+[bBdD]\s+' + re.escape(name) + r'$', result.stdout, re.M)
    if len(matches) != 1:
        raise RuntimeError(f'{binary} has no unique {name} symbol')
    return int(matches[0], 16)


def check_dynamic_light(engine, binary):
    # FrameCounter already resolves the ELF load address, including PIE.
    counter = bench_ripple.FrameCounter(engine.process.pid, binary)
    try:
        base = counter.address - get_symbol(binary, 'host_framecount')
        address = base + get_symbol(binary, 'cl_dlights')
        # dlight_t is 64 bytes: seven floats, one int, then eight floats.
        data = os.pread(counter.fd, 64 * 64, address)
        if len(data) != 64 * 64:
            raise RuntimeError('could not read complete dlight array')
        for i in range(64):
            values = struct.unpack_from('=7fi8f', data, i * 64)
            if all(abs(a - b) < .01 for a, b in zip(values[:3], (-128, 0, 160))) and abs(values[3] - 200) < .01:
                if values[4] <= 0 or max(values[8:11]) <= 0:
                    raise RuntimeError(f'fixture light is inactive: {values}')
                return dict(index=i, origin=values[:3], radius=values[3], die=values[4], color=values[8:11])
        raise RuntimeError('stationary rocket dynamic light missing from client')
    finally:
        counter.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--quakec-source', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--gpu-cull', action='store_true', help='compare GPU and CPU instance culling with the same active light')
    args = parser.parse_args()
    args.binary = args.binary.resolve(); args.width, args.height = 1280, 720; args.game = 'quiver_light'
    source = args.basedir.resolve()
    if (source / 'id1').is_dir(): source /= 'id1'
    paks = sorted(x for x in source.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: parser.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-dynamic-light-parity-')).resolve()
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1/maps').mkdir(parents=True); (work / args.game).mkdir(); (work / 'user').mkdir()
    for pak in paks: (work / 'id1' / pak.name.lower()).symlink_to(pak)
    add_script_marker(work / 'id1/maps/light_test.bsp')
    build_game(work, args.tool.resolve(), args.compiler.resolve(), args.quakec_source.resolve())
    boot = bench.BOOT.replace('map start', 'r_rtshadows 0\nr_particles 0\nmax_edicts 32800\nhost_maxfps 30\nmap light_test')
    (work / 'id1/boot.cfg').write_text(boot)
    engine = bench.Engine(args, work)
    try:
        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        deadline = time.monotonic() + args.timeout
        while True:
            try:
                bench_ripple.check_state(engine, indices=(0, 32767))
                light = check_dynamic_light(engine, args.binary)
                break
            except RuntimeError:
                if time.monotonic() >= deadline: raise
                time.sleep(.1)
        engine.cmd('setpos -480 0 32 0 0 0\nr_drawviewmodel 0\nr_particles 0\npause')
        time.sleep(.2)
        check_dynamic_light(engine, args.binary)
        captures = []
        for value in (0, 1):
            if args.gpu_cull:
                engine.cmd(f'r_ecs_batch 1\nr_ecs_gpu_cull {value}')
            else:
                engine.cmd(f'r_ecs_batch {value}')
            time.sleep(.15)
            state = engine.cmd('ecs_visual_stats')
            expected_batch = 1 if args.gpu_cull else value
            if not re.search(rf'\blocal=32768\b.*\bbatched={expected_batch}\b', state):
                raise RuntimeError(f'batch switch did not take effect: {state}')
            if args.gpu_cull:
                status = engine.cmd('ecs_gpu_stats')
                if not re.search(rf'enabled={value}\s+available=1\s+active={value}\b', status):
                    raise RuntimeError(f'GPU cull path did not take effect: {status}')
            check_dynamic_light(engine, args.binary)
            captures.append(engine.photo(f'dynamic-light-{("gpu" if args.gpu_cull else "batch")}-{value}'))
        with Image.open(captures[0]) as legacy, Image.open(captures[1]) as batched:
            delta = ImageChops.difference(legacy.convert('RGB'), batched.convert('RGB'))
            # The same paused scene and light must have exactly matching pixels.
            bbox = delta.getbbox()
            if bbox:
                raise RuntimeError(f'dynamic-light render differs in {bbox}: {captures}')
        if args.validation:
            log = (work / 'output.log').read_text(errors='replace')
            if 'Using VK_LAYER_KHRONOS_validation' not in log or re.search(r'VUID-|Validation Error|SYNC-HAZARD', log):
                raise RuntimeError('validation layer missing or reported errors')
        print(f'PASS: 32768 ripple entities, active dynamic light {light}, paused '
              f'{"GPU/CPU cull" if args.gpu_cull else "batch/legacy"} pixels identical; '
              f'screenshots {captures}', flush=True)
    finally:
        if engine.process.poll() is None:
            engine.process.terminate()
            engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__':
    main()
