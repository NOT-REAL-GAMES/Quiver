#!/usr/bin/env python3
"""Exercise the actual world trace merge helper against real triangle sweeps."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


def extract_function(source,name):
    start=source.index('static void '+name+' (');body=source.index('{',start);depth=0
    for end in range(body,len(source)):
        depth+=(source[end]=='{')-(source[end]=='}')
        if depth==0:return source[start:end+1]
    raise AssertionError('world collision helper has unbalanced braces')


class StaticModelWorldTest(unittest.TestCase):
    def test_actual_world_helper_preserves_world_and_merges_mesh_contacts(self):
        source=(ROOT/'Quake/world.c').read_text();helper=extract_function(source,'SV_ClipToStaticModels')
        hook=source.index('SV_ClipToStaticModels (&clip.trace, start, mins, maxs, end, clip.hitcontents);')
        self.assertGreater(hook,source.index('clip.trace = SV_ClipMoveToEntity (qcvm->edicts'))
        prefix=r'''
#include "static_models.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
typedef int qboolean;
typedef float vec3_t[3];
typedef struct { int unused; } edict_t;
typedef struct { vec3_t normal; float dist; } plane_t;
typedef struct {
 qboolean allsolid,startsolid,inopen,inwater;float fraction;vec3_t endpos;
 plane_t plane;edict_t *ent;int contents;
} trace_t;
typedef struct { qsm_data_t *static_models; } model_t;
typedef struct { model_t *worldmodel;edict_t *edicts; } qcvm_t;
static qcvm_t *qcvm;
#define CONTENTS_SOLID (-2)
#define CONTENTMASK_FROMQ1(x) ((x)==CONTENTS_SOLID?1u:2u)
#define VectorCopy(a,b) memcpy((b),(a),sizeof(vec3_t))
'''
        program=r'''
static trace_t clear_trace(const float end[3]) {
 trace_t t={0};t.fraction=1;t.inopen=true;VectorCopy(end,t.endpos);return t;
}
int main(void) {
 qsm_instance_t instance={0};instance.flags=QSM_COLLISION;instance.triangle_count=1;
 qsm_triangle_t triangle={.position={{-100,-100,0},{100,-100,0},{0,100,0}}};
 qsm_data_t data={.instances=&instance,.triangles=&triangle,.instance_count=1,.triangle_count=1};
 char error[160];assert(QSM_BuildCollision(&data,error,sizeof(error)));
 model_t world={&data};edict_t worldentity={0};qcvm_t vm={&world,&worldentity};qcvm=&vm;
 float zero[3]={0},start[3]={0,0,10},end[3]={0,0,-10};trace_t t=clear_trace(end);
 SV_ClipToStaticModels(&t,start,zero,zero,end,1);
 assert(t.fraction<.5f && t.fraction>.49f && t.ent==&worldentity && t.contents==CONTENTS_SOLID);
 assert(fabsf(t.endpos[2]-.03125f)<1e-6f && t.plane.normal[2]==1 && !t.startsolid);
 t=clear_trace(end);t.fraction=.2f;t.endpos[2]=6;t.plane.normal[0]=1;t.ent=&worldentity;
 SV_ClipToStaticModels(&t,start,zero,zero,end,1);
 assert(t.fraction==.2f && t.endpos[2]==6 && t.plane.normal[0]==1);
 t=clear_trace(end);t.allsolid=true;t.startsolid=true;t.fraction=0;VectorCopy(start,t.endpos);
 SV_ClipToStaticModels(&t,start,zero,zero,end,1);assert(t.allsolid && t.fraction==0 && t.endpos[2]==10);
 t=clear_trace(end);SV_ClipToStaticModels(&t,start,zero,zero,end,2);assert(t.fraction==1 && !t.ent);
 vm.worldmodel=NULL;t=clear_trace(end);SV_ClipToStaticModels(&t,start,zero,zero,end,1);assert(t.fraction==1);
 vm.worldmodel=&world;
 float inside[3]={0,0,0},away[3]={0,0,10},mins[3]={-1,-1,-1},maxs[3]={1,1,1};
 t=clear_trace(away);SV_ClipToStaticModels(&t,inside,mins,maxs,away,1);
 assert(t.startsolid && !t.allsolid && t.fraction==1 && t.endpos[2]==10);
 assert(t.ent==&worldentity && t.contents==CONTENTS_SOLID);
 t=clear_trace(inside);SV_ClipToStaticModels(&t,inside,mins,maxs,inside,1);
 assert(t.startsolid && t.allsolid && t.fraction==0 && t.ent==&worldentity);
 QSM_FreeCollision(data.collision);return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);driver=root/'world-merge.c';driver.write_text(prefix+helper+program)
            command=shlex.split(os.environ.get('CC','cc'))+['-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'Quake')]
            if os.environ.get('STATIC_MODELS_SANITIZE'):
                command+=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
            command += [str(driver),str(ROOT/'Quake/static_models_collision.c'),str(ROOT/'Quake/radiance_scene.c'),'-lm','-o',str(root/'world-merge')]
            subprocess.run(command,check=True,capture_output=True)
            subprocess.run([str(root/'world-merge')],check=True,capture_output=True,timeout=30)


if __name__=='__main__':unittest.main()
