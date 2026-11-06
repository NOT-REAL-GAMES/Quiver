#!/usr/bin/env python3
"""Exercise real compiled-instance loading, transforms and efrag lifecycle without a GPU."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


prefix = r'''
#include "quakedef.h"
#include "radiance_alias.h"
#include "q_ctype.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
client_state_t cl;
viddef_t vid;
entity_state_t nullentitystate;
cvar_t r_enhancedmodels;
int r_trace_line_cache_counter;
static unsigned blocks, links, frees, blas_frees;
static qmodel_t alias_model;
static aliashdr_t header;
static bool missing;
static jmp_buf error_jump;
static char error_message[256];
static vec3_t r_emins,r_emaxs,last_mins,last_maxs;
static entity_t *r_addent;
static mnode_t *r_pefragtopnode;
void *Mem_Alloc(const size_t bytes) { ++blocks;void *p=calloc(1,bytes);assert(p);return p; }
void *Mem_Realloc(void *p,const size_t bytes) { p=realloc(p,bytes);assert(p);return p; }
void Mem_Free(const void *p) { if(p){++frees;free((void *)p);} }
void Host_Error(const char *format,...) {
 va_list args;va_start(args,format);vsnprintf(error_message,sizeof(error_message),format,args);va_end(args);
 longjmp(error_jump,1);
}
qmodel_t *Mod_ForName(const char *name,qboolean crash) {
 (void)crash;assert(!strcmp(name,"progs/compiled.mdl"));return missing?NULL:&alias_model;
}
static qmodel_t *Mod_LoadModel(qmodel_t *model,qboolean crash) { (void)crash;return model; }
uint32_t RCAlias_TriangleCount(const rc_alias_mesh_t *mesh) { assert(mesh==(void *)1);return 1; }
void R_FreeEntityBLAS(entity_t *entity) { (void)entity;++blas_frees; }
void R_TranslateNewPlayerSkin(int player) { (void)player; }
int32_t COM_Rand(void) { return 1; }
static void R_SplitEntityOnNode(mnode_t *node) {
 (void)node;++links;
 if(r_addent->compiled_bounds_valid) {
  assert(!memcmp(r_emins,r_addent->compiled_mins,sizeof(vec3_t)));
  assert(!memcmp(r_emaxs,r_addent->compiled_maxs,sizeof(vec3_t)));
 }
}
void R_CheckEfrags(void) {}
qboolean R_CullBox(vec3_t mins,vec3_t maxs) { memcpy(last_mins,mins,sizeof(vec3_t));memcpy(last_maxs,maxs,sizeof(vec3_t));return false; }
'''

body = function('Quake/common.c', 'int q_strcasecmp (')
body += function('Quake/common.c', 'const char *COM_FileGetExtension (')
body += function('Quake/gl_model.c', 'void *Mod_Extradata_CheckSkin (')
body += function('Quake/gl_model.c', 'void *Mod_Extradata_Exact (')
body += ''.join(function('Quake/mathlib.c', signature) for signature in (
    'void VectorMA (', 'void IdentityMatrix (', 'void MatrixMultiply (', 'void RotationMatrix (',
    'void TranslationMatrix (', 'void ScaleMatrix ('))
body += function('Quake/gl_rmain.c', 'void R_RotateForEntityScale (')
body += function('Quake/gl_rmain.c', 'void R_RotateForEntity (')
body += function('Quake/gl_rmain.c', 'qboolean R_CullModelForEntity (')
body += function('Quake/gl_refrag.c', 'void R_AddEfrags (')
body += function('Quake/gl_refrag.c', 'void R_RebuildAllEfrags (')
body += function('Quake/cl_parse.c', 'static entity_t *CL_NewStaticEntity (')
body += function('Quake/cl_parse.c', 'static void CL_LoadCompiledStaticModels (')
body += function('Quake/cl_parse.c', 'static qboolean CL_UpdateEntityModel (')

suffix = r'''
static void cleanup(void) {
 for(int i=0;i<cl.num_statics;i+=64)Mem_Free(cl.static_entities[i]);
 Mem_Free(cl.static_entities);cl.static_entities=NULL;cl.num_statics=cl.max_static_entities=0;
}
static void near(float a,float b) { assert(fabsf(a-b)<1e-4f); }
int main(void) {
 nullentitystate.colormod[0]=nullentitystate.colormod[1]=nullentitystate.colormod[2]=32;
 qmodel_t world={0};mleaf_t leafs[2]={0};world.leafs=leafs;world.numleafs=1;
 qsm_instance_t instances[65]={0};qsm_triangle_t triangles[65]={0};
 qsm_data_t data={.instances=instances,.triangles=triangles,.instance_count=65,.triangle_count=65};
 world.static_models=&data;cl.worldmodel=&world;
 alias_model.type=mod_alias;header.numframes=header.numposes=header.numskins=1;header.radiance_mesh=(void *)1;
 strcpy(alias_model.name,"progs/compiled.mdl");alias_model.extradata[PV_QUAKE1]=(byte *)&header;
 aliashdr_t replacement={0};replacement.numskins=1;replacement.numframes=2;
 alias_model.extradata[PV_MD5]=(byte *)&replacement;r_enhancedmodels.value=1;
 assert(Mod_Extradata_CheckSkin(&alias_model,0)==&replacement && Mod_Extradata_Exact(&alias_model,0)==&header);
 for(unsigned i=0;i<65;++i) {
  qsm_instance_t *instance=&instances[i];strcpy(instance->model,"progs/compiled.mdl");
  instance->origin[0]=(float)i*8;instance->origin[1]=20;instance->origin[2]=30;
  instance->angles[1]=90;instance->modelscale=1.375f;
  instance->first_triangle=i;instance->triangle_count=1;
  float matrix[16];IdentityMatrix(matrix);
  R_RotateForEntityScale(matrix,instance->origin,instance->angles,instance->modelscale);
  const float local[3][3]={{0,0,0},{2,0,0},{0,1,0}};
  for(unsigned v=0;v<3;++v)for(unsigned a=0;a<3;++a)
   triangles[i].position[v][a]=matrix[a]*local[v][0]+matrix[4+a]*local[v][1]+matrix[8+a]*local[v][2]+matrix[12+a];
 }
 CL_LoadCompiledStaticModels();
 assert(cl.num_statics==65 && cl.max_static_entities==128 && blocks==2 && links==65);
 for(unsigned i=0;i<65;++i) {
  entity_t *entity=cl.static_entities[i];assert(entity->is_static && entity->compiled_instance==&instances[i]);
  assert(entity->compiled_instance_index==i && entity->model==&alias_model && !entity->netstate.modelindex);
  assert(entity->compiled_bounds_valid && entity->modelscale_override==1.375f && R_EntityModelScale(entity)==1.375f);
  assert(entity->netstate.colormod[0]==32 && entity->netstate.colormod[1]==32 && entity->netstate.colormod[2]==32);
  near(entity->compiled_mins[0],(float)i*8-1.375f);near(entity->compiled_maxs[0],(float)i*8);
  near(entity->compiled_mins[1],20);near(entity->compiled_maxs[1],22.75f);
  assert(!CL_UpdateEntityModel(entity,0) && entity->model==&alias_model && blas_frees==0);
  assert(!R_CullModelForEntity(entity));
  assert(!memcmp(last_mins,entity->compiled_mins,sizeof(vec3_t)) && !memcmp(last_maxs,entity->compiled_maxs,sizeof(vec3_t)));
 }
 R_RebuildAllEfrags();assert(links==130 && cl.num_statics==65);
 for(unsigned enhanced=0;enhanced<2;++enhanced) {
  r_enhancedmodels.value=(float)enhanced;
  assert(Mod_Extradata_CheckSkin(&alias_model,0)==(enhanced?&replacement:&header));
  for(int i=0;i<cl.num_statics;++i)assert(Mod_Extradata_Exact(cl.static_entities[i]->model,cl.static_entities[i]->skinnum)==&header);
 }
 alias_model.extradata[PV_QUAKE1]=NULL;assert(!Mod_Extradata_Exact(&alias_model,0));alias_model.extradata[PV_QUAKE1]=(byte *)&header;
 qmodel_t explicit_model=alias_model;
 strcpy(explicit_model.name,"progs/compiled.MD3");explicit_model.extradata[PV_QUAKE3]=(byte *)&replacement;
 assert(Mod_Extradata_Exact(&explicit_model,0)==&replacement);
 strcpy(explicit_model.name,"progs/compiled.md5mesh");assert(Mod_Extradata_Exact(&explicit_model,0)==&replacement);
 assert(!Mod_Extradata_Exact(&explicit_model,-1) && !Mod_Extradata_Exact(&explicit_model,1));
 entity_t *protocol=CL_NewStaticEntity();assert(protocol->is_static && !protocol->compiled_instance && !protocol->modelscale_override);
 protocol->netstate.scale=32;assert(R_EntityModelScale(protocol)==2);
 protocol->model=&alias_model;assert(CL_UpdateEntityModel(protocol,0) && !protocol->model && blas_frees==1);
 float matrix[16],origin[3]={1,2,3},angles[3]={0};IdentityMatrix(matrix);
 R_RotateForEntity(matrix,origin,angles,32);near(matrix[0],2);near(matrix[5],2);near(matrix[12],1);
 cleanup();assert(frees==3);
 data.instance_count=1;header.numframes=2;
 if(!setjmp(error_jump)){CL_LoadCompiledStaticModels();assert(!"animated model accepted");}
 assert(strstr(error_message,"one mesh frame") && !cl.num_statics);
 header.numframes=1;missing=true;
 if(!setjmp(error_jump)){CL_LoadCompiledStaticModels();assert(!"missing model accepted");}
 assert(strstr(error_message,"missing or unsupported") && !cl.num_statics);missing=false;
 instances[0].skin=1;
 if(!setjmp(error_jump)){CL_LoadCompiledStaticModels();assert(!"changed model skins accepted");}
 assert(strstr(error_message,"missing or unsupported") && !cl.num_statics);instances[0].skin=0;
 instances[0].modelscale=NAN;
 if(!setjmp(error_jump)){CL_LoadCompiledStaticModels();assert(!"invalid scale accepted");}
 assert(strstr(error_message,"invalid instance parameters") && !cl.num_statics);
 instances[0].modelscale=1.375f;instances[0].triangle_count=0;
 if(!setjmp(error_jump)){CL_LoadCompiledStaticModels();assert(!"invalid geometry range accepted");}
 assert(strstr(error_message,"invalid bounds geometry") && !cl.num_statics);
 world.static_models=NULL;CL_LoadCompiledStaticModels();assert(!cl.num_statics);
 puts("PASS: compiled static allocation, exact float bounds, efrag rebuild, slot0 survival and malformed asset rejection");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='static-model-client-') as folder:
    folder = Path(folder)
    source, binary = folder / 'test.c', folder / 'test'
    source.write_text(prefix + body + suffix)
    flags = subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True).split()
    subprocess.run(['cc', '-std=gnu2x', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-DUSE_SDL3',
                    '-I', str(root / 'Quake'), '-I', str(root / 'Quake/mimalloc'), *flags,
                    str(source), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
