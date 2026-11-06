#!/usr/bin/env python3
"""Exercise actual lazy soft-post submissions and hard fallback under validation."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import threading
import time
from types import SimpleNamespace

import bench_meshlets as bench
import bench_radiance as rc
import bench_radiance_soft_shadows as soft
from bench_ecs_ripple import FrameCounter

POSE=(607,143,40,3,91,0)


def expected(scale, filtering, checker, enabled, indirect):
    reduced=scale!=1 and not indirect
    return {'reduced':reduced,'post':bool(reduced and filtering=='texture' and enabled),
            'scale':scale if reduced else 1,'filter':filtering if reduced else 'full',
            'mode':(int(scale!=1),int(reduced),int(checker),int(checker and reduced))}


def verify_live(text, backend, scale, filtering, checker, enabled, indirect):
    want=expected(scale,filtering,checker,enabled,indirect)
    rc.verify_state(text,'radiance',backend,0);rc.verify_pose(text,POSE)
    rc.verify_probe_filter(text,0);rc.verify_hold(text);rc.verify_receiver_diagnostics_off(text)
    soft.verify_soft(text,enabled,2,.5,want['post'])
    mode=rc.RECEIVER_STATE.search(text);size=rc.RECEIVER_SCALE.search(text);filt=rc.RECEIVER_FILTER.search(text)
    if not mode or tuple(map(int,mode.groups()))!=want['mode']:
        raise RuntimeError('requested/active checker or receiver mode differs')
    if not filt or filt.groups()!=(filtering,want['filter']):raise RuntimeError('active filter differs')
    values=(scale,want['scale'],rc.receiver_divisor(want['scale']))
    if not size or any(not math.isclose(float(size[i+1]),v,rel_tol=1e-5,abs_tol=1e-7) for i,v in enumerate(values)):
        raise RuntimeError('requested/active receiver scale differs')
    for name,value in (('r_indirect',indirect),('vid_fsaa',1),('fov',90),('r_lightmap',1),('r_radiance_highlight',0),
                       ('r_radiance_quality',0),('r_radiance_grid',1)):
        soft.verify_cvar(text,name,value)
    want['sources']=rc.verify_sources(text,1,1,0)
    return want


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--basedir',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--backends',nargs='+',choices=('hardware','software'),default=('hardware','software'))
    p.add_argument('--validation',action='store_true');p.add_argument('--allow-existing',action='store_true')
    p.add_argument('--timeout',type=float,default=30)
    args=p.parse_args()
    if not 0<args.timeout<=35:p.error('timeout must be in(0,35] seconds')
    rc.ensure_no_game(args.allow_existing)
    root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
    frozen=root/'binary/vkquake';frozen.parent.mkdir();shutil.copy2(args.binary.resolve(),frozen)
    source=args.basedir.resolve();source=source/'id1' if (source/'id1').is_dir() else source
    paks=sorted(source.glob('pak*.pak'))
    if not paks:p.error('Quake PAK files required')
    (root/'id1').mkdir();(root/'user').mkdir()
    for pak in paks:(root/'id1'/pak.name).symlink_to(pak)
    (root/'harness').mkdir()
    for name in (Path(__file__).name,'bench_radiance_soft_shadows.py','bench_radiance.py','bench_meshlets.py','bench_ecs_ripple.py'):
        shutil.copy2(Path(__file__).with_name(name),root/'harness'/name)
    report={'complete':False,'checks':[],'binary_sha256':rc.sha256(frozen),
            'assets':{str(pak):rc.sha256(pak) for pak in paks},'viewport':[641,361],
            'harness_sha256':{q.name:rc.sha256(q) for q in (root/'harness').iterdir()},
            'validation_requested':args.validation,'optional_stage_queries':False,
            'validation_environment':{k:os.environ.get(k) for k in ('VK_LAYER_PATH','VK_INSTANCE_LAYERS','VK_LAYER_ENABLES','VK_LAYER_VALIDATE_SYNC')}}
    def save():(root/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    first=2 if args.backends[0]=='hardware' else 1
    startup=(f'host_timescale .000001\nscr_centertime -1\nr_rtshadows 0\nr_radiance 1\nr_radiance_backend {first}\n'
        'r_radiance_quality 0\nr_radiance_grid 1\nr_radiance_point_gain 1\nr_radiance_emissive_gain 1\nr_radiance_bounce_gain 0\n'
        'r_radiance_resolution_scale .666666667\nr_radiance_receiver_filter 1\nr_radiance_checkerboard 0\n'
        'r_radiance_receiver_diagnostics 0\nr_radiance_probe_gaussian 0\nr_radiance_shadow_radius 2\nr_radiance_shadow_sun_angle .5\n'
        'r_lightmap 1\nr_radiance_highlight 0\nfov 90\nviewsize 130\nr_drawviewmodel 0\ncrosshair 0\ncl_bob 0\ncl_rollangle 0\n'
        'echo RC_SOFT_CAPTURE_BOOT\nmap e1m1')
    (root/'id1/boot.cfg').write_text(bench.BOOT.replace('vid_fsaa 0','vid_fsaa 1').replace('r_indirect 1','r_indirect 0').replace('map start',startup))
    temporary,alias=soft.short_binary_alias(frozen);bench.SHIM=soft.isolated_poll_source(bench.SHIM)
    config=SimpleNamespace(binary=alias,width=641,height=361,timeout=args.timeout,validation=args.validation,game=None)
    engine=bench.Engine(config,root);counter=None;stop=threading.Event();deadline=[time.monotonic()+35]
    def watchdog():
        while not stop.wait(.1):
            if time.monotonic()>deadline[0] and engine.process.poll() is None:
                report['own_process_watchdog']={'pid':engine.process.pid,'limit_s':35}
                engine.process.kill();return
    thread=threading.Thread(target=watchdog,daemon=True);thread.start()
    def cmd(body):deadline[0]=time.monotonic()+35;return engine.cmd(body)
    sources=None
    def settled(backend,spec):
        end=time.monotonic()+args.timeout
        while True:
            text=cmd(soft.QUERY)
            report.setdefault('current_attempt',{})['last_state']=text
            try:
                want=verify_live(text,backend,*spec)
                if not rc.radiance_ready(text):raise RuntimeError('receiver not ready')
                return text,want
            except RuntimeError:
                if time.monotonic()>end:raise
                time.sleep(.05)
    states=[('off',2/3,'texture',0,0,0),('on',2/3,'texture',0,1,0),
        ('checker',2/3,'texture',1,1,0),('quarter',.25,'texture',1,1,0),
        ('full',1,'texture',0,1,0),('probe',2/3,'probe',1,1,0),
        ('texture-restored',2/3,'texture',1,1,0),('indirect-fallback',2/3,'texture',1,1,1),
        ('indirect-restored',2/3,'texture',1,1,0),('off-final',2/3,'texture',1,0,0)]
    try:
        try:
            engine.wait(soft.boot_executed);engine.wait(lambda t:'CL_SignonReply: 4' in t)
            soft.verify_cvar(cmd('r_radiance_soft_shadows'),'r_radiance_soft_shadows',0)
            counter=FrameCounter(engine.process.pid,frozen);clock=rc.SimulationClock(counter,frozen)
            cmd('setpos '+' '.join(map(str,POSE)))
            for backend in args.backends:
                cmd(f'r_radiance_backend {2 if backend=="hardware" else 1}')
                for label,*spec in states:
                    scale,filtering,checker,enabled,indirect=spec
                    report['current_attempt']={'backend':backend,'label':label,'requested':spec,'verified':False};save()
                    cmd(f'r_indirect {indirect}\nr_radiance_resolution_scale {scale:.12g}\n'
                        f'r_radiance_receiver_filter {int(filtering=="texture")}\nr_radiance_checkerboard {checker}\nr_radiance_soft_shadows {enabled}')
                    settled(backend,spec);time.sleep(.2);before,want=settled(backend,spec)
                    report['current_attempt'].update(before=before,expected=want);save()
                    start_clock=clock.sample();time.sleep(.25)
                    capture=None
                    if label=='indirect-restored':
                        capture=engine.photo(backend+'-soft-checker');soft.viewport(capture,641,361)
                    after,final=settled(backend,spec);end_clock=clock.sample()
                    report['current_attempt'].update(after=after,simulation_start=start_clock,simulation_end=end_clock);save()
                    if not 0<=end_clock-start_clock<.001:raise RuntimeError('simulation advanced')
                    if sources is not None and (want['sources']!=sources or final['sources']!=sources):raise RuntimeError('source counts changed')
                    sources=want['sources']
                    row={'backend':backend,'label':label,'requested':spec,'expected':want,'before':before,'after':after,
                        'simulation_start':start_clock,'simulation_end':end_clock,
                        'soft_shadow_frames':soft.verify_soft_frames(soft.soft_frames(before),soft.soft_frames(after),want['post']),
                        'receiver_frames':rc.verify_receiver_frames(rc.receiver_frames(before),rc.receiver_frames(after),want['reduced'])}
                    if capture:row['capture']=str(capture)
                    report['checks'].append(row);report.pop('current_attempt',None);save();print('PASS '+backend+' '+label,flush=True)
        finally:
            if counter:counter.close()
            if engine.process.poll() is None:time.sleep(5)
            try:engine.close()
            finally:stop.set();thread.join(timeout=1);temporary.cleanup()
            log=(root/'output.log').read_text(errors='replace')
            report.update(engine_exit_code=engine.process.returncode,driver_errors=soft.DRIVER_ERRORS.findall(log),
                validation_errors=soft.ERRORS.findall(log),binary_unchanged=rc.sha256(frozen)==report['binary_sha256'])
            report['validation_settings_present']='VALIDATION-SETTINGS' in log;save()
        if report['engine_exit_code'] or report['driver_errors'] or report['validation_errors']:raise RuntimeError('renderer/driver/validation failed')
        if args.validation and not report['validation_settings_present']:raise RuntimeError('SDK validation settings unavailable')
        report['complete']=True;save()
    except Exception as exc:
        report['failure']={'type':type(exc).__name__,'message':str(exc)};save();raise


if __name__=='__main__':main()
