#!/usr/bin/env python3
"""Actual editor→BSPX→renderer/player/nail collision smoke, isolated files.

Requires the identity-joint Sponza MD5 asset, local Quake PAKs, FTEQCC and an
SDL/Vulkan display. --prepare-only builds fixture files/QC without opening GPU.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

import numpy as np
import bench_meshlets as bench
import smoke_editor  # Installs the real editor console/F9/Tab SDL input shim.
from qbsp_smoke import brush
from smoke_sector_props_runtime import origin as edict_origin
from static_models_collision_sponza import export_fixture
from bench_radiance import ensure_no_game

bench.SHIM = bench.SHIM.replace("action == 't' ? SDL_SCANCODE_TAB :",
    "action == 't' ? SDL_SCANCODE_TAB : action == '4' ? SDL_SCANCODE_4 :")

ROOT = Path(__file__).resolve().parents[1]
MODEL = 'progs/sponza.md5mesh'
TRIANGLES = 262266
VALIDATION = {
    'VK_INSTANCE_LAYERS': 'VK_LAYER_KHRONOS_validation',
    'VK_LAYER_ENABLES': 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT',
}
CASES = (
    dict(name='model_on', scale=4, yaw=0, origin=(0,0,64), collision=1,
         drop=(-40,-250,650), rest=(-40,-250,87.792778), yaw_fire=0),
    dict(name='model_off', scale=4, yaw=0, origin=(0,0,64), collision=0,
         drop=(-40,-250,650), rest=(-40,-250,-103.96875), yaw_fire=0),
    dict(name='model_transform', scale=3.5, yaw=37, origin=(200,-100,64), collision=1,
         drop=(304.019361,-284.564278,650), rest=(304.019348,-284.564270,87.822586), yaw_fire=37),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_map(path, wad, case):
    walls = ((-1040,-1040,-144,1040,1040,-128), (-1040,-1040,1024,1040,1040,1040),
             (-1040,-1040,-128,-1024,1040,1024), (1024,-1040,-128,1040,1040,1024),
             (-1024,-1040,-128,1024,-1024,1024), (-1024,1024,-128,1024,1040,1024))
    text = '{\n"classname" "worldspawn"\n"wad" "' + wad.as_posix() + '"\n'
    text += ''.join(brush(*wall) for wall in walls) + '}\n'
    text += '{\n"classname" "info_player_start"\n"origin" "-800 0 -104"\n"angle" "0"\n}\n'
    text += '{\n"classname" "light"\n"origin" "0 0 800"\n"light" "1000"\n}\n'
    text += ('{\n"classname" "quiver_static_model"\n"model" "' + MODEL + '"\n'
             '"origin" "' + ' '.join(map(str,case['origin'])) + '"\n'
             '"angles" "0 ' + str(case['yaw']) + ' 0"\n'
             '"modelscale" "' + str(case['scale']) + '"\n"skin" "0"\n'
             '"collision" "' + str(case['collision']) + '"\n}\n')
    path.write_text(text)


def prepare(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    work = output / 'work'
    (work / 'id1').mkdir(parents=True)
    (work / args.game / 'maps').mkdir(parents=True)
    (work / 'user').mkdir()
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak',p.name.lower()))
    if not paks:
        raise RuntimeError(f'no Quake PAKs in {source}')
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    assets = args.assets.resolve()
    if not (assets / MODEL).is_file():
        raise RuntimeError(f'missing {assets / MODEL}')
    (work / args.game / 'progs').symlink_to(assets / 'progs', target_is_directory=True)
    if (assets / 'textures').is_dir():
        (work / args.game / 'textures').symlink_to(assets / 'textures', target_is_directory=True)
    wad = work / 'editor_defaults.wad'
    wad.symlink_to(ROOT / 'Misc/editor_defaults.wad')
    for case in CASES:
        write_map(work / args.game / 'maps' / (case['name'] + '.map'), wad, case)
    qc = output / 'qc'
    shutil.copytree(ROOT / 'ThirdParty/id1-quakec', qc)
    weapons = qc / 'weapons.qc'
    text = weapons.read_text()
    # Observe actual MOVETYPE_FLYMISSILE touch callbacks. No movement, collision
    # or weapon behavior is replaced by the disposable instrumentation.
    needle = 'if (pointcontents(self.origin) == CONTENT_SKY)'
    begin = text.index('void() spike_touch =')
    at = text.index(needle,begin)
    text = (text[:at] + 'dprint("QSM_SMOKE_NAIL_TOUCH "); dprint(vtos(self.origin));\n'
            '\tdprint(" world="); dprint(ftos(other == world)); dprint("\\n");\n\t' + text[at:])
    weapons.write_text(text)
    src = qc / 'progs.src'
    lines = src.read_text().splitlines()
    lines[0] = '../work/' + args.game + '/progs.dat'
    src.write_text('\n'.join(lines) + '\n')
    result = subprocess.run([str(args.qcc.resolve())], cwd=qc, capture_output=True, text=True, timeout=90)
    (output / 'qcc.log').write_text(result.stdout + result.stderr)
    if result.returncode or not (work / args.game / 'progs.dat').is_file():
        raise RuntimeError('fixture QC compilation failed; see qcc.log')
    local = output / 'local-triangles.bin'
    export_fixture(assets / MODEL,local)
    boot = bench.BOOT.replace('r_indirect 1','r_indirect 0').replace('map start',
        'host_maxfps 72\nvid_fsaa 1\nr_rtshadows 0\nr_radiance 0\nr_radiance_soft_shadows 0\n'
        'r_radiance_profile 1\nr_radiance_quality 0\nr_radiance_grid 0\nr_radiance_probe_spacing 128\n'
        'r_radiance_resolution_scale 0.666\nr_radiance_receiver_filter 1\nr_radiance_checkerboard 1\n'
        'r_radiance_probe_gaussian 2\nmap start')
    (work / args.game / 'boot.cfg').write_text(boot)
    manifest = dict(binary=str(args.binary.resolve()), binary_sha256=sha(args.binary.resolve()),
                    asset=str(assets / MODEL), asset_sha256=sha(assets / MODEL), cases=CASES,
                    note='Private game, PAK/WAD/model links; only fixture spike_touch logging modifies QuakeC.')
    (output / 'manifest.json').write_text(json.dumps(manifest,indent=2) + '\n')
    return output,work,manifest


def decode_bsp(path,case,local):
    data = path.read_bytes()
    if data[:4] not in (b'BSP2', b'2PSB', struct.pack('<I',29)):
        raise RuntimeError('base BSP version changed unexpectedly')
    lumps = [struct.unpack_from('<II',data,4+i*8) for i in range(15)]
    end = max(124,*(offset+length for offset,length in lumps))
    extension = (end+3)&~3
    if data[extension:extension+4] != b'BSPX':
        raise RuntimeError('editor playtest did not attach BSPX after LIGHT')
    count, = struct.unpack_from('<I',data,extension+4)
    found = []
    for i in range(count):
        name,offset,length = struct.unpack_from('<24sII',data,extension+8+i*32)
        if name.rstrip(b'\0') == b'Q_STATIC_MODELS':
            found.append(data[offset:offset+length])
    if len(found) != 1:
        raise RuntimeError('expected exactly one Q_STATIC_MODELS lump')
    payload = found[0]
    version,instances,triangles,record = struct.unpack_from('<4I',payload)
    if (version,instances,triangles,record) != (1,1,TRIANGLES,300):
        raise RuntimeError(f'wrong static model payload {(version,instances,triangles,record)}')
    model = payload[16:272].split(b'\0',1)[0].decode()
    values = struct.unpack_from('<7f4I',payload,16+256)
    expected = (*case['origin'],0,case['yaw'],0,case['scale'],0,case['collision'],0,TRIANGLES)
    if model != MODEL or values != expected:
        raise RuntimeError(f'wrong compiled transform/flags: {model} {values}')
    actual = np.frombuffer(payload,dtype='<f4',offset=16+300).reshape(TRIANGLES,3,3)
    angle = math.radians(case['yaw'])
    rotation = np.array([[math.cos(angle),-math.sin(angle),0],
                         [math.sin(angle),math.cos(angle),0],[0,0,1]],dtype=np.float32)
    expected_triangles = (local * np.float32(case['scale'])) @ rotation.T + np.asarray(case['origin'],dtype=np.float32)
    error = float(np.abs(actual-expected_triangles).max())
    if error > .0004:
        raise RuntimeError(f'compiled geometry differs from independent asset transform: max error {error}')
    entity_offset,entity_length = lumps[0]
    if b'quiver_static_model' in data[entity_offset:entity_offset+entity_length]:
        raise RuntimeError('compile-only model entity leaked into QuakeC entity text')
    model_offset,_ = lumps[14]
    first_face,face_count=struct.unpack_from('<II',data,model_offset+56)
    face_offset,face_bytes=lumps[7];face_size=20 if data[:4]==struct.pack('<I',29) else 28
    if face_bytes%face_size:raise RuntimeError('invalid world BSP face array')
    edge_offset,edge_format=(8,'<H') if face_size==20 else (12,'<I')
    world_triangles=sum(max(0,struct.unpack_from(edge_format,data,face_offset+i*face_size+edge_offset)[0]-2)
                        for i in range(first_face,first_face+face_count))
    return dict(bsp=str(path),bsp_sha256=sha(path),bsp_version=data[:4].hex(),instances=instances,
                triangles=triangles,max_transform_error=error,collision=case['collision'],
                world_bsp_triangles=world_triangles,
                bounds_min=actual.min(axis=(0,1)).tolist(),bounds_max=actual.max(axis=(0,1)).tolist())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--basedir',type=Path,required=True)
    parser.add_argument('--assets',type=Path,required=True)
    parser.add_argument('--qcc',type=Path,default=ROOT / 'build/fteqcc')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--expected-sha256')
    parser.add_argument('--timeout',type=float,default=120)
    parser.add_argument('--validation',action='store_true')
    parser.add_argument('--prepare-only',action='store_true')
    args = parser.parse_args()
    args.binary=args.binary.resolve();args.width,args.height=1280,720;args.game='qsm_editor'
    if args.expected_sha256 and sha(args.binary) != args.expected_sha256:
        parser.error('renderer hash differs from the coordinated candidate')
    output,work,manifest=prepare(args)
    print(f'static-model smoke: {output}',flush=True)
    if args.prepare_only:
        print('PREPARED: fixture maps, private QC, assets and local geometry; no GPU opened',flush=True)
        return
    if args.validation:
        os.environ.update(VALIDATION)
    ensure_no_game(False)
    os.environ['SMOKE_WIDTH']=str(args.width);os.environ['SMOKE_HEIGHT']=str(args.height)
    local=np.fromfile(output / 'local-triangles.bin',dtype='<f4',offset=4).reshape(TRIANGLES,3,3)
    report=dict(complete=False,manifest=manifest,cases=[],checks=[])
    engine=bench.Engine(args,work)

    def command(body,editor=False,close_console=True):
        engine.serial+=1;marker=f'QSM_EDITOR_{engine.serial}_DONE';start=len(engine.text())
        content=body+'\necho '+marker+'\n'+('toggleconsole\n' if editor and close_console else '')
        temp=work / args.game / 'control.tmp';temp.write_text(content);temp.replace(temp.with_name('control.cfg'))
        os.write(engine.key,b'c' if editor else b'k')
        return engine.wait(lambda text:text if marker in text else None,start)

    def capture(label,editor=False):
        start=len(engine.text())
        # Disconnected toggleconsole opens the main menu; editor_return owns
        # the proper editor key destination and preserves its source view.
        command(('editor_return\n' if editor else '')+'wait\nwait\nscreenshot png',editor,False)
        match=engine.wait(lambda text:re.search(r'Wrote [\x01\x02]?([^\n]+\.png)',text),start)
        source=next(work.rglob(match[1]),None)
        if source is None:raise RuntimeError('capture missing')
        target=output/(label+'.png');shutil.copy2(source,target);return str(target)

    def stats(case,baked):
        text=command('static_models_stats')
        summary=re.search(r'StaticModels: instances=(\d+) triangles=(\d+) collision=(\d+) client_compiled=(\d+)',text)
        if not summary or tuple(map(int,summary.groups())) != (1,TRIANGLES,case['collision'],1):
            raise RuntimeError('compiled model did not load into the real client/world:\n'+text)
        bounds=re.search(r'StaticModels: index=0 .*?mins=\(([^)]+)\) maxs=\(([^)]+)\)',text)
        if not bounds:raise RuntimeError('missing baked model bounds:\n'+text)
        if not re.search(r'StaticModels: index=0 .*?client_bounds_match=1\b',text):
            raise RuntimeError('actual client entity bounds do not match baked triangles:\n'+text)
        for actual,expected in zip(bounds.groups(),(baked['bounds_min'],baked['bounds_max'])):
            if not np.allclose(np.fromstring(actual,sep=' '),expected,rtol=0,atol=.001):
                raise RuntimeError('runtime baked bounds differ from compiled BSPX')
        return text

    def fall(case,radiance):
        command(f'r_radiance {radiance}\nsetpos '+ ' '.join(map(str,case['drop']))+' 0 '+str(case['yaw_fire'])+' 0\nnoclip\nwait\nwait')
        deadline=time.monotonic()+12;last=None;stable=0
        while time.monotonic()<deadline:
            detail=command('edict 1');position=edict_origin(detail)
            if abs(position[2]-case['rest'][2])<.6 and last is not None and abs(position[2]-last)<.05:
                stable+=1
                if stable>=2:return dict(radiance=radiance,origin=position,edict=detail)
            else:stable=0
            last=position[2];time.sleep(.15)
        raise RuntimeError(f'player did not rest on expected mesh/brush floor: {case["name"]} {last}')

    def nails(case,radiance):
        # Hold the firing origin fixed even in collisionOFF control; only the
        # player is noclip, while actual nail missiles use ordinary SV_Move.
        firing=CASES[0]['rest'] if not case['collision'] else case['rest']
        command(f'r_radiance {radiance}\nsetpos '+ ' '.join(map(str,firing))+' 0 '+str(case['yaw_fire'])+' 0\nimpulse 9')
        # Give the server a weapon-think interval; a later impulse must not
        # overwrite the cheat/ammo request while cold rendering is paused.
        time.sleep(.3);command('impulse 4');time.sleep(.25)
        start=len(engine.text());command('+attack');time.sleep(.45);command('-attack')
        engine.wait(lambda text:'QSM_SMOKE_NAIL_TOUCH ' in text,start)
        time.sleep(1.3)
        touches=[tuple(map(float,m[:3])) for m in re.findall(
            r"QSM_SMOKE_NAIL_TOUCH\s+'?\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)'?\s+world=1",engine.text()[start:])]
        if not touches:raise RuntimeError('no actual world nail impacts were logged')
        distances=[math.dist(point,firing) for point in touches]
        if case['collision']:
            if not all(60<distance<180 for distance in distances):
                raise RuntimeError(f'nails missed compiled mesh: {distances} {touches}')
        elif not all(distance>700 for distance in distances):
            raise RuntimeError(f'collisionOFF still blocked nails: {distances} {touches}')
        return dict(radiance=radiance,touches=touches,distances=distances)

    try:
        engine.wait(lambda text:'CL_SignonReply: 4' in text)
        compiled={}
        for case in CASES:
            ensure_no_game(False,engine.process.pid)
            opened=command('disconnect\neditor_open '+case['name'])
            if 'Source map opened' not in opened:raise RuntimeError('editor_open failed:\n'+opened)
            os.write(engine.key,b'4');time.sleep(.1)
            previews=[capture(case['name']+'-editor-3d',True)]
            os.write(engine.key,b't');time.sleep(.1);previews.append(capture(case['name']+'-editor-ortho',True))
            old=set((work/args.game/'maps').glob('__quiver_editor_*.bsp'));start=len(engine.text())
            command('editor_play',True)
            engine.wait(lambda text:'Starting playtest' in text,start)
            engine.wait(lambda text:'CL_SignonReply: 4' in text,start)
            stage_log=engine.text()[start:]
            if not all(s in stage_log for s in ('Calculating visibility','Calculating lighting')):
                raise RuntimeError('editor did not run QBSP/VIS/LIGHT')
            new=set((work/args.game/'maps').glob('__quiver_editor_*.bsp'))-old
            if len(new)!=1:raise RuntimeError('editor did not publish exactly one new BSP')
            bsp=new.pop();baked=decode_bsp(bsp,case,local);compiled[case['name']]=bsp.stem
            runtime=stats(case,baked);command('god\nnotarget\nhost_maxfps 72')
            entry=dict(case=case,baked=baked,runtime_stats=runtime,previews=previews,falls=[],nails=[])
            report['cases'].append(entry)
            # Cold pipeline/atlas/AS initialization can block rendering for
            # several seconds. Complete those frames before timing a physical
            # fall; wall time spent compiling is not simulated gravity time.
            warm=command('r_radiance 1\nwait\nwait\nwait\nradiance_stats\nstatic_models_stats')
            if not re.search(r'Radiance: requested=1 active=1\b',warm):
                raise RuntimeError('Radiance did not become ready before physics controls')
            entry['warmup']=warm
            for radiance in (0,1):
                entry['falls'].append(fall(case,radiance));entry['nails'].append(nails(case,radiance))
                lighting=command('radiance_stats');entry[f'radiance_{radiance}']=lighting
                if not re.search(rf'Radiance: requested={radiance} active={radiance}\b',lighting):
                    raise RuntimeError('requested Radiance collision control did not become active')
                geometry=stats(case,baked);entry[f'geometry_{radiance}']=geometry
                if radiance:
                    match=re.search(r'StaticModels: radiance_active=1 scene_triangles=(\d+) static_triangles=(\d+) base_triangles=(\d+) compiled_triangles=(\d+)',geometry)
                    if not match:raise RuntimeError('missing actual Radiance prepared geometry counts:\n'+geometry)
                    scene_count,static_count,base_count,compiled_count=map(int,match.groups())
                    if compiled_count!=TRIANGLES or static_count!=TRIANGLES+baked['world_bsp_triangles'] or base_count!=static_count or scene_count<base_count:
                        raise RuntimeError('compiled mesh was omitted or duplicated in Radiance base:\n'+geometry)
                (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
            entry['capture']=capture(case['name']+'-playtest')
            (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        restart=command('vid_restart\nwait\nwait\nr_enhancedmodels 0\nwait\nwait')
        after_restart=stats(CASES[-1],report['cases'][-1]['baked'])
        if not re.search(r'StaticModels: radiance_active=1 .*compiled_triangles=262266',after_restart):
            raise RuntimeError('vid_restart/enhanced toggle lost retained compiled lighting geometry')
        report['checks'].append(dict(name='vid-restart-exact-format',restart_output=restart,stats=after_restart))
        command('r_enhancedmodels 1\nwait\nwait')
        report['checks'].append(dict(name='enhanced-restore',output=stats(CASES[-1],report['cases'][-1]['baked'])))
        start=len(engine.text());command('r_radiance 0\nmap start');engine.wait(lambda text:'CL_SignonReply: 4' in text,start)
        cleanup=command('static_models_stats')
        if not re.search(r'StaticModels: instances=0 triangles=0 collision=0 client_compiled=0',cleanup):
            raise RuntimeError('stock map retained compiled model state')
        report['checks'].append(dict(name='stock-map-cleanup',output=cleanup))
        start=len(engine.text());command('map '+compiled['model_on']);engine.wait(lambda text:'CL_SignonReply: 4' in text,start)
        report['checks'].append(dict(name='compiled-map-reload',output=stats(CASES[0],report['cases'][0]['baked'])))
        if re.search(r'No spawn function[^\n]*quiver_static_model|Host_Error|Sys_Error',engine.text()):
            raise RuntimeError('compiled entity leaked to QC or renderer reported a fatal error')
        (work/args.game/'control.cfg').write_text('quit\n');os.write(engine.key,b'k');engine.process.wait(timeout=20)
        if engine.process.returncode!=0:raise RuntimeError('renderer teardown failed')
        report['complete']=True
    finally:
        if engine.process.poll() is None:
            engine.process.terminate();engine.process.wait(timeout=20)
        engine.close()
        report['exit_code']=engine.process.returncode
        report['binary_sha256_after']=sha(args.binary)
        report['hash_unchanged']=report['binary_sha256_after']==manifest['binary_sha256']
        logs=(work/'output.log').read_text(errors='replace')+'\n'+engine.text()
        shutil.copy2(work/'output.log',output/'output.log')
        (output/'console.log').write_text(engine.text())
        errors=re.findall(r'VUID-[^\n]+|SYNC-HAZARD[^\n]+|Validation Error[^\n]+',logs)
        report['validation_errors']=errors
        report['validation_layer_present']=('Using VK_LAYER_KHRONOS_validation' in logs or
            'Validation Warning: [ VALIDATION-SETTINGS ]' in logs or
            re.search(r'Insert instance layer.*VK_LAYER_KHRONOS_validation',logs) is not None)
        if errors or not report['hash_unchanged'] or (args.validation and not report['validation_layer_present']):report['complete']=False
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['complete']:raise RuntimeError('validation/hash/teardown checks failed; see results.json')
    print('PASS: actual editor/BSPX/client models, player floor and nail wall collision with RadianceOFF/ON, collisionOFF and transformed controls, reload cleanup',flush=True)


if __name__=='__main__':main()
