#!/usr/bin/env python3
"""CPU-only checks for soft-shadow evidence and private input isolation."""
import os
import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest import mock
import numpy  # Preload before temporary sys.modules patches in protocol tests.

import bench_meshlets
import bench_radiance_soft_shadows as soft


class SoftShadowHarnessTest(unittest.TestCase):
    @contextlib.contextmanager
    def fake_capture(self, root, off_only=False, mismatch=False, inactive_on=False):
        from PIL import Image
        engines=[]
        output=root/'capture'
        for name in ('old-app','new-app','pak0.pak'):(root/name).write_bytes(name.encode())
        class Engine:
            def __init__(self,config,work):
                self.work=work;self.tag=work.name;self.config=config;self.enabled=0;self.frames=0
                self.soft_active=0;self.soft_inactive=0
                self.process=SimpleNamespace(pid=123,returncode=None);self.commands=[];self.closed=False
                self.process.poll=lambda:self.process.returncode
                (work/'output.log').write_text('startup\n');engines.append(self)
            def wait(self,predicate):return predicate('RC_SOFT_CAPTURE_BOOT\nCL_SignonReply: 4')
            def cmd(self,body):
                self.commands.append(body)
                if body=='r_radiance_soft_shadows 1':
                    gate=json.loads((output/'results.json').read_text())
                    if not gate.get('off_parity_gate_passed_before_on'):raise AssertionError('ON requested before matched off gate')
                    self.enabled=1
                elif body=='r_radiance_soft_shadows 0':self.enabled=0
                return (f'"r_radiance_soft_shadows" is "{self.enabled}" '+('(default)' if not self.enabled else '(default: "0")')+'\n'
                    'Radiance: gains point=1 emissive=1 bounce=0\nRadiance: lights=468 emissive=19 emissive_omitted=0\n'
                    f'Radiance: receiver frames reduced={self.frames} full=0\n'
                    f'Radiance: soft shadow frames active={self.soft_active} inactive={self.soft_inactive}\n')
            def photo(self,label):
                self.frames+=2
                if self.enabled and not inactive_on:self.soft_active+=2
                else:self.soft_inactive+=2
                value=65 if mismatch and self.tag=='new' and label=='off' else 62 if label=='on' else 64
                path=self.work/(label+'.png');Image.new('RGB',(self.config.width,self.config.height),(value,value,value)).save(path)
                return path
            def close(self):
                self.closed=True;self.process.returncode=0
                with (self.work/'output.log').open('a') as log:log.write('normal exit\n')
        class Counter:
            def __init__(self,*args):pass
            def close(self):pass
        argv=['soft','--binary',str(root/'new-app'),'--old-off-binary',str(root/'old-app'),
              '--basedir',str(root),'--output',str(output),'--width','32','--height','16']
        if off_only:argv.append('--off-only')
        modules={'bench_meshlets':SimpleNamespace(BOOT=bench_meshlets.BOOT,SHIM=bench_meshlets.SHIM,Engine=Engine),
                 'bench_ecs_ripple':SimpleNamespace(FrameCounter=Counter)}
        with mock.patch.dict(sys.modules,modules),mock.patch.object(sys,'argv',argv), \
             mock.patch.object(soft,'external_games'),mock.patch.object(soft,'verify',return_value=(468,19,0)), \
             mock.patch.object(soft.rc,'radiance_ready',return_value=True), \
             mock.patch.object(soft.rc,'SimulationClock',return_value=SimpleNamespace(sample=lambda:1.0)), \
             mock.patch.object(soft.time,'sleep'),contextlib.redirect_stdout(io.StringIO()):
            yield engines,output

    def test_exact_requested_active_and_ray_budget(self):
        for enabled in (False,True):
            text=f'soft shadows requested={int(enabled)} active={int(enabled)} radius=4 sun_radius_deg=0.5 max_extra_rays=3'
            soft.verify_soft(text,enabled,4,.5)
        good='soft shadows requested=1 active=1 radius=4 sun_radius_deg=0.5 max_extra_rays=3'
        for text in ('',good.replace('active=1','active=0'),good.replace('radius=4','radius=8'),
                     good.replace('max_extra_rays=3','max_extra_rays=4')):
            with self.assertRaises(RuntimeError):soft.verify_soft(text,True,4,.5)

    def test_zero_radius_active_flag_still_allows_real_area_emitters(self):
        soft.verify_soft('soft shadows requested=1 active=1 radius=0 sun_radius_deg=0 max_extra_rays=3',True,0,0)

    def test_compute_post_is_inactive_for_full_and_reference_receivers(self):
        self.assertFalse(soft.post_expected(1,'texture',True))
        self.assertFalse(soft.post_expected(2/3,'probe',True))
        self.assertFalse(soft.post_expected(2/3,'texture',False))
        self.assertTrue(soft.post_expected(2/3,'texture',True))
        soft.verify_soft('soft shadows requested=1 active=0 radius=4 sun_radius_deg=.5 max_extra_rays=3',True,4,.5,False)

    def test_actual_post_windows_require_only_expected_submissions(self):
        start=soft.soft_frames('Radiance: soft shadow frames active=10 inactive=20')
        active=soft.verify_soft_frames(start,{'active':15,'inactive':20},True)
        self.assertEqual(active['delta'],{'active':5,'inactive':0})
        inactive=soft.verify_soft_frames(start,{'active':10,'inactive':23},False)
        self.assertEqual(inactive['delta'],{'active':0,'inactive':3})
        for final in ({'active':15,'inactive':21},{'active':10,'inactive':20},{'active':10,'inactive':23}):
            with self.assertRaises(RuntimeError):soft.verify_soft_frames(start,final,True)
        with self.assertRaises(RuntimeError):soft.soft_frames('soft shadows requested=1 active=1')
        with self.assertRaises(RuntimeError):soft.soft_frames('soft shadow frames active=4294967296 inactive=0')

    def test_actual_post_submission_counters_handle_uint32_wrap(self):
        window=soft.verify_soft_frames({'active':0xfffffffd,'inactive':14},{'active':2,'inactive':14},True)
        self.assertEqual(window['delta'],{'active':5,'inactive':0})

    def test_live_checker_full_probe_and_indirect_actual_post_contract(self):
        import smoke_radiance_soft_shadows as live
        def state(scale,filtering,checker,indirect,actual_post):
            reduced=scale!=1 and not indirect;active_scale=scale if reduced else 1
            text=('Radiance: requested=1 active=1 backend=ray-query quality=0\n'
                'Radiance: gains point=1 emissive=1 bounce=0\nRadiance: lights=468 emissive=19 emissive_omitted=0\n'
                f'Radiance: soft shadows requested=1 active={actual_post} radius=2 sun_radius_deg=.5 max_extra_rays=3\n'
                f'Radiance: receiver scale requested={scale} active={active_scale} divisor={soft.rc.receiver_divisor(active_scale)}\n'
                f'Radiance: receiver filter requested={filtering} active={filtering if reduced else "full"}\n'
                f'Radiance: reduced receiver requested={int(scale!=1)} active={int(reduced)} checkerboard requested={checker} active={int(checker and reduced)}\n'
                'Viewpos: (607 143 40) 3 91 0\n"host_timescale" is ".000001"\n"r_radiance_probe_gaussian" is "0"\n'
                '"r_radiance_receiver_diagnostics" is "0"\n')
            for name,value in (('r_indirect',indirect),('vid_fsaa',1),('fov',90),('r_lightmap',1),('r_radiance_highlight',0),
                               ('r_radiance_quality',0),('r_radiance_grid',1)):
                text+=f'"{name}" is "{value}"\n'
            return text
        for scale,filtering,checker,indirect,post in ((2/3,'texture',1,0,1),(1,'texture',0,0,0),
                (2/3,'probe',1,0,0),(2/3,'texture',1,1,0)):
            text=state(scale,filtering,checker,indirect,post)
            result=live.verify_live(text,'hardware',scale,filtering,checker,1,indirect)
            self.assertEqual(result['post'],bool(post))
            with self.assertRaises(RuntimeError):
                live.verify_live(text.replace(f'active={post} radius=2',f'active={1-post} radius=2'),
                    'hardware',scale,filtering,checker,1,indirect)
        args=SimpleNamespace(scale=2/3,receiver_filter='texture',checkerboard=1,backend='hardware',gaussian=0,
            lightmap=1,radius=2,sun_angle=.5,point_gain=1,emissive_gain=1,bounce_gain=0)
        self.assertEqual(soft.verify(state(2/3,'texture',1,0,1),args,live.POSE,True),(468,19,0))

    def test_source_mismatch_cannot_pass_an_identical_off_image(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'off.png';Image.new('RGB',(8,8),(64,64,64)).save(path)
            old={'capture':str(path),'sources':(469,19,0)}
            new={'capture':str(path),'sources':(468,19,0)}
            evidence=soft.off_pair_evidence(old,new,0)
            self.assertEqual(evidence['max_channel_difference_255'],0)
            self.assertFalse(evidence['accepted'])

    def test_optional_rounding_allowance_bounds_channel_and_pixel_counts(self):
        from PIL import Image
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);old=root/'old.png';new=root/'new.png'
            pixels=np.full((8,8,3),64,dtype=np.uint8);Image.fromarray(pixels).save(old)
            rows=[{'capture':str(path),'sources':(468,19,0)} for path in (old,new)]
            pixels[0,0,0]=65;pixels[0,1,2]=63;Image.fromarray(pixels).save(new)
            self.assertFalse(soft.off_pair_evidence(*rows,1)['accepted'])
            evidence=soft.off_pair_evidence(*rows,1,32)
            self.assertTrue(evidence['accepted']);self.assertEqual(evidence['changed_pixels'],2)
            pixels[:,:,:]=65;Image.fromarray(pixels).save(new)
            self.assertFalse(soft.off_pair_evidence(*rows,1,32)['accepted'])
            pixels[:,:,:]=64;pixels[0,0,0]=66;Image.fromarray(pixels).save(new)
            self.assertFalse(soft.off_pair_evidence(*rows,1,32)['accepted'])

    def test_saved_control_requires_pinned_clean_immutable_metadata(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);image=root/'off.png';binary=root/'app';metadata=root/'reference.json'
            Image.new('RGB',(8,8),(64,64,64)).save(image);binary.write_bytes(b'baseline')
            args=SimpleNamespace(width=8,height=8,scale=2/3);pose=(607,143,40,3,91,0);assets={'pak':'hash'}
            value={'capture':str(image),'sources':[468,19,0],'before':'before','after':'after',
                'simulation_start':1,'simulation_end':1,'receiver_frames':{
                    'before':{'reduced':10,'full':1},'after':{'reduced':14,'full':1}}}
            reference={'complete':True,'mode':'capture','old_vs_new_off_image':{'accepted':True},
                'pose':pose,'assets':assets,'binaries':{'old':{'path':str(binary),'sha256':soft.rc.sha256(binary)}},
                'runs':{'old':{'engine_exit_code':0,'driver_errors':[],'validation_errors':[],
                    'binary_unchanged':True,'pairs':[{'old_off':value}]}}}
            metadata.write_text(json.dumps(reference));image_hash=soft.rc.sha256(image)
            with mock.patch.object(soft,'verify',return_value=(468,19,0)) as guard:
                saved=soft.saved_off_reference(metadata,soft.rc.sha256(metadata),image_hash,args,pose,assets)
                self.assertTrue(saved['reused_without_launch']);self.assertEqual(guard.call_count,2)
                self.assertEqual(saved['value']['sources'],(468,19,0))
                with self.assertRaisesRegex(RuntimeError,'report hash'):
                    soft.saved_off_reference(metadata,'bad',image_hash,args,pose,assets)
                with self.assertRaisesRegex(RuntimeError,'camera or assets'):
                    soft.saved_off_reference(metadata,soft.rc.sha256(metadata),image_hash,args,pose,{'pak':'changed'})
                reference['runs']['old']['driver_errors']=['NVVM compilation failed: 3'];metadata.write_text(json.dumps(reference))
                with self.assertRaisesRegex(RuntimeError,'not clean'):
                    soft.saved_off_reference(metadata,soft.rc.sha256(metadata),image_hash,args,pose,assets)
                reference['runs']['old']['driver_errors']=[];metadata.write_text(json.dumps(reference))
                Image.new('RGB',(8,8),(65,65,65)).save(image)
                with self.assertRaisesRegex(RuntimeError,'image hash'):
                    soft.saved_off_reference(metadata,soft.rc.sha256(metadata),image_hash,args,pose,assets)

    def test_driver_error_after_normal_teardown_is_rejected(self):
        text='Sending clc_disconnect\nRC experimental auxiliary resources released\nNVVM compilation failed: 3\n'
        self.assertEqual(soft.DRIVER_ERRORS.findall(text),['NVVM compilation failed: 3'])
        self.assertTrue(soft.DRIVER_ERRORS.search('VK_ERROR_DEVICE_LOST'))
        self.assertFalse(soft.DRIVER_ERRORS.search('normal shutdown'))

    def test_actual_viewport_must_match_requested_ultrawide(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'capture.png';Image.new('RGB',(1920,1080)).save(path)
            self.assertEqual(soft.viewport(path,1920,1080),[1920,1080])
            with self.assertRaises(RuntimeError):soft.viewport(path,3440,1440)

    def test_caller_paused_game_guard_does_not_accept_resume(self):
        args=SimpleNamespace(mode='benchmark',allow_existing=False,expected_paused_pid=42)
        with mock.patch.object(soft.rc,'other_game_pids',return_value=iter([42])), \
             mock.patch.object(soft,'Path',return_value=SimpleNamespace(read_text=lambda:'State:\tT (stopped)\n')):
            soft.external_games(args,99)
        with mock.patch.object(soft.rc,'other_game_pids',return_value=iter([42])), \
             mock.patch.object(soft,'Path',return_value=SimpleNamespace(read_text=lambda:'State:\tS (sleeping)\n')):
            with self.assertRaises(RuntimeError):soft.external_games(args,99)

    def test_short_launch_alias_and_explicit_boot_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/('long-artifact-output-name-'*6)/'binary/vkquake'
            binary.parent.mkdir(parents=True);binary.write_bytes(b'frozen executable')
            temporary,alias=soft.short_binary_alias(binary)
            try:
                self.assertLess(len(str(alias)),64)
                self.assertEqual(alias.resolve(),binary.resolve())
                self.assertEqual(alias.read_bytes(),binary.read_bytes())
            finally:temporary.cleanup()
            self.assertFalse(alias.exists())
        self.assertTrue(soft.boot_executed('execing boot.cfg\nRC_SOFT_CAPTURE_BOOT\n'))
        self.assertFalse(soft.boot_executed('Playing demo from demo1.dem.'))
        with self.assertRaisesRegex(RuntimeError,'truncation'):
            soft.boot_executed("couldn't exec boot.cf\nPlaying demo from demo1.dem.")

    def test_off_only_keeps_lazy_post_unrequested_and_records_startup_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.fake_capture(Path(directory),off_only=True) as (engines,output):
                soft.main()
                report=json.loads((output/'results.json').read_text())
            self.assertTrue(report['complete'])
            self.assertTrue(report['runs']['new']['startup']['default_off_verified'])
            self.assertIn('launch_to_signon_s',report['runs']['new']['startup'])
            self.assertEqual(report['runs']['new']['engine_exit_code'],0)
            self.assertNotIn('cold_on_request_unix',report['runs']['new']['startup'])
            self.assertFalse(any('r_radiance_soft_shadows 1'==c for e in engines for c in e.commands))
            self.assertTrue(all(e.closed for e in engines))

    def test_bad_off_parity_aborts_before_any_on_compile(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.fake_capture(Path(directory),mismatch=True) as (engines,output):
                with self.assertRaisesRegex(RuntimeError,'no subsequent ON requested'):soft.main()
                report=json.loads((output/'results.json').read_text())
            self.assertFalse(report['complete'])
            self.assertFalse(report['off_parity_gate_passed_before_on'])
            self.assertFalse(any('r_radiance_soft_shadows 1'==c for e in engines for c in e.commands))
            self.assertTrue(all(e.closed for e in engines))

    def test_matched_off_gate_precedes_on_and_records_activation(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.fake_capture(Path(directory)) as (engines,output):
                soft.main()
                report=json.loads((output/'results.json').read_text())
            self.assertTrue(report['complete'])
            self.assertTrue(report['off_parity_gate_passed_before_on'])
            self.assertTrue(report['runs']['new']['startup']['cold_on_actual_post_confirmed'])
            self.assertIn('cold_on_request_to_verified_state_s',report['runs']['new']['startup'])
            self.assertEqual(report['runs']['new']['engine_exit_code'],0)
            self.assertTrue(all(e.closed for e in engines))

    def test_failed_post_capture_guard_preserves_rejected_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.fake_capture(Path(directory)) as (engines,output):
                def state_guard(*args):
                    if engines[-1].tag=='new' and engines[-1].enabled and engines[-1].frames>=4:
                        raise RuntimeError('post-capture actual soft post inactive')
                    return (468,19,0)
                with mock.patch.object(soft,'verify',side_effect=state_guard):
                    with self.assertRaisesRegex(RuntimeError,'post-capture actual'):soft.main()
                report=json.loads((output/'results.json').read_text())
            self.assertFalse(report['complete'])
            rejected=report['runs']['new']['pairs'][0]['on']
            self.assertTrue(Path(rejected['capture']).is_file())
            self.assertFalse(rejected['capture_verified'])
            self.assertIn('capture_state',rejected)
            self.assertEqual(report['failure']['message'],'post-capture actual soft post inactive')
            self.assertTrue(all(e.closed for e in engines))

    def test_inactive_submission_rejects_on_despite_good_state_snapshots(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.fake_capture(Path(directory),inactive_on=True) as (engines,output):
                with self.assertRaisesRegex(RuntimeError,'wrong actual soft-shadow submissions'):soft.main()
                report=json.loads((output/'results.json').read_text())
            self.assertFalse(report['complete'])
            rejected=report['runs']['new']['pairs'][0]['on']
            self.assertFalse(rejected['capture_verified'])
            self.assertFalse(rejected['soft_shadow_frames']['verified'])
            self.assertEqual(rejected['soft_shadow_frames']['before'],{'active':0,'inactive':2})
            self.assertEqual(rejected['soft_shadow_frames']['after'],{'active':0,'inactive':4})
            self.assertTrue(report['off_parity_gate_passed_before_on'])
            self.assertTrue(all(e.closed for e in engines))

    @unittest.skipUnless(shutil.which('cc') and shutil.which('pkg-config'),'SDL CPU shim check needs compiler/pkg-config')
    def test_private_sdl_queue_filters_real_input_and_keeps_control_key(self):
        flags=subprocess.run(['pkg-config','--cflags','--libs','sdl2'],capture_output=True,text=True)
        if flags.returncode:self.skipTest('SDL2 development package unavailable')
        program=r'''
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
 int p[2]; if(pipe(p))return 1; fcntl(p[0],F_SETFL,O_NONBLOCK);
 char number[32];snprintf(number,sizeof(number),"%d",p[0]);setenv("BENCH_KEY_FD",number,1);
 if(SDL_Init(SDL_INIT_EVENTS))return 2;
 SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_SPACE;SDL_PushEvent(&e);
 e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;SDL_PushEvent(&e);
 e.type=SDL_USEREVENT;SDL_PushEvent(&e);write(p[1],"k",1);
 if(!SDL_PollEvent(&e)||e.type!=SDL_KEYDOWN||e.key.keysym.sym!=SDLK_F12)return 3;
 if(!SDL_PollEvent(&e)||e.type!=SDL_KEYUP||e.key.keysym.sym!=SDLK_F12)return 4;
 if(!SDL_PollEvent(&e)||e.type!=SDL_USEREVENT)return 5;
 if(SDL_PollEvent(&e))return 6;SDL_Quit();return 0;
}'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'shim.c').write_text(soft.isolated_poll_source(bench_meshlets.SHIM));(root/'test.c').write_text(program)
            cflags=subprocess.check_output(['pkg-config','--cflags','sdl2'],text=True).split()
            subprocess.run(['cc','-shared','-fPIC',*cflags,str(root/'shim.c'),'-ldl','-o',str(root/'shim.so')],check=True,capture_output=True)
            subprocess.run(['cc',str(root/'test.c'),*flags.stdout.split(),'-o',str(root/'cpu-events')],check=True,capture_output=True)
            env=dict(os.environ,LD_PRELOAD=str(root/'shim.so'))
            subprocess.run([str(root/'cpu-events')],env=env,check=True,timeout=5,capture_output=True)


if __name__=='__main__':unittest.main()
