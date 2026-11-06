#!/usr/bin/env python3
"""Bounded real-scene deferred model captures; no performance claims or signals.

Uses a prepared private Sponza fixture and exact actual-mode/frame counters.
Functional GPU sharing requires explicit --allow-existing; unrelated games are
never paused or terminated. Validation must be supplied by the caller's SDK.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import time
from types import SimpleNamespace

import bench_meshlets as bench
import bench_radiance as rc
from bench_ecs_ripple import FrameCounter
from bench_radiance_soft_shadows import isolated_poll_source, short_binary_alias, viewport

QUERY=('radiance_stats\nstatic_models_stats\nviewpos\nhost_timescale\n'
       'r_radiance_probe_gaussian\nr_radiance_receiver_diagnostics\nr_indirect\n'
       'r_radiance_quality\nr_radiance_deferred\nhost_maxfps\nr_ssao')
CASES=((1,1,1),(1,2/3,1),(1,.5,1),(0,1,1))


def verify(output,deferred,scale,checker,pose,minimum_triangles,ssao=0):
    rc.verify_state(output,'radiance','hardware',0)
    rc.verify_receiver_state(output,rc.receiver_name(scale,bool(checker)),scale,'texture',True,
        full_texture=bool(deferred and scale==1),checkerboard_requested=bool(checker))
    model=rc.verify_model_receiver_state(output,bool(deferred),bool(deferred),minimum_triangles)
    if rc.verify_sources(output,1,1,1)!=(11,0,0):raise RuntimeError('private scene source counts changed')
    rc.verify_hold(output);rc.verify_pose(output,pose);rc.verify_probe_filter(output,2)
    rc.verify_receiver_diagnostics_off(output)
    if not re.search(r'StaticModels: instances=1 triangles=262266 collision=1 client_compiled=1',output):
        raise RuntimeError('compiled model asset omitted')
    for name,wanted in [('r_indirect',0),('r_radiance_deferred',deferred),('host_maxfps',30),('r_ssao',ssao)]:
        match=re.search('"'+name+r'" is "([^"]+)"',output)
        if not match or float(match[1])!=wanted:raise RuntimeError('wrong '+name)
    return model


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--expected-sha256',required=True)
    p.add_argument('--fixture',type=Path,required=True,help='prepared artifact root containing manifest.json and fixture/')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--allow-existing',action='store_true')
    p.add_argument('--quick',action='store_true');p.add_argument('--ssao',type=int,choices=(0,1),default=0)
    p.add_argument('--coverage',action='store_true',help='collect separate estimated pixel reuse after captures')
    p.add_argument('--two-thirds',action='store_true',help='compact full/2-thirds/forward sequence with reduced checker off')
    p.add_argument('--timeout',type=float,default=35);p.add_argument('--minimum-triangles',type=int,default=100000)
    a=p.parse_args()
    if not 0<a.timeout<=40:p.error('timeout must be in(0,40]')
    rc.ensure_no_game(a.allow_existing)
    if rc.sha256(a.binary.resolve())!=a.expected_sha256:raise RuntimeError('candidate hash mismatch')
    root=a.output.resolve();root.mkdir(parents=True,exist_ok=False);work=root/'work'
    (work/'id1/maps').mkdir(parents=True);(work/'user').mkdir()
    fixture=a.fixture.resolve();manifest=json.loads((fixture/'manifest.json').read_text());scene=fixture/'fixture/sponza_bench'
    for source in (fixture/'fixture/id1').glob('pak*.pak'):(work/'id1'/source.name).symlink_to(source.resolve())
    for source in (scene/'maps').iterdir():(work/'id1/maps'/source.name).symlink_to(source.resolve())
    (work/'id1/progs').symlink_to(scene/'progs',target_is_directory=True)
    binary=root/'binary/vkquake';binary.parent.mkdir();shutil.copy2(a.binary.resolve(),binary)
    settings={**manifest['settings'],'host_maxfps':30,'r_radiance_deferred':1,'r_radiance_resolution_scale':1,
              'r_radiance_checkerboard':1,'r_radiance':1,'vid_fullscreen':0,'r_ssao':a.ssao}
    boot=bench.BOOT.replace('map start','\n'.join(f'{key} {value}' for key,value in settings.items())+'\nscr_centertime 0\nmap sponza_color')
    (work/'id1/boot.cfg').write_text(boot);bench.SHIM=isolated_poll_source(bench.SHIM)
    report={'complete':False,'mode':'SDK functional only','gpu_shared':a.allow_existing,'authoritative_timing':False,
        'binary_sha256':rc.sha256(binary),'source_binary':str(a.binary.resolve()),'cases':[],
        'fixture_map_sha256':manifest['map_sha256'],'errors':[]}
    result=root/'results.json'
    def save():result.write_text(json.dumps(report,indent=2)+'\n')
    save();alias,path=short_binary_alias(binary)
    engine=bench.Engine(SimpleNamespace(binary=path,width=1280,height=720,timeout=a.timeout,validation=True),work)
    counter=None;pose=(-16,-300,80,-5,90,0)
    def query():return engine.cmd(QUERY)
    def settled(deferred,scale,checker):
        deadline=time.monotonic()+a.timeout;last='';error=None
        while time.monotonic()<deadline:
            last=query()
            try:
                model=verify(last,deferred,scale,checker,pose,a.minimum_triangles,a.ssao)
                if rc.radiance_ready(last):return last,model
            except RuntimeError as failure:error=str(failure)
            time.sleep(.15)
        raise RuntimeError(f'actual deferred state did not settle: {error}\n{last}')
    def capture(label):
        image=engine.photo(label);viewport(image,1280,720)
        target=root/(label+'.png');shutil.copy2(image,target);return str(target)
    try:
        launched=time.monotonic();engine.wait(lambda text:'CL_SignonReply: 4' in text)
        engine.cmd('setpos '+' '.join(map(str,pose)))
        counter=FrameCounter(engine.process.pid,binary);clock=rc.SimulationClock(counter,binary)
        cases=((1,1,1),(1,2/3,1),(1,2/3,0),(0,1,1)) if a.two_thirds else CASES[::3] if a.quick else CASES
        for deferred,scale,checker in cases:
            if not a.allow_existing:rc.ensure_no_game(False,engine.process.pid)
            engine.cmd(f'r_radiance_deferred {deferred}\nr_radiance_resolution_scale {scale:.12g}\nr_radiance_checkerboard {checker}')
            before,model_before=settled(deferred,scale,checker)
            # Warm real submitted frames, including background driver jobs;
            # no timing is recorded while the GPU is shared.
            time.sleep(1)
            before=query();model_before=verify(before,deferred,scale,checker,pose,a.minimum_triangles,a.ssao)
            t0=clock.sample();image=capture(f'deferred{deferred}-scale{scale:g}-checker{checker}')
            after=query();model_after=verify(after,deferred,scale,checker,pose,a.minimum_triangles,a.ssao)
            t1=clock.sample()
            if not 0<=t1-t0<.001:raise RuntimeError('held capture clock advanced')
            row={'deferred':deferred,'scale':scale,'checkerboard_requested':checker,'capture':image,
                 'state_before':before,'state_after':after,'clock_before':t0,'clock_after':t1,
                 'receiver_frames':rc.verify_receiver_frames(rc.receiver_frames(before),rc.receiver_frames(after),scale!=1),
                 'model_frames':rc.verify_model_receiver_frames(model_before,model_after,bool(deferred))}
            if deferred and a.coverage:
                from diagnose_radiance_receiver_filter import coverage
                engine.cmd('r_radiance_receiver_diagnostics 1')
                deadline=time.monotonic()+a.timeout;seen=None
                while True:
                    text=engine.cmd('radiance_stats')
                    try:
                        data=coverage(text,'texture',rc.receiver_divisor(scale),1280,720)
                        if seen is not None and data['frame']!=seen:break
                        seen=data['frame']
                    except RuntimeError:pass
                    if time.monotonic()>deadline:raise RuntimeError('fresh model coverage unavailable')
                    time.sleep(.1)
                row['coverage']=data;row['coverage_state']=text
                engine.cmd('r_radiance_receiver_diagnostics 0')
                rc.verify_receiver_diagnostics_off(engine.cmd('r_radiance_receiver_diagnostics'))
            report['cases'].append(row);save()
            print(f'PASS functional deferred={deferred} scale={scale:g}: models_active={model_after["active"]},draws={model_after["draws"]},triangles={model_after["triangles"]}',flush=True)
        engine.cmd('r_radiance_deferred 1\nr_radiance_resolution_scale 1\nvid_restart\nwait\nwait')
        state,model=settled(1,1,1);report['restart']={'state':state,'capture':capture('deferred1-full-after-restart')}
        engine.cmd('bind F12 quit');os.write(engine.key,b'k');engine.process.wait(timeout=15)
        if engine.process.returncode!=0:raise RuntimeError('unclean renderer exit')
        report['complete']=True;report['launch_to_exit_seconds']=time.monotonic()-launched
    except BaseException as failure:
        report['failure']=str(failure);raise
    finally:
        if counter:counter.close()
        if engine.process.poll() is None:
            engine.process.terminate()
            try:engine.process.wait(timeout=15)
            except Exception:engine.process.kill();engine.process.wait(timeout=5)
        engine.close();alias.cleanup()
        report['exit_code']=engine.process.returncode;report['hash_unchanged']=rc.sha256(binary)==a.expected_sha256
        logs=(work/'output.log').read_text(errors='replace')+'\n'+engine.text()
        report['errors']=re.findall(r'VUID-[^\n]*|SYNC-HAZARD[^\n]*|Validation Error[^\n]*|NVVM compilation failed[^\n]*|VK_ERROR_DEVICE_LOST[^\n]*|Host_Error[^\n]*|Sys_Error[^\n]*',logs)
        report['validation_observed']=bool(re.search(r'Using.*validation|VALIDATION-SETTINGS',logs,re.I))
        if report['errors'] or not report['validation_observed'] or not report['hash_unchanged']:report['complete']=False
        shutil.copy2(work/'output.log',root/'output.log');(root/'console.log').write_text(engine.text());save()
        (root/'harness').mkdir(exist_ok=True)
        for name in (Path(__file__).name,'bench_radiance.py','bench_meshlets.py','bench_ecs_ripple.py','bench_radiance_soft_shadows.py'):
            shutil.copy2(Path(__file__).with_name(name),root/'harness'/name)
    if not report['complete']:raise RuntimeError('functional acceptance failed')
    print('COMPLETE functional:',result,flush=True)

if __name__=='__main__':main()
