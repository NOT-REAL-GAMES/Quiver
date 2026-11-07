#!/usr/bin/env python3
"""Private held-camera shadow-filter captures or isolated paired timings.

Never sends signals to other games. Capture mode may explicitly share the GPU;
timing mode aborts if another game appears and loads no validation/stage queries.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import threading
import time
from types import SimpleNamespace

import bench_meshlets as bench
import bench_radiance as rc
from bench_ecs_ripple import FrameCounter
import bench_radiance_soft_shadows as soft

POSES = {'dm2': (1696,-1380,24,-8,90,0), 'ecs': (-480,0,32,-25,0,0),
         'sponza': (-16,-300,80,-5,90,0), 'doorway': (607,143,40,3,91,0)}
QUERY = (soft.QUERY+'\nr_radiance_shadow_filter\nr_radiance_soft_shadows\n'
         'r_radiance_shadow_radius\nr_radiance_shadow_sun_angle\nr_radiance_probe_spacing\nr_radiance_shadow_filter_radius\n'
         'r_radiance_batch_shadows\nr_radiance_deferred')


def verify_filter(output, strength, radius=None):
    soft.verify_cvar(output,'r_radiance_shadow_filter',strength)
    match=re.search(r'shadow filter requested=(\S+) active=(\S+)',output)
    try:
        actual=tuple(float(match[i]) for i in (1,2)) if match else ()
    except ValueError:
        actual=()
    if len(actual)!=2 or any(not math.isfinite(v) or abs(v-strength)>1e-6 for v in actual):
        raise RuntimeError('shadow filter requested/active state differs: '+output)
    if radius is not None:
        match=re.search(r'shadow filter requested=\S+ active=\S+ radius=(\S+) active_radius=(\S+)',output)
        try:actual=tuple(float(match[i]) for i in (1,2)) if match else ()
        except ValueError:actual=()
        expected=(radius,radius if strength>0 else 0)
        if len(actual)!=2 or any(not math.isfinite(v) or abs(v-e)>1e-6 for v,e in zip(actual,expected)):
            raise RuntimeError('shadow filter requested/active world radius differs: '+output)


def filter_frames(output):
    match=re.search(r'shadow filter frames active=(\d+) inactive=(\d+)',output)
    if not match:raise RuntimeError('actual shadow-filter submission counters unavailable')
    result={'active':int(match[1]),'inactive':int(match[2])}
    if any(not 0<=v<=0xffffffff for v in result.values()):raise RuntimeError('invalid shadow-filter uint32 counters')
    return result


def verify_filter_frames(before,after,strength):
    return soft.verify_soft_frames(filter_frames(before),filter_frames(after),strength>0)


def verify(output, args, pose, strength):
    rc.verify_state(output,'radiance','hardware',args.quality)
    rc.verify_receiver_state(output,rc.receiver_name(args.scale,args.checkerboard),args.scale,'texture',True,
                             full_texture=args.scale==1,checkerboard_requested=args.checkerboard)
    rc.verify_pose(output,pose);rc.verify_probe_filter(output,args.gaussian);rc.verify_hold(output)
    rc.verify_receiver_diagnostics_off(output);soft.verify_soft(output,True,args.soft_radius,.5,True)
    verify_filter(output,strength,args.filter_radius)
    for name,expected in (('r_lightmap',args.lightmap),('r_indirect',0),('vid_fsaa',1),
                          ('r_radiance_quality',args.quality),('r_radiance_grid',1),
                          ('r_radiance_highlight',0),('r_radiance_batch_shadows',args.batch_shadows)):
        soft.verify_cvar(output,name,expected)
    if args.scene=='sponza':rc.verify_model_receiver_state(output,1,1,200000)
    return rc.verify_sources(output,args.point_gain,args.emissive_gain,args.bounce_gain)


def prepare_assets(args, work):
    pakdir=args.basedir/'id1' if (args.basedir/'id1').is_dir() else args.basedir
    paks=sorted(pakdir.glob('pak*.pak'))
    if not paks:raise RuntimeError('Quake PAKs missing')
    (work/'id1').mkdir();(work/'user').mkdir()
    for path in paks:(work/'id1'/path.name).symlink_to(path.resolve())
    game=None;directory=work/'id1';mapname='e1m1' if args.scene=='doorway' else args.scene
    assets={str(p):rc.sha256(p)for p in paks}
    if args.scene=='sponza':
        if not args.scene_assets:raise RuntimeError('--scene-assets is required for Sponza')
        for name in ('maps','progs'):(directory/name).symlink_to((args.scene_assets/name).resolve(),target_is_directory=True)
        mapname='sponza_color'
        bsp=args.scene_assets/'maps/sponza_color.bsp';assets[str(bsp.resolve())]=rc.sha256(bsp)
    elif args.scene=='ecs':
        if not args.ecs_progs_dat:raise RuntimeError('--ecs-progs-dat is required for ECS')
        game='quiver_ripple';directory=work/game;(directory/'maps').mkdir(parents=True);(directory/'progs').mkdir()
        shutil.copy2(args.ecs_progs_dat,directory/'progs.dat')
        repo=Path(__file__).resolve().parents[1]
        for source,target in ((repo/'Misc/vq_pak/maps/ecs32768.bsp',directory/'maps/ecs32768.bsp'),
                              (repo/'Misc/vq_pak/progs/quiver_ripple.mdl',directory/'progs/quiver_ripple.mdl')):
            shutil.copy2(source,target);assets[str(source)]=rc.sha256(source)
        assets[str(args.ecs_progs_dat.resolve())]=rc.sha256(args.ecs_progs_dat);mapname='ecs32768'
    return game,directory,mapname,assets


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--basedir',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--scene',choices=tuple(POSES),default='dm2')
    p.add_argument('--scene-assets',type=Path);p.add_argument('--ecs-progs-dat',type=Path)
    p.add_argument('--mode',choices=('capture','benchmark'),default='capture')
    p.add_argument('--allow-existing',action='store_true');p.add_argument('--validation',action='store_true')
    p.add_argument('--width',type=int,default=1280);p.add_argument('--height',type=int,default=720)
    p.add_argument('--scale',type=rc.resolution_scale,default=.5)
    p.add_argument('--checkerboard',type=int,choices=(0,1),default=1)
    p.add_argument('--quality',type=int,choices=(0,1),default=1)
    p.add_argument('--gaussian',type=rc.probe_gaussian_strength,default=1)
    p.add_argument('--lightmap',type=int,choices=(0,1),default=0)
    p.add_argument('--pose',type=float,nargs=6)
    p.add_argument('--point-gain',type=float,default=1)
    p.add_argument('--emissive-gain',type=float,default=1)
    p.add_argument('--bounce-gain',type=float,default=0)
    p.add_argument('--probe-spacing',type=float,default=32)
    p.add_argument('--soft-radius',type=float,default=15)
    p.add_argument('--filter-radius',type=float,default=8)
    p.add_argument('--batch-shadows',type=int,choices=(0,1),default=1)
    p.add_argument('--pairs',type=int,default=3);p.add_argument('--seconds',type=float,default=1)
    p.add_argument('--initial-warmup',type=float,default=10);p.add_argument('--warmup',type=float,default=1)
    p.add_argument('--timeout',type=float,default=35)
    p.add_argument('--exit-grace-seconds',type=float,default=5,help='final idle grace outside every timing window')
    args=p.parse_args()
    if args.mode=='benchmark' and (args.allow_existing or args.validation):p.error('timing requires no other game and no validation')
    if (args.pairs<1 or min(args.width,args.height)<1 or
        not all(math.isfinite(v) for v in (args.seconds,args.warmup,args.timeout,args.initial_warmup,args.exit_grace_seconds,args.probe_spacing,args.soft_radius,args.filter_radius,args.point_gain,args.emissive_gain,args.bounce_gain)) or
        min(args.seconds,args.warmup,args.timeout)<=0 or args.initial_warmup<10 or not 0<=args.exit_grace_seconds<=10 or not 8<=args.probe_spacing<=128 or not 0<=args.soft_radius<=64 or not 0<args.filter_radius<=128):
        p.error('positive finite durations/dimensions and initial warmup>=10 required')
    if not args.allow_existing:rc.ensure_no_game(False)
    environment=soft.clean_timing_environment()if args.mode=='benchmark'else None
    root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
    frozen=root/'vkquake';shutil.copy2(args.binary,frozen);binary_hash=rc.sha256(frozen)
    game,configdir,mapname,assets=prepare_assets(args,root);pose=tuple(args.pose) if args.pose is not None else POSES[args.scene]
    settings={'host_timescale':.000001,'host_maxfps':30 if args.mode=='capture' else 0,'max_edicts':32800 if args.scene=='ecs' else 2048,
        'r_radiance':1,'r_radiance_backend':2,'r_radiance_quality':args.quality,'r_radiance_grid':1,
        'r_radiance_probe_spacing':args.probe_spacing,'r_radiance_probe_gaussian':args.gaussian,
        'r_radiance_point_gain':args.point_gain,'r_radiance_emissive_gain':args.emissive_gain,'r_radiance_bounce_gain':args.bounce_gain,
        'r_radiance_deferred':1,'r_radiance_resolution_scale':args.scale,'r_radiance_receiver_filter':1,
        'r_radiance_checkerboard':args.checkerboard,'r_radiance_soft_shadows':1,'r_radiance_shadow_radius':args.soft_radius,
        'r_radiance_shadow_sun_angle':.5,'r_radiance_shadow_filter':0,'r_radiance_shadow_filter_radius':args.filter_radius,'r_radiance_batch_shadows':args.batch_shadows,
        'r_radiance_receiver_diagnostics':0,'r_ecs_batch':1,'r_ecs_gpu_cull':1,'r_ssao':0,'r_drawviewmodel':0,
        'crosshair':0,'viewsize':130,'fov':90,'cl_bob':0,'r_rtshadows':0,'r_lightmap':args.lightmap,'r_radiance_highlight':0}
    boot=bench.BOOT.replace('vid_fsaa 0','vid_fsaa 1').replace('r_indirect 1','r_indirect 0').replace('map start',
        '\n'.join(f'{k} {v}'for k,v in settings.items())+'\nscr_centertime 0\nmap '+mapname)
    (configdir/'boot.cfg').write_text(boot);bench.SHIM=soft.isolated_poll_source(bench.SHIM)
    report={'complete':False,'mode':args.mode,'authoritative_timing':False,'binary_sha256':binary_hash,
        'settings':settings,'pose':pose,'assets':assets,'timing_environment':environment,'requested_final_idle_grace_seconds':args.exit_grace_seconds,'pairs':[]}
    def save():(root/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    save();alias,path=soft.short_binary_alias(frozen)
    e=bench.Engine(SimpleNamespace(binary=path,width=args.width,height=args.height,timeout=args.timeout,validation=args.validation,game=game),root)
    counter=None;stop=threading.Event();watchdog=None
    def external_guard():
        if args.mode=='benchmark':rc.ensure_no_game(False,e.process.pid)
    def watch():
        while not stop.wait(.1):
            try:external_guard()
            except Exception as exc:
                report['external_game_guard_failure']=str(exc)
                if e.process.poll()is None:e.process.kill()
                return
    if args.mode=='benchmark':watchdog=threading.Thread(target=watch,daemon=True);watchdog.start()
    try:
        e.wait(lambda text:'CL_SignonReply: 4'in text);e.cmd('setpos '+' '.join(map(str,pose)))
        counter=FrameCounter(e.process.pid,frozen);clock=rc.SimulationClock(counter,frozen);sources=None;initial=True
        for pair in range(1 if args.mode=='capture'else args.pairs):
            row={};report['pairs'].append(row)
            for label in rc.alternating_order(pair,'off','on'):
                strength=int(label=='on');external_guard();e.cmd(f'r_radiance_shadow_filter {strength}\nwait\nwait\nwait\nwait')
                deadline=time.monotonic()+args.timeout
                while True:
                    state=e.cmd(QUERY)
                    try:
                        verify(state,args,pose,strength)
                        if not rc.radiance_ready(state):raise RuntimeError('receiver not ready')
                        break
                    except RuntimeError:
                        if time.monotonic()>=deadline:raise
                        time.sleep(.1)
                time.sleep((args.initial_warmup if initial else 0)+args.warmup)
                if initial and args.mode=='benchmark':
                    preview=e.photo('viewport-proof')
                    report['premeasurement_viewport']={'capture':str(preview),'actual':soft.viewport(preview,args.width,args.height)}
                    save()
                initial=False
                before=e.cmd(QUERY);current=verify(before,args,pose,strength)
                if sources is not None and current!=sources:raise RuntimeError('light source counts changed')
                sources=current;t0=clock.sample();value={'before':before,'sources':sources};row[label]=value;save()
                if args.mode=='benchmark':
                    e.cmd('gpu_frame_time start');wall0,frame0=counter.sample();time.sleep(args.seconds);external_guard()
                    wall1,frame1=counter.sample();timers=e.cmd('gpu_frame_time stop');gpu=rc.parse_gpu(timers)
                    if gpu.get('median_truncated')or gpu.get('p95_truncated')or gpu.get('median_ms')is None or gpu.get('p95_ms')is None:raise RuntimeError('missing/truncated GPU frame times')
                    frames=(frame1-frame0)&0xffffffff
                    if not frames:raise RuntimeError('no measured frames')
                    value.update(whole_frame_gpu=gpu,whole_frame_wall_ms=1000*(wall1-wall0)/frames,fps=frames/(wall1-wall0),timers=timers)
                if args.mode=='capture'or pair==args.pairs-1:
                    image=e.photo(f'{label}-pair{pair+1}');value.update(capture=str(image),actual_viewport=soft.viewport(image,args.width,args.height))
                after=e.cmd(QUERY);verify(after,args,pose,strength)
                if rc.verify_sources(after,args.point_gain,args.emissive_gain,args.bounce_gain)!=sources:raise RuntimeError('light sources changed during sample')
                t1=clock.sample()
                if not 0<=t1-t0<.001:raise RuntimeError('simulation not held')
                value.update(after=after,simulation_before=t0,simulation_after=t1,
                    receiver_frames=rc.verify_receiver_frames(rc.receiver_frames(before),rc.receiver_frames(after),args.scale!=1),
                    soft_shadow_frames=soft.verify_soft_frames(soft.soft_frames(before),soft.soft_frames(after),True),
                    shadow_filter_frames=verify_filter_frames(before,after,strength))
                save();print(args.scene,pair+1,label,value.get('capture',value.get('whole_frame_gpu')),flush=True)
        if args.mode=='benchmark':report['summary']=rc.summarize_pairs(report['pairs'],'off','on')
        report['complete']=True
    except Exception as exc:report['failure']=repr(exc);raise
    finally:
        if counter:counter.close()
        if e.process.poll()is None and args.exit_grace_seconds:
            grace_begin=time.monotonic();time.sleep(args.exit_grace_seconds)
            report['final_idle_grace_seconds']=time.monotonic()-grace_begin
        e.close();stop.set()
        if watchdog:watchdog.join(timeout=1)
        alias.cleanup();report['exit_code']=e.process.returncode;log=(root/'output.log').read_text(errors='replace')+'\n'+e.text()
        report['validation_observed']=bool(re.search(r'VALIDATION-SETTINGS|Using[^\n]*validation',log,re.I))
        report['errors']=re.findall(r'NVVM compilation failed[^\n]*|VK_ERROR_DEVICE_LOST[^\n]*|VUID-[^\n]*|SYNC-HAZARD[^\n]*|Validation Error[^\n]*',log)
        report['binary_unchanged']=rc.sha256(frozen)==binary_hash
        if report['errors']or e.process.returncode or not report['binary_unchanged']or(args.validation and not report['validation_observed']):report['complete']=False
        if args.mode=='benchmark'and report['validation_observed']:report['complete']=False;report['timing_invalid']='validation unexpectedly loaded'
        (root/'console.log').write_text(e.text());shutil.copy2(Path(__file__),root/'harness.py');save()
    print('COMPLETE',root/'results.json',flush=True)

if __name__=='__main__':main()
