#!/usr/bin/env python3
"""Private real-inspector RGB edits and textured source-model preview acceptance.

Opens only its own SDL process; files, input and shaders are isolated. Requires
Quake PAKs, the Sponza MD5 asset and an SDL/Vulkan display. No timing claims.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

import numpy as np
from PIL import Image
import bench_meshlets as bench
from bench_radiance import ensure_no_game
from bench_radiance_soft_shadows import short_binary_alias

ROOT=Path(__file__).resolve().parents[1]
SHIM=r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef USE_SDL3
#include <SDL3/SDL.h>
typedef bool poll_result;
#define KD SDL_EVENT_KEY_DOWN
#define KU SDL_EVENT_KEY_UP
#define MD SDL_EVENT_MOUSE_BUTTON_DOWN
#define MU SDL_EVENT_MOUSE_BUTTON_UP
#define MM SDL_EVENT_MOUSE_MOTION
#define MW SDL_EVENT_MOUSE_WHEEL
#define TI SDL_EVENT_TEXT_INPUT
#else
#include <SDL2/SDL.h>
typedef int poll_result;
#define KD SDL_KEYDOWN
#define KU SDL_KEYUP
#define MD SDL_MOUSEBUTTONDOWN
#define MU SDL_MOUSEBUTTONUP
#define MM SDL_MOUSEMOTION
#define MW SDL_MOUSEWHEEL
#define TI SDL_TEXTINPUT
#endif
struct __attribute__((packed)) packet {char kind;int a,b,c;char text[32];};
static int mx=640,my=368;
#ifdef USE_SDL3
SDL_MouseButtonFlags SDL_GetMouseState(float *x,float *y) {
#else
Uint32 SDL_GetMouseState(int *x,int *y) {
#endif
 if(x)*x=mx;if(y)*y=my;return 0;
}
poll_result SDL_PollEvent(SDL_Event *e) {
 static poll_result (*real_poll)(SDL_Event *);static int fd=-1;static struct packet p;
 if(!real_poll){real_poll=dlsym(RTLD_NEXT,"SDL_PollEvent");fd=atoi(getenv("BENCH_KEY_FD"));}
 if(e&&read(fd,&p,sizeof p)==sizeof p){
  SDL_zero(*e);
  if(p.kind=='k'){e->type=p.b?KD:KU;
#ifdef USE_SDL3
   e->key.scancode=p.a;e->key.key=SDL_GetKeyFromScancode(p.a,0,0);e->key.down=p.b;
#else
   e->key.keysym.scancode=p.a;e->key.keysym.sym=SDL_GetKeyFromScancode(p.a);e->key.state=p.b?SDL_PRESSED:SDL_RELEASED;
#endif
  }else if(p.kind=='m'){mx=p.a;my=p.b;e->type=MM;e->motion.x=mx;e->motion.y=my;
  }else if(p.kind=='b'){e->type=p.b?MD:MU;e->button.button=p.a;e->button.x=mx;e->button.y=my;
#ifdef USE_SDL3
   e->button.down=p.b;
#else
   e->button.state=p.b?SDL_PRESSED:SDL_RELEASED;
#endif
  }else if(p.kind=='w'){e->type=MW;e->wheel.y=p.a;
  }else if(p.kind=='t'){e->type=TI;
#ifdef USE_SDL3
   e->text.text=p.text;
#else
   memcpy(e->text.text,p.text,sizeof p.text);
#endif
  }else return 0;
  return 1;
 }
 while(real_poll(e)){
  /* Real user input cannot mutate this disposable test's source document. */
  if(e->type==KD||e->type==KU||e->type==MD||e->type==MU||e->type==MM||e->type==MW||e->type==TI)continue;
  return 1;
 }
 return 0;
}
'''
FIXTURE=r'''
#include "editor_map.h"
#include "editor_sector.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void save(const char *path,const char *text,size_t length){FILE*f=fopen(path,"wb");assert(f);assert(fwrite(text,1,length,f)==length);assert(!fclose(f));}
int main(int argc,char **argv){
 assert(argc==4);editor_sector_map_t s={0};editor_map_t m={0};char error[256]={0};
 const double lo[3]={-1024,-1024,-128},hi[3]={1024,1024,1024};size_t i;
 assert(EditorSector_Init(&s));assert(EditorSector_AddBox(&s,lo,hi,"stone1",NULL,error,sizeof error));
 assert(EditorMap_SetProperty(&s.entities.entities[0],"wad",argv[3]));
 assert(EditorMap_AddEntity(&s.entities,"info_player_start",&i));assert(EditorMap_SetProperty(&s.entities.entities[i],"origin","-600 0 128"));
 const double light[3]={-600,0,256};assert(EditorMap_AddLight(&s.entities,light,&i));
 assert(EditorMap_SetProperty(&s.entities.entities[i],"_color","1 0 0"));
 assert(EditorMap_SetProperty(&s.entities.entities[i],"color","255 255 255"));
 assert(EditorMap_SetProperty(&s.entities.entities[i],"light","800"));
 const double origin[3]={0,0,64};assert(EditorMap_AddModel(&s.entities,"progs/sponza.md5mesh",origin,&i));
 editor_map_model_t model;assert(EditorMap_GetModel(&s.entities,i,&model));model.modelscale=3.5;model.angles[1]=37;assert(EditorMap_SetModel(&s.entities,i,&model));
 char *text=NULL;size_t length;assert(EditorSector_Serialize(&s,&text,&length));save(argv[2],text,length);free(text);
 assert(EditorSector_Compile(&s,&m,error,sizeof error));assert(EditorMap_Serialize(&m,&text,&length));save(argv[1],text,length);free(text);
 EditorMap_Free(&m);EditorSector_Free(&s);return 0;
}
'''


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def saved_color(path,expected):
    text=path.read_text();pairs=re.findall(r'"([^"\n]+)"\s*"([^"\n]*)"',text)
    values=dict(pairs)
    if 'color' in values:raise RuntimeError('legacy color alias survived explicit inspector edit')
    actual=tuple(map(float,values.get('_color','').split()))
    if len(actual)!=3 or not np.allclose(actual,np.asarray(expected)/255,rtol=0,atol=1e-12):
        raise RuntimeError(f'wrong saved RGB in {path}: {actual}, expected bytes{expected}')
    if values.get('light')!='800':raise RuntimeError('editing RGB changed authored intensity')
    return dict(rgb_normalized=actual,bytes=expected,path=str(path),sha256=sha(path),format='QSECTOR' if text.startswith('QSECTOR') else 'MAP')


def compiled_color(bsp,expected):
    data=bsp.read_bytes();off,size=struct.unpack_from('<ii',data,4)
    entities=data[off:off+size].rstrip(b'\0').decode()
    pairs=dict(re.findall(r'"([^"\n]+)"\s*"([^"\n]*)"',entities))
    got=tuple(map(float,pairs['_color'].split()))
    if not np.allclose(got,np.asarray(expected)/255,rtol=0,atol=1e-6) or 'color' in pairs:
        raise RuntimeError('compiled light entity differs from saved RGB')
    lit=bsp.with_suffix('.lit');raw=lit.read_bytes()
    if raw[:8]!=b'QLIT\x01\0\0\0' or not (len(raw)-8)%3==0:raise RuntimeError('bad RGB .lit')
    rgb=np.frombuffer(raw[8:],dtype=np.uint8).reshape(-1,3)
    # A single edited blue light must yield colored samples, not white/default.
    colored=(rgb[:,2]>rgb[:,1])&(rgb[:,1]>rgb[:,0])
    if colored.sum()<16:raise RuntimeError('compiled LIGHT did not use the blue inspector color')
    return dict(bsp=str(bsp),bsp_sha256=sha(bsp),entities=entities,lit=str(lit),lit_sha256=sha(lit),
                light_rgb=got,colored_samples=int(colored.sum()),total_samples=len(rgb),max_rgb=rgb.max(axis=0).tolist())


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--basedir',type=Path,required=True)
    p.add_argument('--assets',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--expected-sha256');p.add_argument('--validation',action='store_true');p.add_argument('--timeout',type=float,default=60)
    p.add_argument('--cases',nargs='+',choices=('color_map','color_sector'),default=('color_map','color_sector'))
    args=p.parse_args();args.binary=args.binary.resolve();args.game='color_mesh';args.width=1280;args.height=720
    if args.expected_sha256 and sha(args.binary)!=args.expected_sha256:raise RuntimeError('candidate hash mismatch')
    ensure_no_game(False)
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False);work=output/'work'
    (work/'id1').mkdir(parents=True);game=work/args.game;(game/'maps').mkdir(parents=True);(work/'user').mkdir()
    source=args.basedir.resolve();source=source/'id1' if (source/'id1').is_dir() else source
    paks=sorted(source.glob('pak*.pak'))
    if not paks:raise RuntimeError('missing PAKs')
    for pak in paks:(work/'id1'/pak.name).symlink_to(pak)
    for name in ('progs','textures'):
        if (args.assets/name).is_dir():(game/name).symlink_to((args.assets/name).resolve(),target_is_directory=True)
    wad=work/'editor_defaults.wad';wad.symlink_to(ROOT/'Misc/editor_defaults.wad')
    builder=output/'fixture.c';builder.write_text(FIXTURE)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'Quake'),str(builder),
                    str(ROOT/'Quake/editor_map.c'),str(ROOT/'Quake/editor_sector.c'),'-lm','-o',str(output/'fixture')],check=True,timeout=30)
    subprocess.run([str(output/'fixture'),str(game/'maps/color_map.map'),str(game/'maps/color_sector.map'),str(wad)],check=True,timeout=10)
    boot=bench.BOOT.replace('host_maxfps 0','host_maxfps 72').replace('map start','r_radiance 0\nr_radiance_soft_shadows 0\nmap start')
    (game/'boot.cfg').write_text(boot);bench.SHIM=SHIM
    report=dict(complete=False,binary=str(args.binary),binary_sha256=sha(args.binary),source_harness_sha256=sha(Path(__file__)),checks=[],cases=[],captures=[])
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    original=args.binary;alias,args.binary=short_binary_alias(original)
    engine=bench.Engine(args,work)
    def packet(kind,a=0,b=0,c=0,text=''):
        os.write(engine.key,struct.pack('<ciii32s',kind.encode(),a,b,c,text.encode()));time.sleep(.035)
    def key(code):packet('k',code,1);packet('k',code,0)
    def chord(code):packet('k',224,1);key(code);packet('k',224,0)
    def mouse(x,y,down=None):
        packet('m',x,y);time.sleep(.07)
        if down is not None:packet('b',1,int(down))
    def click(x,y):mouse(x,y,True);packet('b',1,0)
    def command(body,editor=False,close=True):
        engine.serial+=1;marker=f'COLOR_MESH_{engine.serial}_DONE';start=len(engine.text())
        # Disconnected toggleconsole chooses the main menu. The editor owns
        # the proper key destination; restore it explicitly after commands.
        (game/'control.tmp').write_text(body+'\necho '+marker+'\n'+('editor_return\n' if editor and close else ''))
        (game/'control.tmp').replace(game/'control.cfg')
        if editor:
            packet('k',225,1);key(41);packet('k',225,0)
        key(69)
        return engine.wait(lambda text:text if marker in text else None,start)
    def photo(label,editor=True):
        start=len(engine.text());command(('editor_return\n' if editor else '')+'wait\nwait\nscreenshot png',editor,False)
        match=engine.wait(lambda text:re.search(r'Wrote [\x01\x02]?([^\n]+\.png)',text),start)
        source=next(work.rglob(match[1]),None)
        if source is None:raise RuntimeError('capture missing')
        target=output/(label+'.png');shutil.copy2(source,target)
        if Image.open(target).size!=(1280,720):raise RuntimeError('actual viewport mismatch')
        report['captures'].append(str(target));return target
    def save(name,expected):
        command('editor_save '+name,True);path=game/'maps'/f'{name}.map'
        return saved_color(path,expected)
    def edit(channel,value):
        click(1200,112+26*channel);key(74)
        for _ in range(16):key(76)
        packet('t',text=str(value));key(40)
    try:
        engine.wait(lambda text:'CL_SignonReply: 4' in text)
        for name in args.cases:
            ensure_no_game(False,engine.process.pid)
            opened=command('disconnect\neditor_open '+name,editor=bool(report['cases']))
            if not re.search(r'(?:Source|Sector) map opened',opened):raise RuntimeError('source open failed')
            key(30);click(640,368)
            photo(name+'-selected-light')
            for channel,value in enumerate((32,96,224)):edit(channel,value)
            row=dict(name=name,saved=save(name,(32,96,224)))
            photo(name+'-rgb-inspector')
            # Click the selected point to release retained textbox focus first.
            click(640,368);chord(29)
            row['undo']=save(name,(32,96,255));click(640,368);chord(28)
            row['redo']=save(name,(32,96,224))
            command('editor_open '+name,True);key(30);click(640,368)
            row['reload']=save(name,(32,96,224));photo(name+'-reload-inspector')
            # Zoom about the selected light so the entire architecture fits.
            mouse(640,368)
            for _ in range(6):packet('w',-1)
            photo(name+'-model-ortho')
            # Real point drag translates the textured instance, then undo
            # restores its authored noninteger scale/yaw fixture exactly.
            mouse(797,368,True);mouse(823,368);packet('b',1,0)
            dragged=save(name,(32,96,224));origin=re.findall(r'"origin"\s*"([^"]+)"',(game/'maps'/f'{name}.map').read_text())[-1]
            if tuple(map(float,origin.split()))!=(96,0,64):raise RuntimeError('model drag did not commit its snapped transform: '+origin)
            row['model_drag']=dict(origin=origin,saved=dragged);photo(name+'-model-dragged')
            click(823,368);chord(29);row['model_drag_undo']=save(name,(32,96,224))
            restored=re.findall(r'"origin"\s*"([^"]+)"',(game/'maps'/f'{name}.map').read_text())[-1]
            if tuple(map(float,restored.split()))!=(0,0,64):raise RuntimeError('undo did not restore model origin')
            click(640,368)
            key(33)
            # Enter the courtyard through the source mesh. Editor movement
            # is intentionally free-flying; screenshots are functional only.
            packet('k',26,1);time.sleep(1.8);packet('k',26,0)
            photo(name+'-model-perspective')
            command('r_enhancedmodels 0\nwait\nwait',True);photo(name+'-model-exact-enhanced-off')
            command('r_enhancedmodels 1\nvid_restart\nwait\nwait',True);photo(name+'-model-restart')
            prior=set((game/'maps').glob('__quiver_editor_*.bsp'));start=len(engine.text());key(66)
            engine.wait(lambda text:'Starting playtest' in text,start);engine.wait(lambda text:'CL_SignonReply: 4' in text,start)
            log=engine.text()[start:]
            if not all(x in log for x in ('Calculating visibility','Calculating lighting')):raise RuntimeError('F9 skipped compiler stages')
            new=set((game/'maps').glob('__quiver_editor_*.bsp'))-prior
            if len(new)!=1:raise RuntimeError('wrong compiled BSP publication count')
            row['compiled']=compiled_color(new.pop(),(32,96,224));photo(name+'-playtest',False)
            row['model_stats']=command('static_models_stats')
            if not re.search(r'StaticModels: instances=1 triangles=262266 collision=1 client_compiled=1',row['model_stats']):raise RuntimeError('model compile lost geometry')
            command('editor_return');photo(name+'-return')
            report['cases'].append(row);(output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        # quit cannot echo a completion marker after process teardown.
        (game/'control.cfg').write_text('quit\n')
        packet('k',225,1);key(41);packet('k',225,0);key(69)
        engine.process.wait(timeout=20)
        if engine.process.returncode!=0:raise RuntimeError('unclean exit')
        report['complete']=True
    except BaseException as error:
        report['failure']=str(error);raise
    finally:
        if engine.process.poll() is None:
            engine.process.terminate()
            try:engine.process.wait(timeout=15)
            except subprocess.TimeoutExpired:engine.process.kill();engine.process.wait(timeout=5)
        # Engine.close must not inject its one-byte stock packets into this shim.
        engine.close();alias.cleanup()
        report['exit_code']=engine.process.returncode;report['binary_sha256_after']=sha(original)
        report['hash_unchanged']=report['binary_sha256']==report['binary_sha256_after']
        logs=(work/'output.log').read_text(errors='replace')+'\n'+engine.text()
        report['errors']=re.findall(r'VUID-[^\n]+|SYNC-HAZARD[^\n]+|Validation Error[^\n]+|NVVM compilation failed[^\n]+|Host_Error[^\n]+|Sys_Error[^\n]+',logs)
        report['validation_observed']=bool(re.search(r'Using.*validation|VALIDATION-SETTINGS',logs,re.I))
        if report['errors'] or args.validation and not report['validation_observed']:report['complete']=False
        (output/'console.log').write_text(engine.text());shutil.copy2(work/'output.log',output/'output.log')
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['complete']:raise RuntimeError('acceptance error or validation layer missing')
    print(json.dumps(dict(complete=True,output=str(output),cases=len(report['cases']),captures=len(report['captures']),errors=report['errors'])),flush=True)

if __name__=='__main__':main()
