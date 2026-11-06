#!/usr/bin/env python3
"""Compare a one-shot TE_EXPLOSION's direct and indirect light before it expires.

Uses a dark copy of the ECS test room with no ECS markers, particles or static
lights. Two isolated game processes take paused snapshots at comparable light
ages: bounce gain zero and bounce gain eight. World pixels black in the direct
capture but lit in the other capture are evidence of the explosion's indirect
light. Requires an unstripped binary, registered PAKs and a Vulkan display.
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

from PIL import Image
import bench_meshlets as bench
import bench_ecs_ripple as bench_ripple
import smoke_radiance_explosion as fixture
import smoke_ecs_batch_dynamic_light as symbols

ROOT = Path(__file__).resolve().parents[1]
fixture.PROBE = r'''
entity explosion_probe;
float explosion_fired;
void() explosion_probe_think = {
    if (cvar("r_test_explosion") && !explosion_fired) {
        WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        WriteByte(MSG_BROADCAST, TE_EXPLOSION);
        WriteCoord(MSG_BROADCAST, -128);
        WriteCoord(MSG_BROADCAST, 0);
        WriteCoord(MSG_BROADCAST, 160);
        explosion_fired = 1;
    }
    if (!cvar("r_test_explosion")) explosion_fired = 0;
    self.nextthink = time + 0.01;
};
void() quiver_map_start = {
    explosion_probe = spawn();
    explosion_probe.classname = "explosion_probe";
    explosion_probe.think = explosion_probe_think;
    explosion_probe.nextthink = time + 0.01;
};
'''


def client_time_offset(binary):
    # Query DWARF type information without attaching to the process. This keeps
    # the expiry check correct if client_state_t's layout changes.
    result = subprocess.run(['gdb', '-nx', '-batch', str(binary),
        '-ex', 'p/x &((client_state_t *)0)->time'], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    match = re.search(r'\$\d+\s*=\s*(0x[0-9a-fA-F]+)', result)
    if not match: raise RuntimeError(f'client time offset needs debug symbols: {result}')
    return int(match[1], 16)


class ExplosionReader:
    def __init__(self, engine, binary, time_offset, second_position=None):
        self.counter = bench_ripple.FrameCounter(engine.process.pid, binary)
        base = self.counter.address - symbols.get_symbol(binary, 'host_framecount')
        self.time_address = base + symbols.get_symbol(binary, 'cl') + time_offset
        self.lights_address = base + symbols.get_symbol(binary, 'cl_dlights')
        self.second_position = second_position

    def sample(self, position=None):
        fd = self.counter.fd
        time_data = os.pread(fd, 8, self.time_address)
        light_data = os.pread(fd, 64 * 64, self.lights_address)
        if len(time_data) != 8 or len(light_data) != 64 * 64:
            raise RuntimeError('could not read client light/time')
        now = struct.unpack('=d', time_data)[0]
        for index in range(64):
            v = struct.unpack_from('=7fi8f', light_data, index * 64)
            if (all(abs(a-b) < .01 for a,b in zip(v[:3],position or fixture.POSITION)) and
                    200.0 < v[3] <= 350.0 and max(v[8:11]) > 0):
                return dict(index=index, radius=v[3], die=v[4], client_time=now,
                            remaining=v[4]-now, frame=self.counter.sample()[1])
        return None

    def close(self): self.counter.close()


def make_dark_map(path):
    fixture.make_map(path)
    data = bytearray(path.read_bytes())
    offset, length = struct.unpack_from('<ii', data, 4)
    entities = bytes(data[offset:offset + length])
    assert entities.count(b'"_quiver_ecs_stress" "1024"') == 1
    entities = entities.replace(b'"_quiver_ecs_stress" "1024"', b'"_quiver_ecs_stress" "0"')
    entities, count = re.subn(rb'\{\s*"classname"\s*"light"[^{}]*\}', b'', entities)
    assert count == 9
    struct.pack_into('<ii', data, 4, len(data), len(entities))
    data.extend(entities)
    path.write_bytes(data)


def capture(engine, label, commands='', reader=None):
    start = len(engine.text())
    engine.cmd(commands + '\nscreenshot png')
    # Sample before waiting for asynchronous PNG readback. CL_ReadFromServer
    # advances cl.time even while paused, so reading it after file completion
    # would not describe the frame for which screenshot was requested.
    at_request = reader.sample() if reader else None
    if reader and reader.second_position is not None:
        second = reader.sample(reader.second_position)
        if at_request is not None:
            at_request['second'] = second
        elif second is not None:
            at_request = {'remaining': -1, 'second': second}
    deadline = time.monotonic() + engine.args.timeout
    while time.monotonic() < deadline:
        match = re.search(r'Wrote [\x01\x02]?([^\n]+\.png)', engine.text()[start:])
        if match:
            original = next(engine.work.rglob(match[1]), None)
            if original:
                result = engine.work / (label + '.png')
                shutil.copy2(original, result)
                return result, at_request
        if engine.process.poll() is not None:
            raise RuntimeError(f'engine exited during capture; inspect {engine.work}')
        time.sleep(.05)
    raise RuntimeError(f'screenshot timed out; inspect {engine.work}')


def measure_images(baseline0, baseline8, direct, bounced):
    with Image.open(baseline0) as a, Image.open(baseline8) as z, Image.open(direct) as b, Image.open(bounced) as c:
        a, z, b, c = a.convert('L'), z.convert('L'), b.convert('L'), c.convert('L')
        if a.size != z.size or z.size != b.size or b.size != c.size: raise RuntimeError('capture sizes differ')
        w, h = a.size
        # HUD and PAUSED overlay are excluded from comparisons.
        masks = [(0, 0, w, int(h*.42)), (0, int(h*.58), w, int(h*.85))]
        changed = energy = shadow = baseline_difference = 0
        for box in masks:
            pa, pz, pb, pc = [im.crop(box).tobytes() for im in (a,z,b,c)]
            for before0, before8, direct_on, bounce_on in zip(pa,pz,pb,pc):
                baseline_difference += abs(before8 - before0)
                direct_change = direct_on - before0
                bounce_change = bounce_on - before8
                delta = bounce_change - direct_change
                if delta >= 4: changed += 1
                if delta > 0: energy += delta
                if direct_change <= 2 and bounce_change >= 5: shadow += 1
        return changed, energy, shadow, baseline_difference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--backend', choices=('software','hardware'), default='software')
    parser.add_argument('--tool', type=Path, default=ROOT/'build/quiverscript')
    parser.add_argument('--compiler', type=Path, default=ROOT/'build/fteqcc')
    parser.add_argument('--quakec-source', type=Path, default=ROOT/'ThirdParty/id1-quakec')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--trace-scheduling', action='store_true',
                        help='log radiance_stats on each live frame while the explosion is active')
    parser.add_argument('--live-frames', type=int, default=10,
                        help='simulation frames to wait before capture (must fit light lifetime)')
    parser.add_argument('--settle-frames', type=int, default=0,
                        help='also capture this many frames after the live screenshot to inspect persistence')
    parser.add_argument('--light', nargs=3, type=float, default=fixture.POSITION,
                        metavar=('X', 'Y', 'Z'), help='explosion position in world coordinates')
    parser.add_argument('--second-light', nargs=3, type=float, metavar=('X', 'Y', 'Z'),
                        help='fire a second simultaneous explosion at this position')
    parser.add_argument('--view', default='-480 0 32 0 0 0', help='setpos camera coordinates and angles')
    parser.add_argument('--timeout', type=float, default=180)
    args=parser.parse_args(); args.binary=args.binary.resolve()
    if args.live_frames < 0: parser.error('--live-frames must not be negative')
    fixture.POSITION = tuple(args.light)
    fixture.PROBE = fixture.PROBE.replace(
        'WriteCoord(MSG_BROADCAST, -128);\n        WriteCoord(MSG_BROADCAST, 0);\n        WriteCoord(MSG_BROADCAST, 160);',
        '\n        '.join(f'WriteCoord(MSG_BROADCAST, {value:g});' for value in args.light), 1)
    if args.second_light:
        primary = '\n        '.join(f'WriteCoord(MSG_BROADCAST, {value:g});' for value in args.light)
        second = ('\n        WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);\n'
                  '        WriteByte(MSG_BROADCAST, TE_EXPLOSION);\n        ' +
                  '\n        '.join(f'WriteCoord(MSG_BROADCAST, {value:g});' for value in args.second_light))
        fixture.PROBE = fixture.PROBE.replace(primary, primary + second, 1)
    time_offset = client_time_offset(args.binary)
    args.width,args.height,args.game=640,360,'quiver_explosion'
    source=args.basedir.resolve()
    if (source/'id1').is_dir(): source/= 'id1'
    paks=sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak',p.name.lower()))
    if not paks: parser.error(f'no Quake PAKs in {source}')
    parent=Path(tempfile.mkdtemp(prefix='quiver-explosion-bounce-')).resolve()
    print(f'Isolated test directory: {parent}',flush=True)
    images={}; lights={}
    for gain in (0,8):
        work=parent/f'gain-{gain}'; (work/'id1/maps').mkdir(parents=True); (work/args.game).mkdir(); (work/'user').mkdir()
        for pak in paks: (work/'id1'/pak.name.lower()).symlink_to(pak)
        make_dark_map(work/'id1/maps/light_test.bsp')
        fixture.build_game(work,args.tool.resolve(),args.compiler.resolve(),args.quakec_source.resolve())
        # Disable overlapping rendering tasks in the fixture: screenshot is a
        # one-shot flag, and a previous frame can otherwise consume it before
        # the requested frame reaches the readback path.
        boot = bench.BOOT.replace('r_tasks 1', 'r_tasks 0')
        (work/'id1/boot.cfg').write_text(boot.replace('map start','r_particles 0\nr_fteparticles 0\nset r_test_explosion 0\n'
            'r_rtshadows 0\nmax_edicts 4096\nhost_maxfps 60\nmap light_test'))
        engine=bench.Engine(args,work)
        try:
            os.setpriority(os.PRIO_PROCESS,engine.process.pid,15)
            engine.wait(lambda text:'CL_SignonReply: 4' in text)
            engine.cmd(f'r_radiance_backend {1 if args.backend == "software" else 2}\nr_radiance_quality 2\nr_radiance 1\n'
                f'r_radiance_sky_fallback 0\nr_radiance_bounce_gain {gain}\nr_radiance_emissive_gain 0\nr_radiance_point_gain 1\nr_radiance_highlight 0\n'
                f'r_drawviewmodel 0\ncrosshair 0\ncl_bob 0\ncl_rollangle 0\nhost_framerate 0.01\nsetpos {args.view}')
            deadline=time.monotonic()+args.timeout
            while time.monotonic()<deadline:
                status=engine.cmd('radiance_stats')
                if re.search(r'Radiance: requested=1 active=1\b',status): break
                time.sleep(.1)
            else: raise RuntimeError('radiance did not activate')
            if f'backend={"software" if args.backend == "software" else "ray-query"}' not in status: raise RuntimeError(status)
            time.sleep(2)
            engine.cmd('pause')
            images[f'baseline{gain}'],_=capture(engine,f'baseline-{gain}')
            reader=ExplosionReader(engine,args.binary,time_offset,args.second_light)
            try:
                engine.cmd('set r_test_explosion 1\npause')
                deadline=time.monotonic()+args.timeout
                while time.monotonic()<deadline:
                    light=reader.sample()
                    if light and light['remaining'] > 0: break
                    time.sleep(.001)
                else: raise RuntimeError('TE_EXPLOSION client light did not appear')
                # One shot is latched. Wait the requested live frames, then pause and
                # request the screenshot before its 0.5-second lifetime ends.
                live_frame = 'wait\nradiance_stats\n' if args.trace_scheduling else 'wait\n'
                images[f'on{gain}'],lights[gain]=capture(engine,f'explosion-{gain}',
                    'set r_test_explosion 0\n' + live_frame*args.live_frames + 'pause',reader)
                if args.settle_frames:
                    images[f'settled{gain}'],lights[f'settled{gain}'] = capture(engine, f'settled-{gain}',
                        'pause\n' + 'wait\n'*args.settle_frames + 'pause', reader)
            finally: reader.close()
            if not lights[gain] or lights[gain]['remaining'] <= 0:
                raise RuntimeError(f'light expired at screenshot request: {lights[gain]}')
            if args.second_light and (not lights[gain].get('second') or lights[gain]['second']['remaining'] <= 0):
                raise RuntimeError(f'second light missing or expired: {lights[gain]}')
            print(f'bounce gain {gain}: light radius {lights[gain]["radius"]:.3f}, '
                  f'remaining lifetime {lights[gain]["remaining"]:.3f} s, captures {images[f"on{gain}"]}',flush=True)
            if args.validation:
                output=engine.text()+(work/'output.log').read_text(errors='replace')
                if ('Using VK_LAYER_KHRONOS_validation' not in output or re.search(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard',output)):
                    raise RuntimeError(f'validation missing or reported errors; inspect {work}')
        finally: engine.close()
    if abs(lights[0]['radius']-lights[8]['radius'])>12: raise RuntimeError(f'explosion ages not comparable: {lights}')
    changed,energy,shadow,baseline_difference=measure_images(images['baseline0'],images['baseline8'],images['on0'],images['on8'])
    print(f'OFF baseline absolute luminance difference={baseline_difference}; '
          f'indirect difference: {changed} pixels >=4 levels; energy={energy}; '
          f'{shadow} pixels hidden from direct light',flush=True)
    if args.settle_frames:
        settled_changed, settled_energy, settled_shadow, _ = measure_images(
            images['baseline0'], images['baseline8'], images['settled0'], images['settled8'])
        print(f'settled light state: {lights["settled0"]}, {lights["settled8"]}; '
              f'indirect persistence: {settled_changed} pixels >=4 levels; energy={settled_energy}; '
              f'{settled_shadow} pixels hidden from direct light', flush=True)
        if any(lights[f'settled{gain}'] and lights[f'settled{gain}']['remaining'] > 0 for gain in (0,8)):
            raise RuntimeError('explosion still active at settled capture')
        if any(lights[f'settled{gain}'] and lights[f'settled{gain}'].get('second') and
               lights[f'settled{gain}']['second']['remaining'] > 0 for gain in (0,8)):
            raise RuntimeError('second explosion still active at settled capture')
        if settled_changed > 100 or settled_shadow > 10:
            raise RuntimeError(f'indirect light lingers after expiry; inspect {parent}')
    if baseline_difference > 1000:
        raise RuntimeError(f'explosion-OFF baselines differ; inspect {parent}')
    if changed<100 or energy<1000 or shadow<10:
        raise RuntimeError(f'no measurable short-lived indirect response; inspect {parent}')
    print('PASS: live explosion contributes indirect light before expiry',flush=True)

if __name__=='__main__': main()
