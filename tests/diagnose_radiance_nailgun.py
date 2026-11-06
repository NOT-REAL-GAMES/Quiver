#!/usr/bin/env python3
"""Isolated actual-Sponza weapon diagnostics; never signals an existing game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import threading
import time
from types import SimpleNamespace

import bench_meshlets as bench
import bench_radiance as rc
import bench_radiance_soft_shadows as soft
from bench_ecs_ripple import FrameCounter

WEAPONS={'shotgun':(2,1,'shells'),'nailgun':(4,4,'nails'),
         'grenade':(6,16,'rockets'),'rocket':(7,32,'rockets'),'lightning':(8,64,'cells')}
POSE=(1696,-1740,170,20,90,0)


def resolve_addresses(output,fields,base):
    addresses={}
    for name,(_,fmt) in fields.items():
        match=re.search(r'FIELD_'+name+r'=\s*\$\d+\s*=\s*(0x[0-9a-fA-F]+)',output)
        size=re.search(r'SIZE_'+name+r'=(\d+)',output)
        if match and size:
            if int(size[1])!=struct.calcsize('='+fmt):
                raise RuntimeError('frozen DWARF type size differs for '+name)
            addresses[name]=base+int(match[1],16)
    return addresses


class Snapshot:
    """Resolve the frozen executable's DWARF, then read only its child memory."""
    def __init__(self,pid,binary):
        self.counter=FrameCounter(pid,binary)
        symbols=subprocess.check_output(['nm','-an',str(binary)],text=True)
        host=int(re.search(r'^([0-9a-fA-F]+)\s+\w\s+host_framecount$',symbols,re.M)[1],16)
        base=self.counter.address-host
        self.fields={'triangles':('rc_engine.scene.runtime.scene.triangle_count','I'),
            'world_triangles':('rc_engine.scene.runtime.static_scene.triangle_count','I'),
            'atlas_pixel_revision':('rc_engine.scene.atlas_upload_generation','Q'),
            'material_selection_revision':('rc_engine.scene.atlas_revision','Q'),
            'geometry_revision':('rc_engine.scene.geometry_generation','Q'),
            'base_triangles':('rc_engine.scene.runtime.base_scene.triangle_count','I'),
            'base_geometry_revision':('rc_engine.scene.base_geometry_generation','Q'),
            'base_content_revision':('rc_engine.scene.base_content_generation','Q'),
            'summary_scanned':('rc_engine.scene.stats.summary_triangles_scanned','Q'),
            'cache_compared':('rc_engine.scene.stats.cache_triangles_compared','Q'),
            'assembly_copied':('rc_engine.scene.stats.assembly_triangles_copied','Q'),
            'active_weapon':('cl.stats[10]','i'),'weapon_frame':('cl.stats[5]','i'),
            'ammo':('cl.stats[3]','i'),'nails':('cl.stats[7]','i'),'shells':('cl.stats[6]','i'),
            'rockets':('cl.stats[8]','i'),'cells':('cl.stats[9]','i'),'simulation_time':('cl.time','d'),
            'cpu_profile_available':('radiance_stats.cpu_profile_available','?')}
        for name in ('rebuilt','refit','geometry_changed','materials_changed','base_content_changed','base_geometry_changed'):
            self.fields[name]=('rc_engine.scene.stats.'+name,'?')
        for name in ('base_triangles_built','mutable_triangles_built'):
            self.fields[name]=('rc_engine.scene.stats.'+name,'Q')
        for name in ('prepare','pipeline','scene','lights','upload','sparse','dispatch'):
            self.fields['cpu_'+name+'_ms']=('radiance_stats.cpu_'+name+'_ms','d')
        for slot in range(2):
            for name,fmt in (('static_triangles','I'),('dynamic_triangles','I'),('dynamic_upload_bytes','Q'),('dynamic_builds','I'),
                             ('dynamic_updates','I'),('tlas_builds','I'),('tlas_updates','I'),('instance_count','I')):
                self.fields[f'slot{slot}_{name}']=(f'rc_engine.scene.rt.frame[{slot}].{name}',fmt)
        command=['gdb','-nx','-batch',str(binary)]
        for name,(expr,_) in self.fields.items():
            command+=['-ex',f'printf "FIELD_{name}="','-ex','p/x &'+expr]
            command+=['-ex',f'printf "SIZE_{name}=%u\\n", (unsigned)sizeof({expr})']
        out=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
        self.address=resolve_addresses(out,self.fields,base)
        self.unavailable=sorted(set(self.fields)-set(self.address))
        if any(name not in self.address for name in ('triangles','atlas_pixel_revision','material_selection_revision','active_weapon','nails','simulation_time')):
            raise RuntimeError('required frozen-DWARF field unavailable: '+out)

    def read(self):
        _,frame=self.counter.sample();result={'host_framecount':frame,'wall_monotonic':time.monotonic()}
        for name,address in self.address.items():
            fmt='='+self.fields[name][1];size=struct.calcsize(fmt)
            result[name]=struct.unpack(fmt,os.pread(self.counter.fd,size,address))[0]
        result['same_host_frame']=self.counter.sample()[1]==frame
        return result

    def close(self):self.counter.close()


def summarize(samples):
    if not samples:return {'samples':0}
    result={'samples':len(samples),'simulation_advance':samples[-1]['simulation_time']-samples[0]['simulation_time'],
        'active_weapons':sorted({s['active_weapon'] for s in samples}),
        'nails_before':samples[0]['nails'],'nails_after':samples[-1]['nails'],
        'atlas_pixel_revisions':sorted({s['atlas_pixel_revision'] for s in samples}),
        'material_selection_revisions':sorted({s['material_selection_revision'] for s in samples})}
    for key in ('triangles','base_triangles','base_triangles_built','mutable_triangles_built','base_geometry_revision',
                'assembly_copied','summary_scanned','cache_compared','slot0_static_triangles','slot1_static_triangles',
                'slot0_dynamic_triangles','slot1_dynamic_triangles'):
        values=[s[key] for s in samples if key in s]
        if values:result[key]={'min':min(values),'max':max(values),'median':statistics.median(values)}
    valid=[s for s in samples if s.get('cpu_profile_available') and s['same_host_frame']]
    result['valid_cpu_samples']=len(valid)
    for key in ('cpu_prepare_ms','cpu_scene_ms','cpu_lights_ms','cpu_upload_ms'):
        values=[s[key] for s in valid if key in s]
        if values:result[key]={'median':statistics.median(values),'max':max(values)}
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--basedir',type=Path,required=True)
    p.add_argument('--model-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--backend',choices=('hardware','software'),default='hardware')
    p.add_argument('--weapons',nargs='+',choices=tuple(WEAPONS),default=('nailgun','shotgun','grenade','rocket','lightning'))
    p.add_argument('--seconds',type=float,default=3);p.add_argument('--timeout',type=float,default=35)
    p.add_argument('--budget-seconds',type=float,default=180)
    p.add_argument('--allow-existing',action='store_true');p.add_argument('--validation',action='store_true')
    args=p.parse_args()
    if not 1<=args.seconds<=10 or not 0<args.timeout<=40 or not 0<args.budget_seconds<=180:
        p.error('seconds1..10, timeout(0,40], budget(0,180] required')
    rc.ensure_no_game(args.allow_existing)
    root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
    binary=root/'binary/vkquake';binary.parent.mkdir();shutil.copy2(args.binary.resolve(),binary)
    source=args.basedir.resolve();source=source/'id1' if (source/'id1').is_dir() else source
    paks=sorted(source.glob('pak*.pak'));assets=sorted(q for q in args.model_dir.resolve().glob('sponza*') if q.is_file())
    if not paks or not any(q.name=='sponza.md5mesh' for q in assets):p.error('Quake PAKs and actual Sponza assets required')
    (root/'id1/progs').mkdir(parents=True);(root/'user').mkdir();(root/'harness').mkdir()
    for q in paks:(root/'id1'/q.name).symlink_to(q)
    for q in assets:(root/'id1/progs'/q.name).symlink_to(q)
    for name in (Path(__file__).name,'bench_meshlets.py','bench_radiance.py','bench_radiance_soft_shadows.py','bench_ecs_ripple.py'):
        shutil.copy2(Path(__file__).with_name(name),root/'harness'/name)
    report={'complete':False,'binary_sha256':rc.sha256(binary),'backend':args.backend,'phases':[],
        'asset_sha256':{str(q):rc.sha256(q) for q in paks+assets},'pose':POSE,
        'note':'Functional normal-clock diagnostics; per-frame memory reads/profile sampling are not an authoritative GPU benchmark.'}
    def save():(root/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    settings=(f'host_timescale 1\nhost_maxfps 30\nr_rtshadows 0\nr_radiance 1\nr_radiance_backend {2 if args.backend=="hardware" else 1}\n'
        'r_radiance_quality 0\nr_radiance_grid 1\nr_radiance_highlight 0\nr_radiance_point_gain 1\nr_radiance_bounce_gain 0\n'
        'r_radiance_emissive_gain 0\nr_radiance_sky_fallback 0\nr_radiance_soft_shadows 0\nr_radiance_shadow_radius 2\n'
        'r_radiance_shadow_sun_angle .5\nr_radiance_receiver_filter 1\n'
        'r_radiance_resolution_scale .666666667\nr_radiance_checkerboard 0\nr_radiance_probe_gaussian 0\n'
        'r_radiance_receiver_diagnostics 0\nr_radiance_profile 1\nr_drawentities 1\nr_drawviewmodel 1\n'
        'viewsize 130\ncrosshair 0\ncl_bob 0\ncl_rollangle 0\nfov 90\necho RC_SOFT_CAPTURE_BOOT\nmap dm2')
    (root/'id1/boot.cfg').write_text(bench.BOOT.replace('r_indirect 1','r_indirect 0').replace('vid_fsaa 0','vid_fsaa 1').replace('map start',settings))
    temporary,alias=soft.short_binary_alias(binary);bench.SHIM=soft.isolated_poll_source(bench.SHIM)
    engine=bench.Engine(SimpleNamespace(binary=alias,width=960,height=540,timeout=args.timeout,validation=args.validation,game=None),root)
    snapshot=None;begin=time.monotonic();deadline=[begin+40];stop=threading.Event()
    def watchdog():
        while not stop.wait(.1):
            now=time.monotonic()
            if (now>deadline[0] or now-begin>args.budget_seconds) and engine.process.poll() is None:
                report['own_process_watchdog']={'pid':engine.process.pid,'command_limit_s':40,
                    'total_limit_s':args.budget_seconds,'elapsed_s':now-begin};engine.process.kill();return
    thread=threading.Thread(target=watchdog,daemon=True);thread.start()
    def cmd(body):deadline[0]=time.monotonic()+40;return engine.cmd(body)
    def state(label):
        text=cmd('viewpos\nhost_timescale\nr_drawentities\nr_drawviewmodel\nradiance_stats');rc.verify_state(text,'radiance',args.backend,0)
        rc.verify_receiver_state(text,'half',2/3,'texture',True);soft.verify_cvar(text,'host_timescale',1)
        soft.verify_soft(text,False,2,.5,False);rc.verify_sources(text,1,0,0)
        soft.verify_cvar(text,'r_drawentities',1);soft.verify_cvar(text,'r_drawviewmodel',1)
        (root/(label+'.txt')).write_text(text);return text
    def phase(label,seconds,weapon=None,shoot=False,photo=False):
        before=state(label+'-before');samples=[];start=time.monotonic();end=start+seconds;last=None
        while time.monotonic()<end:
            sample=snapshot.read()
            if sample['host_framecount']!=last:samples.append(sample);last=sample['host_framecount']
            time.sleep(.01)
        after=state(label+'-after');row={'label':label,'samples':samples,'summary':summarize(samples),'before':before,'after':after,
            'weapon':weapon,'shooting':shoot,'wall_seconds':time.monotonic()-start,'verified':False}
        report['phases'].append(row);save()
        if weapon and any(s['active_weapon']!=WEAPONS[weapon][1] for s in samples):raise RuntimeError('controlled weapon did not stay selected')
        if shoot and samples and samples[-1][WEAPONS[weapon][2]]>=samples[0][WEAPONS[weapon][2]]:
            raise RuntimeError('firing did not consume expected ammunition')
        if photo:row['capture']=str(engine.photo(label));soft.viewport(row['capture'],960,540)
        row['verified']=True;save();print(label+' '+json.dumps(row['summary']),flush=True)
    try:
        try:
            engine.wait(soft.boot_executed);engine.wait(lambda t:'CL_SignonReply: 4' in t)
            cmd('god\nnotarget\nnoclip\nviewmodel\nsetpos '+' '.join(map(str,POSE))+'\ngive 2\ngive 4\ngive 6\ngive 7\ngive 8\ngive n 1000\ngive s 1000\ngive r 1000\ngive c 1000\nimpulse 2')
            snapshot=Snapshot(engine.process.pid,binary);report['unavailable_snapshot_fields']=snapshot.unavailable
            report['snapshot_field_formats']=snapshot.fields
            time.sleep(.5);phase('world-only',1,photo=True)
            base=snapshot.read();cmd('viewmodel progs/sponza.md5mesh\nwait\nwait');time.sleep(1)
            loaded=snapshot.read();report['sponza_triangle_delta']=loaded['triangles']-base['triangles']
            if report['sponza_triangle_delta']!=262266:raise RuntimeError('actual Sponza triangle delta differs')
            phase('sponza-idle',1,photo=True)
            schedule=list(args.weapons)
            if 'nailgun' in schedule:schedule.insert(schedule.index('nailgun')+1,'nailgun')
            occurrences={}
            for weapon in schedule:
                occurrences[weapon]=occurrences.get(weapon,0)+1;label=weapon+'-'+str(occurrences[weapon])
                cmd('-attack\nimpulse '+str(WEAPONS[weapon][0]));limit=time.monotonic()+10
                while snapshot.read()['active_weapon']!=WEAPONS[weapon][1]:
                    if time.monotonic()>limit:raise RuntimeError('weapon selection timed out')
                    time.sleep(.05)
                phase(label+'-idle',1,weapon)
                cmd('+attack');phase(label+'-firing',args.seconds,weapon,True,True)
                cmd('-attack');phase(label+'-after',1.5,weapon)
        finally:
            if engine.process.poll() is None:
                try:cmd('-attack');time.sleep(5)
                except Exception:pass
            try:engine.close()
            finally:stop.set();thread.join(timeout=1);temporary.cleanup()
            if snapshot:snapshot.close()
            logs=(root/'output.log').read_text(errors='replace')+'\n'+engine.text()
            report.update(engine_exit_code=engine.process.returncode,driver_errors=soft.DRIVER_ERRORS.findall(logs),
                validation_errors=soft.ERRORS.findall(logs),binary_unchanged=rc.sha256(binary)==report['binary_sha256']);save()
        if report['engine_exit_code'] or report['driver_errors'] or report['validation_errors']:raise RuntimeError('renderer/driver/validation failure')
        report['complete']=True;save()
    except Exception as exc:report['failure']={'type':type(exc).__name__,'message':str(exc)};save();raise


if __name__=='__main__':main()
