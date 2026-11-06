#!/usr/bin/env python3
"""Exercise the production static-model compiler adapter without a renderer."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


PREFIX = r'''
#include "quakedef.h"
#include "q_ctype.h"
#include "editor_map.h"
#include "static_models_engine.h"
#include "radiance_alias.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
static qmodel_t models[3];
static aliashdr_t headers[3][2], replacements[3];
static unsigned exact_loads;
static const char *paths[]={"progs/native.mdl","progs/native.md3","progs/native.md5mesh"};
static const poseverttype_t formats[]={PV_QUAKE1,PV_QUAKE3,PV_MD5};
cvar_t r_enhancedmodels;
qmodel_t *Mod_ForName(const char *path,qboolean crash) {
 (void)crash;
 for(unsigned i=0;i<3;++i)if(!strcmp(path,paths[i]))return &models[i];
 return NULL;
}
static qmodel_t *Mod_LoadModel(qmodel_t *model,qboolean crash) {
 (void)crash;++exact_loads;return model;
}
/* Any accidental fallback to the ordinary replacement selector is a failure. */
void *Mod_Extradata_CheckSkin(qmodel_t *model,int skin) {
 (void)model;(void)skin;assert(!"compiler selected an enhanced replacement");return NULL;
}
void QSM_FreeCollision(qsm_collision_t *collision) { assert(!collision); }
'''

BODY = function('Quake/common.c', 'int q_strcasecmp (')
BODY += function('Quake/common.c', 'const char *COM_FileGetExtension (')
BODY += function('Quake/gl_model.c', 'void *Mod_Extradata_Exact (')
BODY += ''.join(function('Quake/mathlib.c', signature) for signature in (
    'void IdentityMatrix (', 'void MatrixMultiply (', 'void RotationMatrix (',
    'void TranslationMatrix (', 'void ScaleMatrix ('))
BODY += function('Quake/gl_rmain.c', 'void R_RotateForEntityScale (')
BODY += function('Quake/static_models_engine.c', 'static bool QSM_EngineError (')
BODY += function('Quake/static_models_engine.c', 'bool EditorStaticModels_Prepare (')

SUFFIX = r'''
static void setup(void) {
 const uint16_t indices[3]={0,1,2};const float uv[3][2]={{0,0},{1,0},{0,1}};
 for(unsigned model=0;model<3;++model) {
  qmodel_t *m=&models[model];strcpy(m->name,paths[model]);m->type=mod_alias;
  m->extradata[formats[model]]=(byte *)&headers[model][0];
  /* A .mdl may have an animated replacement loaded simultaneously. */
  if(!model)m->extradata[PV_MD5]=(byte *)&replacements[model];
  replacements[model].numframes=999;replacements[model].numskins=2;
  for(unsigned surface=0;surface<2;++surface) {
   aliashdr_t *h=&headers[model][surface];
   h->numframes=h->numposes=1;h->numskins=2;h->numindexes=3;h->poseverttype=formats[model];
   if(!surface)h->nextsurface=&headers[model][1];
   float offset=(float)(model*16+surface*4);
   if(model<2) {
    uint16_t xyz[3][3]={{0,0,0},{65535,0,0},{0,65535,65535}};
    float scale[3]={2.f/255,4.f/255,6.f/255},origin[3]={offset+.25f,-.125f,3};
    if(model) {
     const uint16_t md3[3][3]={{32768,32768,32768},{32770,32768,32768},{32768,32772,32774}};
     memcpy(xyz,md3,sizeof(xyz));scale[0]=scale[1]=scale[2]=1;
    }
    h->radiance_mesh=RCAlias_Create(model?RC_ALIAS_MD3:RC_ALIAS_MDL,3,1,3,
     xyz,sizeof(xyz[0]),uv,sizeof(uv[0]),indices,scale,origin);
   } else {
    md5vert_t xyz[3]={0};
    for(unsigned v=0;v<3;++v) {
     xyz[v].xyz[0]=NAN;xyz[v].joint_weights[0]=255;
     xyz[v].joint_position_x[0]=offset+.25f+(v==1?2:0);
     xyz[v].joint_position_y[0]=-.125f+(v==2?4:0);
     xyz[v].joint_position_z[0]=3+(v==2?6:0);
     xyz[v].st[0]=uv[v][0];xyz[v].st[1]=uv[v][1];
    }
    const float joint[12]={1,0,0,0,0,1,0,0,0,0,1,0};
    h->radiance_mesh=RCAlias_CreateSkinnedStatic(3,3,xyz,sizeof(xyz[0]),4,1,joint,indices);
   }
   assert(h->radiance_mesh);
  }
 }
}

static size_t add_model(editor_map_t *map,unsigned model,unsigned placement,bool collision) {
 size_t index;double origin[3]={10.25+(double)placement*31,-20.5,30.125};
 assert(EditorMap_AddModel(map,paths[model],origin,&index));
 editor_map_model_t value;assert(EditorMap_GetModel(map,index,&value));
 value.modelscale=1.375+(double)placement*.125;value.skin=1;value.collision=collision;
 value.angles[0]=20;value.angles[1]=90;value.angles[2]=-30;
 assert(EditorMap_SetModel(map,index,&value));return index;
}

/* Independent double oracle for Rz(yaw)*Ry(-pitch)*Rx(roll). */
static void transformed(const qsm_instance_t *instance,const float local[3],double out[3]) {
 const double radians=3.14159265358979323846/180;
 double pitch=instance->angles[0]*radians,yaw=instance->angles[1]*radians,roll=instance->angles[2]*radians;
 double x=local[0]*instance->modelscale,y=local[1]*instance->modelscale,z=local[2]*instance->modelscale;
 double ry=cos(roll)*y-sin(roll)*z,rz=sin(roll)*y+cos(roll)*z;
 double px=cos(pitch)*x-sin(pitch)*rz,pz=sin(pitch)*x+cos(pitch)*rz;
 out[0]=instance->origin[0]+cos(yaw)*px-sin(yaw)*ry;
 out[1]=instance->origin[1]+sin(yaw)*px+cos(yaw)*ry;
 out[2]=instance->origin[2]+pz;
}

static char *serialized(const editor_map_t *map,size_t *size) {
 char *text=NULL;assert(EditorMap_Serialize(map,&text,size));return text;
}

static void success(void) {
 editor_map_t source={0},snapshot={0};assert(EditorMap_Init(&source));
 size_t light;assert(EditorMap_AddEntity(&source,"light",&light));
 assert(EditorMap_SetProperty(&source.entities[light],"targetname","preserved"));
 for(unsigned i=0;i<3;++i)add_model(&source,i,i,i!=1);
 assert(EditorMap_Clone(&snapshot,&source));
 size_t source_bytes;char *source_text=serialized(&source,&source_bytes);
 uint8_t *bytes=NULL;size_t length=0;char error[256];
 r_enhancedmodels.value=1;unsigned before_loads=exact_loads;
 assert(EditorStaticModels_Prepare(&snapshot,&bytes,&length,error,sizeof(error)));
 assert(exact_loads==before_loads+3 && snapshot.num_entities==2 && source.num_entities==5);
 assert(!strcmp(EditorMap_GetProperty(&snapshot.entities[1],"targetname"),"preserved"));
 for(size_t i=1;i<snapshot.num_entities;++i)assert(!EditorMap_IsModel(&snapshot,i));
 size_t after_bytes;char *after=serialized(&source,&after_bytes);
 assert(source_bytes==after_bytes && !memcmp(source_text,after,source_bytes));free(after);free(source_text);
 qsm_data_t decoded={0};assert(QSM_Decode(&decoded,bytes,length,error,sizeof(error)));
 assert(decoded.instance_count==3 && decoded.triangle_count==6);
 for(unsigned i=0;i<3;++i) {
  const qsm_instance_t *instance=&decoded.instances[i];
  assert(!strcmp(instance->model,paths[i]) && instance->skin==1 && instance->modelscale==1.375f+i*.125f);
  assert(instance->first_triangle==i*2 && instance->triangle_count==2);
  assert(instance->flags==(i==1?0:QSM_COLLISION)); /* collisionOFF still has full bounds geometry. */
  for(unsigned s=0;s<2;++s)for(unsigned v=0;v<3;++v) {
   float local[3]={(float)(i*16+s*4)+.25f+(v==1?2:0),-.125f+(v==2?4:0),3+(v==2?6:0)};
   double expected[3];transformed(instance,local,expected);
   const float *actual=decoded.triangles[i*2+s].position[v];
   for(unsigned a=0;a<3;++a)assert(fabs(actual[a]-expected[a])<3e-5);
  }
 }
 /* The global enhanced setting cannot alter encoded placement geometry. */
 editor_map_t classic={0};assert(EditorMap_Clone(&classic,&source));r_enhancedmodels.value=0;
 uint8_t *classic_bytes=NULL;size_t classic_length=0;
 assert(EditorStaticModels_Prepare(&classic,&classic_bytes,&classic_length,error,sizeof(error)));
 assert(classic_length==length && !memcmp(classic_bytes,bytes,length));
 QSM_Free(&decoded);free(bytes);free(classic_bytes);
 EditorMap_Free(&classic);EditorMap_Free(&snapshot);EditorMap_Free(&source);
}

static void failure(unsigned mutation) {
 editor_map_t map={0};assert(EditorMap_Init(&map));
 add_model(&map,0,0,true);size_t bad=add_model(&map,1,1,false);
 editor_map_model_t value;assert(EditorMap_GetModel(&map,bad,&value));
 aliashdr_t saved=headers[1][1];byte *saved_format=models[1].extradata[PV_QUAKE3];
 switch(mutation) {
  case 0:strcpy(value.model,"progs/missing.md3");break;
  case 1:value.skin=2;break;
  case 2:headers[1][1].numframes=2;break;
  case 3:headers[1][1].numposes=2;break;
  case 4:headers[1][1].radiance_mesh=NULL;break;
  case 5:models[1].extradata[PV_QUAKE3]=NULL;models[1].extradata[PV_QUAKE1]=(byte *)&headers[0][0];break;
  case 6:value.modelscale=2000;break; /* Fails final format validation after extraction. */
  case 7:headers[1][1].numindexes=4;break;
 }
 assert(EditorMap_SetModel(&map,bad,&value));
 size_t original_length;char *original=serialized(&map,&original_length);
 uint8_t sentinel=0xa5,*output=&sentinel;size_t output_length=971;char error[256]={0};
 uint64_t retained=RCAlias_TotalBytes();
 assert(!EditorStaticModels_Prepare(&map,&output,&output_length,error,sizeof(error)));
 assert(output==&sentinel && output_length==971 && sentinel==0xa5 && error[0]);
 assert(RCAlias_TotalBytes()==retained && map.num_entities==3 && EditorMap_IsModel(&map,1) && EditorMap_IsModel(&map,2));
 size_t after_length;char *after=serialized(&map,&after_length);
 assert(original_length==after_length && !memcmp(original,after,original_length));
 free(original);free(after);EditorMap_Free(&map);
 headers[1][1]=saved;models[1].extradata[PV_QUAKE3]=saved_format;models[1].extradata[PV_QUAKE1]=NULL;
}

int main(void) {
 setup();success();for(unsigned i=0;i<8;++i)failure(i);
 editor_map_t empty={0};assert(EditorMap_Init(&empty));
 uint8_t sentinel,*bytes=&sentinel;size_t length=1;char error[128];
 assert(EditorStaticModels_Prepare(&empty,&bytes,&length,error,sizeof(error)) && !bytes && !length && empty.num_entities==1);
 EditorMap_Free(&empty);
 for(unsigned m=0;m<3;++m)for(unsigned s=0;s<2;++s)RCAlias_Destroy(headers[m][s].radiance_mesh);
 assert(!RCAlias_TotalBytes());
 puts("PASS: compiler exact formats, float world triangles, snapshot-only removal and transactional failures");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='static-model-compiler-') as directory:
    directory = Path(directory)
    source, binary = directory / 'test.c', directory / 'test'
    source.write_text(PREFIX + BODY + SUFFIX)
    flags = subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True).split()
    extra = shlex.split(os.environ.get('STATIC_MODELS_TEST_CFLAGS', ''))
    command = [os.environ.get('CC', 'cc'), '-std=gnu2x', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-DUSE_SDL3', '-I', str(ROOT / 'Quake'), '-I', str(ROOT / 'Quake/mimalloc'),
               *flags, *extra, str(source)]
    command += [str(ROOT / 'Quake' / name) for name in (
        'editor_map.c', 'radiance_alias.c', 'radiance_scene.c', 'static_models.c', 'strlcpy.c')]
    subprocess.run([*command, '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
