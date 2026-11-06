#!/usr/bin/env python3
"""Held native soft-shadow captures and off/on timings, isolated from user games.

Capture mode may share a GPU explicitly. Benchmark mode requires other games
closed (or a caller-paused, identity-checked process). This script sends no
process signals. An optional old executable provides an off-image/control;
its timing block is reported separately, not called an interleaved paired run.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import tempfile
import threading
import time
from types import SimpleNamespace

import bench_radiance as rc

CAMERAS = {'doorway': (607,143,40,3,91,0), 'courtyard': (1312,880,-248,0,90,0),
           'spawn': (480,-352,88,0,90,0)}
SOFT = re.compile(r'soft shadows requested=(\d+) active=(\d+) radius=(\S+) sun_radius_deg=(\S+) max_extra_rays=(\d+)')
SOFT_FRAMES = re.compile(r'soft shadow frames active=(\d+) inactive=(\d+)')
ERRORS = re.compile(r'VUID-|Validation Error|SYNC-HAZARD|(?:READ|WRITE)_AFTER_(?:READ|WRITE) hazard', re.I)
DRIVER_ERRORS = re.compile(r'NVVM compilation failed[^\n]*|VK_ERROR_DEVICE_LOST|failed to create[^\n]*pipeline', re.I)
QUERY = ('radiance_stats\nviewpos\nhost_timescale\nr_lightmap\nr_indirect\nvid_fsaa\nfov\n'
         'r_radiance_highlight\nr_radiance_quality\nr_radiance_grid\nr_radiance_probe_gaussian\n'
         'r_radiance_receiver_diagnostics')


def verify_cvar(output, name, expected):
    match = re.search(r'"'+re.escape(name)+r'" is "([^"]+)"', output)
    try: value = float(match[1]) if match else math.nan
    except ValueError: value = math.nan
    if not math.isfinite(value) or not math.isclose(value, expected, rel_tol=1e-6, abs_tol=1e-7):
        raise RuntimeError(f'expected {name}={expected}: {output}')


def verify_soft(output, enabled, radius, sun_angle, active=None):
    match = SOFT.search(output)
    if not match: raise RuntimeError('soft-shadow active state unavailable')
    # The optional compute post applies only to fast reduced texture receivers.
    # Q0 emitters may still use their area with both radius controls zero.
    expected_active = int(enabled if active is None else active)
    try: actual = (int(match[1]), int(match[2]), float(match[3]), float(match[4]), int(match[5]))
    except ValueError as exc: raise RuntimeError('invalid soft-shadow state') from exc
    if (actual[:2] != (int(enabled), expected_active) or actual[4] != 3 or
            not math.isclose(actual[2], radius, rel_tol=1e-5, abs_tol=1e-7) or
            not math.isclose(actual[3], sun_angle, rel_tol=1e-5, abs_tol=1e-7)):
        raise RuntimeError(f'unexpected native soft-shadow state: {match[0]}')


def post_expected(scale, filtering, enabled):
    return bool(enabled and scale != 1 and filtering == 'texture')


def soft_frames(output):
    match=SOFT_FRAMES.search(output)
    if not match:raise RuntimeError('actual soft-shadow submission counters unavailable')
    counters={'active':int(match[1]),'inactive':int(match[2])}
    if any(not 0<=value<=0xffffffff for value in counters.values()):
        raise RuntimeError('invalid soft-shadow uint32 submission counters')
    return counters


def verify_soft_frames(before, after, expected_active):
    deltas={key:(after[key]-before[key])&0xffffffff for key in ('active','inactive')}
    wanted='active' if expected_active else 'inactive'
    unwanted='inactive' if expected_active else 'active'
    if not deltas[wanted] or deltas[unwanted]:
        raise RuntimeError(f'wrong actual soft-shadow submissions: expected {wanted}, deltas={deltas}')
    return {'before':before,'after':after,'delta':deltas,'expected_active':bool(expected_active)}


def verify(output, args, pose, enabled=None):
    rc.verify_state(output, 'radiance', args.backend, 0)
    rc.verify_receiver_state(output, rc.receiver_name(args.scale, getattr(args,'checkerboard',False)), args.scale, args.receiver_filter, True)
    rc.verify_pose(output, pose)
    rc.verify_probe_filter(output, args.gaussian)
    rc.verify_hold(output)
    rc.verify_receiver_diagnostics_off(output)
    for name, expected in [('r_lightmap', args.lightmap), ('r_indirect',0), ('vid_fsaa',1), ('fov',90),
                           ('r_radiance_highlight',0), ('r_radiance_quality',0), ('r_radiance_grid',1)]:
        verify_cvar(output, name, expected)
    if enabled is not None:
        expected = post_expected(args.scale,args.receiver_filter,enabled)
        verify_soft(output, enabled, args.radius, args.sun_angle, expected)
    return rc.verify_sources(output, args.point_gain, args.emissive_gain, args.bounce_gain)


def viewport(path, width, height):
    from PIL import Image
    with Image.open(path) as image:
        if image.size != (width,height):
            raise RuntimeError(f'actual capture viewport {image.size} differs from requested {(width,height)}')
        return list(image.size)


def off_pair_evidence(old, new, tolerance, max_changed_pixels=0):
    """Return an explicit pre-ON gate; source mismatch cannot pass pixel tests."""
    from PIL import Image
    import numpy as np
    a=np.asarray(Image.open(old['capture']).convert('RGB'),dtype=np.int16)
    b=np.asarray(Image.open(new['capture']).convert('RGB'),dtype=np.int16)
    if a.shape!=b.shape:raise RuntimeError('old/new off viewport mismatch')
    delta=np.abs(b-a)
    sources_match=old['sources']==new['sources']
    changed_pixels=int(np.count_nonzero(np.any(delta>0,axis=2)))
    return {'sources_match':sources_match,'old_sources':old['sources'],'new_sources':new['sources'],
        'MAE_RGB_255':float(delta.mean()),'max_channel_difference_255':int(delta.max()),
        'changed_pixels':changed_pixels,'fraction_pixels_changed':float(np.mean(np.any(delta>0,axis=2))),
        'accepted_channel_tolerance':tolerance,'accepted_changed_pixels':max_changed_pixels,
        'accepted':bool(sources_match and delta.max()<=tolerance and changed_pixels<=max_changed_pixels)}


def saved_off_reference(path, report_sha256, image_sha256, args, pose, assets):
    """Reuse a pinned, previously clean control without starting its executable."""
    if rc.sha256(path)!=report_sha256:raise RuntimeError('saved OFF report hash differs')
    reference=json.loads(path.read_text())
    if not reference.get('complete') or reference.get('mode')!='capture':
        raise RuntimeError('saved OFF report is not a complete capture')
    if not reference.get('old_vs_new_off_image',{}).get('accepted'):
        raise RuntimeError('saved OFF report did not pass its original parity gate')
    if tuple(reference.get('pose',()))!=tuple(pose) or reference.get('assets')!=assets:
        raise RuntimeError('saved OFF camera or assets differ')
    old=reference['runs']['old'];binary=reference['binaries']['old']
    if (old.get('engine_exit_code')!=0 or old.get('driver_errors')!=[] or old.get('validation_errors')!=[] or
            old.get('binary_unchanged') is not True or rc.sha256(Path(binary['path']))!=binary['sha256']):
        raise RuntimeError('saved OFF executable/run was not clean and immutable')
    value=dict(old['pairs'][-1]['old_off']);value['sources']=tuple(value['sources'])
    capture=Path(value['capture'])
    if rc.sha256(capture)!=image_sha256:raise RuntimeError('saved OFF image hash differs')
    viewport(capture,args.width,args.height)
    for state in ('before','after'):
        if verify(value[state],args,pose)!=value['sources']:raise RuntimeError('saved OFF source metadata differs')
    if not 0<=value['simulation_end']-value['simulation_start']<.001:
        raise RuntimeError('saved OFF simulation was not held')
    rc.verify_receiver_frames(value['receiver_frames']['before'],value['receiver_frames']['after'],args.scale!=1)
    return {'source_report':str(path),'report_sha256':report_sha256,'image_sha256':image_sha256,
            'binary':binary,'value':value,'reused_without_launch':True}


def external_games(args, own_pid=None):
    if args.mode == 'capture' and args.allow_existing: return
    if args.expected_paused_pid is None:
        rc.ensure_no_game(False, own_pid)
        return
    others = list(rc.other_game_pids(Path('/proc'), {os.getpid(),own_pid}))
    if others != [args.expected_paused_pid]:
        raise RuntimeError(f'expected only paused game{args.expected_paused_pid}, found {others}; no signal sent')
    status = Path(f'/proc/{args.expected_paused_pid}/status').read_text()
    if not re.search(r'^State:\s+[Tt]\b', status, re.M):
        raise RuntimeError('caller-paused game resumed; no signal sent')


def isolated_poll_source(source):
    """Drop real input only in this private SDL shim; keep injected F12."""
    replacement = '''poll_result result;
    do { result = real_poll(event); }
    while (result && event && (event->type == DOWN || event->type == UP
#ifdef USE_SDL3
        || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP
        || event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_WHEEL
#else
        || event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP
        || event->type == SDL_MOUSEMOTION || event->type == SDL_MOUSEWHEEL
#endif
    ));
    return result;'''
    if source.count('return real_poll(event);') != 1:
        raise RuntimeError('unexpected SDL shim; input isolation was not applied')
    return source.replace('return real_poll(event);', replacement)


def clean_timing_environment():
    keys = ('VK_INSTANCE_LAYERS','VK_LAYER_ENABLES','VK_LAYER_VALIDATE_SYNC','VK_LAYER_SETTINGS_PATH',
            'VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_ALLOW','VK_LOADER_DEBUG',
            'VK_ADD_DRIVER_FILES','VK_LOADER_DRIVERS_SELECT','VK_LOADER_DRIVERS_DISABLE')
    removed = {key:os.environ.pop(key) for key in keys if key in os.environ}
    icd = '/usr/share/vulkan/icd.d/nvidia_icd.json'
    if not Path(icd).is_file():raise RuntimeError('NVIDIA ICD is required for this RTX benchmark')
    os.environ['VK_ICD_FILENAMES']=icd;os.environ['VK_DRIVER_FILES']=icd
    return {'removed_environment':removed,'cleaned_keys':keys,'VK_ICD_FILENAMES':icd,'VK_DRIVER_FILES':icd,
            'validation_requested':False,'optional_stage_queries':False,'receiver_diagnostics':0}


def short_binary_alias(binary):
    """Keep vkQuake's reconstructed startup command below its fixed buffer."""
    directory=tempfile.TemporaryDirectory(prefix='rc-soft-')
    alias=Path(directory.name)/'vkquake';alias.symlink_to(binary.resolve())
    return directory,alias


def boot_executed(text):
    if re.search(r"couldn't exec boot\.",text):
        raise RuntimeError('capture boot config was not executed; possible startup command truncation')
    return 'RC_SOFT_CAPTURE_BOOT' in text


def main():
    import bench_meshlets as bench
    from bench_ecs_ripple import FrameCounter
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--old-off-binary', type=Path)
    p.add_argument('--old-off-reference',type=Path,help='previous complete clean capture report; starts no old process')
    p.add_argument('--old-off-reference-sha256',help='required pinned report hash when reusing a clean control')
    p.add_argument('--old-off-reference-image-sha256',help='required pinned image hash when reusing a clean control')
    p.add_argument('--parity-max-difference', type=float, default=0,
                   help='old/new off capture channel tolerance; default requires bit equivalence')
    p.add_argument('--parity-max-changed-pixels',type=int,default=0,
                   help='bounded old/new off rounding allowance, max32 pixels; default0 remains exact')
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mode', choices=('capture','benchmark'), default='capture')
    p.add_argument('--camera', choices=tuple(CAMERAS), default='doorway')
    p.add_argument('--pose', type=float, nargs=6)
    p.add_argument('--width', type=int, default=1920)
    p.add_argument('--height', type=int, default=1080)
    p.add_argument('--scale', type=rc.resolution_scale, default=2/3)
    p.add_argument('--receiver-filter', choices=('probe','texture'), default='texture')
    p.add_argument('--checkerboard',type=int,choices=(0,1),default=0)
    p.add_argument('--off-only', action='store_true', help='capture safe default startup/off parity without requesting the lazy post')
    p.add_argument('--gaussian', type=rc.probe_gaussian_strength, default=0)
    p.add_argument('--point-gain', type=float, default=1)
    p.add_argument('--emissive-gain', type=float, default=1)
    p.add_argument('--bounce-gain', type=float, default=0)
    p.add_argument('--radius', type=float, default=2)
    p.add_argument('--sun-angle', type=float, default=.5)
    p.add_argument('--lightmap', type=int, choices=(0,1), default=1)
    p.add_argument('--backend', choices=('hardware','software'), default='hardware')
    p.add_argument('--pairs', type=int, default=3)
    p.add_argument('--seconds', type=float, default=1)
    p.add_argument('--warmup', type=float, default=1)
    p.add_argument('--initial-warmup', type=float, default=10)
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--watchdog-seconds', type=float, default=35,
                   help='kill only this test renderer if startup or first lazy ON exceeds this limit')
    p.add_argument('--exit-grace-seconds',type=float,default=0,
                   help='idle after final capture/sample before clean shutdown; excluded from timing windows')
    p.add_argument('--allow-existing', action='store_true')
    p.add_argument('--expected-paused-pid', type=int)
    p.add_argument('--validation', action='store_true')
    args = p.parse_args()
    if args.mode == 'benchmark' and (args.allow_existing or args.validation):
        p.error('benchmark requires isolated GPU and no validation; use --expected-paused-pid only with caller coordination')
    if args.mode=='benchmark' and args.off_only:p.error('--off-only is a capture/default-startup check')
    if args.old_off_reference and args.old_off_binary:p.error('choose an old executable or saved clean OFF reference')
    if bool(args.old_off_reference)!=bool(args.old_off_reference_sha256 and args.old_off_reference_image_sha256):
        p.error('saved OFF reference requires pinned report and image SHA256 hashes')
    if (args.pairs < 1 or args.seconds <= 0 or args.warmup <= 0 or args.initial_warmup < 10 or
            not all(math.isfinite(v) for v in (args.seconds,args.warmup,args.initial_warmup,args.radius,args.sun_angle)) or
            not 0 <= args.radius <= 64 or not 0 <= args.sun_angle <= 15 or
            not 0 <= args.parity_max_difference <= 1 or not 0<=args.parity_max_changed_pixels<=32 or
            not 0 < args.watchdog_seconds <= 40 or
            not math.isfinite(args.exit_grace_seconds) or not 0<=args.exit_grace_seconds<=10):
        p.error('finite positive durations/initial10s and radius0..64/sun-angle0..15 are required')
    external_games(args)
    environment = clean_timing_environment() if args.mode=='benchmark' else None
    bench.SHIM = isolated_poll_source(bench.SHIM)
    root = args.output.resolve(); root.mkdir(parents=True, exist_ok=False)
    source = args.basedir.resolve()
    if (source/'id1').is_dir(): source /= 'id1'
    paks = sorted(source.glob('pak*.pak'))
    if not paks: p.error('Quake PAKs are required')
    pose = tuple(args.pose) if args.pose is not None else CAMERAS[args.camera]
    report = {'complete':False, 'mode':args.mode, 'authoritative_timing':False,
        'eligible_for_authoritative_timing':args.mode=='benchmark' and args.pairs>=5 and args.seconds>=2,
        'settings':{key:value for key,value in vars(args).items() if key not in ('binary','old_off_binary','old_off_reference','basedir','output')},
        'pose':pose, 'assets':{str(pak):rc.sha256(pak) for pak in paks}, 'binaries':{}, 'runs':{},
        'input_isolation':'Real SDL key/mouse events ignored only in the private test process; injected command key preserved.',
        'timing_environment':environment,
        'first_on_cache_scope':'First ON in a fresh renderer process; disk driver/pipeline caches are not cleared.',
        'note':'Old-off timing is a separate sequential control block. Only new off/on rows are rotating paired comparisons. Captures do not establish contact-hardening quality automatically.'}
    (root/'harness').mkdir()
    for name in (Path(__file__).name,'bench_radiance.py','bench_meshlets.py','bench_ecs_ripple.py'):
        shutil.copy2(Path(__file__).with_name(name), root/'harness'/name)
    report['harness_sha256']={p.name:rc.sha256(p) for p in (root/'harness').iterdir()}
    result_path = root/'results.json'
    def save(): result_path.write_text(json.dumps(report,indent=2)+'\n')
    def old_reference():
        return (report['old_off_reference']['value'] if args.old_off_reference else
                report['runs']['old']['pairs'][-1]['old_off'])
    has_reference=bool(args.old_off_binary or args.old_off_reference)
    def run(tag, source_binary, legacy):
        work=root/tag; (work/'id1').mkdir(parents=True); (work/'user').mkdir()
        frozen=work/'binary/vkquake'; frozen.parent.mkdir(); shutil.copy2(source_binary.resolve(),frozen)
        report['binaries'][tag]={'source_path':str(source_binary.resolve()),'path':str(frozen),'sha256':rc.sha256(frozen)}
        for pak in paks:(work/'id1'/pak.name).symlink_to(pak)
        startup=(f'host_timescale .000001\nscr_centertime -1\nr_rtshadows 0\nr_radiance 1\nr_radiance_backend {2 if args.backend=="hardware" else 1}\n'
            f'r_radiance_quality 0\nr_radiance_grid 1\nr_radiance_point_gain {args.point_gain}\nr_radiance_emissive_gain {args.emissive_gain}\n'
            f'r_radiance_bounce_gain {args.bounce_gain}\nr_radiance_sky_fallback .15\nr_radiance_resolution_scale {args.scale:.12g}\n'
            f'r_radiance_receiver_filter {int(args.receiver_filter=="texture")}\nr_radiance_checkerboard {args.checkerboard}\nr_radiance_receiver_diagnostics 0\nr_radiance_probe_gaussian {args.gaussian}\n'
            f'r_lightmap {args.lightmap}\nr_radiance_highlight 0\nfov 90\nviewsize 130\nr_drawviewmodel 0\ncrosshair 0\ncl_bob 0\ncl_rollangle 0\n'
            'echo RC_SOFT_CAPTURE_BOOT\n')
        if not legacy:startup+=f'r_radiance_shadow_radius {args.radius}\nr_radiance_shadow_sun_angle {args.sun_angle}\n'
        (work/'id1/boot.cfg').write_text(bench.BOOT.replace('vid_fsaa 0','vid_fsaa 1').replace('r_indirect 1','r_indirect 0').replace('map start',startup+'map e1m1'))
        launch_directory,launch_binary=short_binary_alias(frozen)
        config=SimpleNamespace(binary=launch_binary,width=args.width,height=args.height,timeout=args.timeout,validation=args.validation,game=None)
        launch_begin=time.monotonic()
        report['runs'][tag]={'pairs':[],'startup':{'launch_begin_unix':time.time()}}
        engine=bench.Engine(config,work); counter=None; rows=[]; cold_on_started=None
        startup_record=report['runs'][tag]['startup']
        startup_record['short_launch_binary']=str(launch_binary)
        startup_record['engine_constructor_elapsed_s']=time.monotonic()-launch_begin
        startup_record['process_pid']=engine.process.pid
        save()
        watchdog_stop=threading.Event();watchdog_status={}
        def watchdog():
            while not watchdog_stop.wait(.1):
                now=time.monotonic();reason=None
                if args.mode=='benchmark':
                    try:external_games(args,engine.process.pid)
                    except RuntimeError as exc:reason='benchmark external-game guard: '+str(exc)
                if 'launch_to_first_ready_s' not in startup_record and now-launch_begin>args.watchdog_seconds:
                    reason='startup/first receiver-ready deadline'
                if cold_on_started is not None and 'cold_on_request_to_verified_state_s' not in startup_record and now-cold_on_started>args.watchdog_seconds:
                    reason='cold ON actual-post deadline'
                if reason and engine.process.poll() is None:
                    watchdog_status.update(reason=reason,limit_s=args.watchdog_seconds,own_pid=engine.process.pid)
                    engine.process.kill()
                    return
        watchdog_thread=threading.Thread(target=watchdog,daemon=True);watchdog_thread.start()
        def cmd(body): external_games(args,engine.process.pid); return engine.cmd(body)
        try:
            engine.wait(boot_executed)
            startup_record['launch_to_boot_config_s']=time.monotonic()-launch_begin
            engine.wait(lambda text:'CL_SignonReply: 4' in text)
            startup_record['launch_to_signon_s']=time.monotonic()-launch_begin
            if not legacy:
                default=cmd('r_radiance_soft_shadows\nradiance_stats')
                verify_cvar(default,'r_radiance_soft_shadows',0)
                if not re.search(r'"r_radiance_soft_shadows" is "0" \(default\)',default):
                    raise RuntimeError('soft shadows did not start at engine default0')
                startup_record['default_off_state']=default
                startup_record['default_off_verified']=True
                save()
            clock_setup=time.monotonic()
            counter=FrameCounter(engine.process.pid,frozen);clock=rc.SimulationClock(counter,frozen)
            startup_record['read_only_clock_setup_s']=time.monotonic()-clock_setup
            cmd('setpos '+' '.join(map(str,pose)))
            query=QUERY+('' if legacy else '\nr_radiance_soft_shadows\nr_radiance_shadow_radius\nr_radiance_shadow_sun_angle')
            initial=True; sources=None
            for pair in range(1 if args.mode=='capture' else args.pairs):
                row={}
                for mode in (('old_off',) if legacy else ('off',) if args.off_only else rc.alternating_order(pair,'off','on')):
                    enabled=mode=='on'
                    switch_begin=time.monotonic()
                    if enabled and cold_on_started is None:
                        cold_on_started=switch_begin
                        startup_record['cold_on_request_unix']=time.time();save()
                    if not legacy:cmd(f'r_radiance_soft_shadows {int(enabled)}')
                    deadline=time.monotonic()+args.timeout
                    while True:
                        state=cmd(query)
                        try:
                            verify(state,args,pose,None if legacy else enabled)
                            if not rc.radiance_ready(state):raise RuntimeError('prepass not ready')
                            break
                        except RuntimeError:
                            if time.monotonic()>=deadline:raise
                            time.sleep(.1)
                    if 'launch_to_first_ready_s' not in startup_record:
                        startup_record['launch_to_first_ready_s']=time.monotonic()-launch_begin
                    activation_elapsed=time.monotonic()-switch_begin
                    expected_post=post_expected(args.scale,args.receiver_filter,enabled)
                    if enabled and 'cold_on_request_to_verified_state_s' not in startup_record:
                        startup_record['cold_on_request_to_verified_state_s']=time.monotonic()-cold_on_started
                        startup_record['cold_on_actual_post_confirmed']=bool(expected_post)
                    time.sleep((args.initial_warmup if initial else 0)+max(args.warmup,2 if args.mode=='capture' else 0));initial=False
                    before=cmd(query);current=verify(before,args,pose,None if legacy else enabled)
                    if sources is not None and current!=sources:raise RuntimeError('source counts changed')
                    sources=current
                    if not legacy and mode=='off' and has_reference:
                        reference_sources=old_reference()['sources']
                        report['off_sources_before_capture']={'old':reference_sources,'new':current,'match':reference_sources==current};save()
                        if reference_sources!=current:raise RuntimeError('old/new sources differ before off capture; no ambiguous RGB pair or ON requested')
                    start_clock=clock.sample()
                    if args.mode=='benchmark':
                        cmd('gpu_frame_time start');t0,f0=counter.sample();time.sleep(args.seconds)
                        external_games(args,engine.process.pid);t1,f1=counter.sample();timers=cmd('gpu_frame_time stop')
                        gpu=rc.parse_gpu(timers)
                        if gpu.get('median_truncated') or gpu.get('p95_truncated'):raise RuntimeError('truncated GPU samples')
                        frames=(f1-f0)&0xffffffff
                        if not frames or t1<=t0:raise RuntimeError('no measured frames')
                        value={'whole_frame_gpu':gpu,'whole_frame_wall_ms':1000*(t1-t0)/frames,'fps':frames/(t1-t0),'timers':timers}
                    else:value={}
                    end_clock=clock.sample()
                    if not 0<=end_clock-start_clock<.001:raise RuntimeError('simulation advanced')
                    after=cmd(query);verify(after,args,pose,None if legacy else enabled)
                    if rc.verify_sources(after,args.point_gain,args.emissive_gain,args.bounce_gain)!=sources:raise RuntimeError('source counts changed during sample')
                    value.update(before=before,after=after,sources=sources,simulation_start=start_clock,simulation_end=end_clock,
                        mode_activation_elapsed_s=activation_elapsed,actual_soft_post_expected=bool(expected_post))
                    if args.mode=='capture' or pair==args.pairs-1 or (not legacy and mode=='off' and pair==0 and has_reference):
                        capture_label=mode if args.mode=='capture' else f'{mode}-pair{pair+1}'
                        image=engine.photo(capture_label);value['capture']=str(image);value['actual_viewport']=viewport(image,args.width,args.height)
                        capture_state=cmd(query);value.update(capture_state=capture_state,capture_verified=False)
                        # A written PNG is only provisional until its actual post
                        # state and enclosing submission window pass every guard.
                        # Preserve the failed evidence if any following check throws.
                        row[mode]=value;report['runs'][tag]['pairs']=rows+[row];save()
                        verify(capture_state,args,pose,None if legacy else enabled)
                        if args.mode=='capture':
                            end_clock=clock.sample()
                            if not 0<=end_clock-start_clock<.001:raise RuntimeError('simulation advanced during capture')
                            value['simulation_end']=end_clock
                    else:capture_state=after
                    value['receiver_frames']=rc.verify_receiver_frames(rc.receiver_frames(before),rc.receiver_frames(capture_state),args.scale!=1)
                    if not legacy:
                        soft_before=soft_frames(before);soft_after=soft_frames(capture_state)
                        value['soft_shadow_frames']={'before':soft_before,'after':soft_after,
                            'expected_active':bool(expected_post),'verified':False}
                        row[mode]=value;report['runs'][tag]['pairs']=rows+[row];save()
                        value['soft_shadow_frames']=verify_soft_frames(soft_before,soft_after,expected_post)
                        value['soft_shadow_frames']['verified']=True
                    if 'capture' in value:value['capture_verified']=True
                    row[mode]=value
                    report['runs'][tag]['pairs']=rows+[row];save()
                    if not legacy and mode=='off' and has_reference and 'capture' in value:
                        evidence=off_pair_evidence(old_reference(),value,args.parity_max_difference,args.parity_max_changed_pixels)
                        value['off_reference_evidence']=evidence
                        report['old_vs_new_off_image']=evidence
                        report.setdefault('off_parity_gate_passed_before_on',evidence['accepted']);save()
                        if not evidence['accepted']:raise RuntimeError('old/new off sources or pixels differ; no subsequent ON requested')
                    print(f'{tag} {pair+1} {mode}: '+(f'{value["whole_frame_gpu"]["median_ms"]}ms GPU, {value["fps"]:.1f}FPS' if args.mode=='benchmark' else value['capture']),flush=True)
                rows.append(row);report['runs'][tag]['pairs']=rows;save()
        finally:
            if counter:counter.close()
            if engine.process.poll() is None and args.exit_grace_seconds:
                grace_begin=time.monotonic();time.sleep(args.exit_grace_seconds)
                startup_record['final_idle_grace_s']=time.monotonic()-grace_begin
            try:engine.close()
            finally:
                watchdog_stop.set();watchdog_thread.join(timeout=1)
            log=(work/'output.log').read_text(errors='replace')
            report['runs'].setdefault(tag,{'pairs':rows}).update(engine_exit_code=engine.process.returncode,
                validation_errors=ERRORS.findall(log),driver_errors=DRIVER_ERRORS.findall(log),
                binary_unchanged=rc.sha256(frozen)==report['binaries'][tag]['sha256'])
            if watchdog_status:startup_record['own_process_watchdog']=watchdog_status
            startup_record['launch_to_exit_s']=time.monotonic()-launch_begin
            if cold_on_started is not None and 'cold_on_request_to_verified_state_s' not in startup_record:
                startup_record['cold_on_unresolved_elapsed_s']=time.monotonic()-cold_on_started
            save()
            launch_directory.cleanup()
            if engine.process.returncode!=0:raise RuntimeError(f'engine exited{engine.process.returncode}')
            if ERRORS.search(log):raise RuntimeError('Vulkan validation errors')
            if DRIVER_ERRORS.search(log):raise RuntimeError('driver compilation/device error; captures do not prove a clean runtime')
            if args.mode=='benchmark' and 'Using VK_LAYER_KHRONOS_validation' in log:raise RuntimeError('validation loaded during timing')
        return rows
    try:
        if args.old_off_reference:
            report['old_off_reference']=saved_off_reference(args.old_off_reference.resolve(),args.old_off_reference_sha256,
                args.old_off_reference_image_sha256,args,pose,report['assets'])
            save()
        if args.old_off_binary:run('old',args.old_off_binary,True)
        rows=run('new',args.binary,False)
        if args.mode=='benchmark':report['off_on_summary']=rc.summarize_pairs(rows,'off','on')
        if has_reference:
            old_sources=old_reference()['sources']
            new_sources=rows[-1]['off']['sources']
            report['old_vs_new_sources_match']=old_sources==new_sources
            if old_sources!=new_sources:raise RuntimeError(f'old/new source counts differ: {old_sources} vs {new_sources}; off parity invalid')
            evidence=off_pair_evidence(old_reference(),rows[-1]['off'],args.parity_max_difference,args.parity_max_changed_pixels)
            report['old_vs_new_off_image']=evidence
            if not evidence['accepted']:raise RuntimeError('old/new off image parity failed; timing/effect evidence is not accepted')
            if args.mode=='benchmark' and args.old_off_binary:
                old_times=[r['old_off']['whole_frame_gpu']['median_ms'] for r in report['runs']['old']['pairs']]
                new_times=[r['off']['whole_frame_gpu']['median_ms'] for r in rows]
                report['old_off_sequential_control']={'old_run_medians_ms':old_times,'new_off_run_medians_ms':new_times,
                    'median_ratio_change_percent':100*(statistics.median(new_times)/statistics.median(old_times)-1),'interleaved_paired':False}
        report['complete']=True
        report['authoritative_timing']=report['eligible_for_authoritative_timing']
    except Exception as exc:
        report['failure']={'type':type(exc).__name__,'message':str(exc)}
        raise
    finally:save()


if __name__=='__main__':main()
