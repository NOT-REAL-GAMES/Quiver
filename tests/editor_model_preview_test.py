#!/usr/bin/env python3
"""CPU regression for editor projection, exact assets, reloads and draw state."""
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
#include "editor_model_preview.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
client_state_t cl;
refdef_t r_refdef;
vec3_t vright,vup,vpn;
cvar_t cl_gun_fovscale,cl_gun_x,cl_gun_y,cl_gun_z,gl_fullbrights;
static qmodel_t model;
static aliashdr_t *native_header,*replacement_header;
static gltexture_t base,other_phase,fullbright;
gltexture_t *nulltexture=&base;
static unsigned loads,draws,scissors,restores,canvases;
static qboolean drawing,missing,pipelines=true;
static VkRect2D last_scissor;
static float last_matrix[16],last_projection[16];
static aliashdr_t *last_header;
static qboolean last_cutout;
qmodel_t *Mod_ForName(const char *path,qboolean crash) {
 (void)crash;assert(!drawing);++loads;
 return !missing && !strcmp(path,model.name)?&model:NULL;
}
static qmodel_t *Mod_LoadModel(qmodel_t *input,qboolean crash) {
 (void)crash;assert(!drawing);++loads;input->needload=false;return input;
}
qboolean R_HasGraphicsPipeline(const cb_context_t *cbx,graphics_pipeline_t pipeline) {
 assert(cbx->subpass_type==SUBPASS_UI && pipeline>=PIPELINE_EDITOR_ALIAS && pipeline<=PIPELINE_EDITOR_MD5_8_ALPHATEST);
 return pipelines;
}
void R_BindGraphicsPipeline(cb_context_t *cbx,graphics_pipeline_t pipeline) {
 (void)cbx;assert(pipeline==PIPELINE_BASIC_BLEND);++restores;
}
void GL_SetCanvas(cb_context_t *cbx,canvastype canvas) {
 assert(cbx->current_canvas==CANVAS_NONE && canvas==CANVAS_DEFAULT);cbx->current_canvas=canvas;++canvases;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkRect2D *rect) {
 (void)cb;assert(!first && count==1);last_scissor=*rect;++scissors;
}
static void GL_DrawAliasFrame(cb_context_t *cbx,entity_t *entity,aliashdr_t *header,lerpdata_t pose,
 gltexture_t *texture,gltexture_t *emission,float matrix[16],float alpha,qboolean cutout,
 vec3_t shade,vec3_t color,int showtris,const float *projection) {
 assert(drawing && cbx->subpass_type==SUBPASS_UI && entity->model==&model && entity->skinnum==1);
 assert(!pose.pose1 && !pose.pose2 && !pose.blend && alpha==1 && !showtris);
 assert(texture==&base && emission==(gl_fullbrights.value?&fullbright:NULL));
 assert(color[0]>0 && color[1]==color[0] && color[2]==color[0]);
 for(unsigned a=0;a<3;++a)assert(isfinite(shade[a]));
 assert(projection);memcpy(last_matrix,matrix,sizeof(last_matrix));memcpy(last_projection,projection,sizeof(last_projection));
 last_header=header;last_cutout=cutout;++draws;
}
'''

BODY = function('Quake/common.c', 'int q_strcasecmp (')
BODY += function('Quake/common.c', 'const char *COM_FileGetExtension (')
BODY += function('Quake/gl_model.c', 'void *Mod_Extradata_Exact (')
BODY += ''.join(function('Quake/mathlib.c', signature) for signature in (
    'void VectorMA (', 'void IdentityMatrix (', 'void MatrixMultiply (', 'void RotationMatrix (',
    'void TranslationMatrix (', 'void ScaleMatrix ('))
BODY += function('Quake/gl_rmain.c', 'void R_RotateForEntityScale (')
BODY += function('Quake/r_alias.c', 'static void R_AliasModelMatrix (')
BODY += function('Quake/r_alias.c', 'qboolean R_DrawEditorAlias (')

SUFFIX = r'''
static void near(float a,float b) {
 if(fabsf(a-b)>=1e-3f)fprintf(stderr,"near mismatch: %.9g vs %.9g\n",a,b);
 assert(fabsf(a-b)<1e-3f);
}
static void clip(const editor_model_view_t *view,const float point[3],float out[4]) {
 for(unsigned a=0;a<4;++a)out[a]=view->projection[a]*point[0]+view->projection[4+a]*point[1]+
  view->projection[8+a]*point[2]+view->projection[12+a];
}
static void projection(void) {
 editor_model_view_t view;const vec3_t camera={11,23,37},forward={1,0,0},right={0,-1,0},up={0,0,1};
 assert(EditorModelPreview_Perspective(&view,641,361,48,329,camera,forward,right,up,182.65f));
 for(unsigned i=0;i<20;++i) {
  const float z=1+(float)i*16,rx=(float)i*.7f-4,uy=(float)i*.3f-2;
  float point[3]={camera[0]+z,camera[1]-rx,camera[2]+uy},out[4];clip(&view,point,out);
  near(out[3],z);near(out[2]/out[3],1/z);
  near((out[0]/out[3]+1)*641*.5f,641*.5f+rx*182.65f/z);
  near((out[1]/out[3]+1)*361*.5f,(48+329)*.5f-uy*182.65f/z);
 }
 float before_near[3]={camera[0]+.5f,camera[1],camera[2]},out[4];clip(&view,before_near,out);assert(out[2]>out[3]);
 float behind[3]={camera[0]-2,camera[1],camera[2]};clip(&view,behind,out);assert(out[3]<0);
 const int axes[3][3]={{0,1,2},{0,2,1},{1,2,0}};
 for(unsigned i=0;i<3;++i) {
  int h=axes[i][0],v=axes[i][1],depth=axes[i][2];
  assert(EditorModelPreview_Ortho(&view,641,361,48,329,camera,h,v,depth,1.375f));
  float point[3];memcpy(point,camera,sizeof(point));point[h]+=31;point[v]-=17;point[depth]=64;
  clip(&view,point,out);near(out[3],1);
  near((out[0]+1)*641*.5f,641*.5f+31*1.375f);
  near((out[1]+1)*361*.5f,(48+329)*.5f+17*1.375f);
  float old_depth=out[2];point[depth]+=100;clip(&view,point,out);assert(out[2]>old_depth);
 }
 assert(!EditorModelPreview_Ortho(&view,641,361,48,329,camera,0,0,2,1));
 assert(!EditorModelPreview_Ortho(&view,641,361,48,329,camera,0,1,2,0));
 assert(!EditorModelPreview_Perspective(&view,641,361,329,48,camera,forward,right,up,1));
}
static aliashdr_t *header(poseverttype_t format) {
 aliashdr_t *h=calloc(1,sizeof(*h));assert(h);h->poseverttype=format;h->numframes=h->numposes=1;h->numskins=2;
 h->vertex_buffer=(VkBuffer)(uintptr_t)1;h->index_buffer=(VkBuffer)(uintptr_t)2;h->joints_set=(VkDescriptorSet)(uintptr_t)3;
 h->radiance_mesh=(void *)(uintptr_t)4;h->scale[0]=h->scale[1]=h->scale[2]=1;
 h->gltextures[1][0]=&base;h->gltextures[1][1]=&other_phase;h->fbtextures[1][0]=&fullbright;return h;
}
static qboolean record(cb_context_t *cbx,const editor_map_model_t *placement,const editor_model_view_t *view) {
 unsigned old_loads=loads;drawing=true;qboolean result=EditorModelPreview_Draw(cbx,placement,view);drawing=false;
 assert(loads==old_loads);return result;
}
static void assets_and_draw(void) {
 editor_model_view_t view;vec3_t center={0};assert(EditorModelPreview_Ortho(&view,641,361,48,329,center,0,1,2,1));
 editor_map_model_t placement={.model="progs/preview.mdl",.skin=1,.modelscale=1.375,.origin={10,20,30},.angles={0,90,0}};
 strcpy(model.name,placement.model);model.type=mod_alias;
 native_header=header(PV_QUAKE1);replacement_header=header(PV_MD5);replacement_header->numframes=9;
 model.extradata[PV_QUAKE1]=(byte *)native_header;model.extradata[PV_MD5]=(byte *)replacement_header;
 cb_context_t cbx={.subpass_type=SUBPASS_UI,.current_canvas=CANVAS_DEFAULT};
 EditorModelPreview_BeginFrame();EditorModelPreview_Prepare(&placement);unsigned old_loads=loads;
 EditorModelPreview_Prepare(&placement);assert(loads==old_loads); /* prepare deduplicates assets/skins */
 gl_fullbrights.value=1;assert(record(&cbx,&placement,&view));
 assert(draws==1 && last_header==native_header && scissors==2 && restores==1 && canvases==1);
 assert(last_scissor.offset.x==0 && last_scissor.offset.y==0 && last_scissor.extent.width==641 && last_scissor.extent.height==361);
 assert(!memcmp(last_projection,view.projection,sizeof(last_projection)));
 near(last_matrix[0],0);near(last_matrix[1],1.375f);near(last_matrix[4],-1.375f);
 near(last_matrix[12],10);near(last_matrix[13],20);near(last_matrix[14],30);
 base.flags|=TEXPREF_ALPHAPIXELS;assert(record(&cbx,&placement,&view) && last_cutout);base.flags=0;
 placement.origin[0]=123;assert(record(&cbx,&placement,&view));near(last_matrix[12],123); /* live drag transform */
 pipelines=false;unsigned before=draws;assert(!record(&cbx,&placement,&view) && draws==before);pipelines=true;
 model.needload=true;assert(!record(&cbx,&placement,&view));model.needload=false;
 /* A main-thread preference/reload may free the old header. The next frame
  * reacquires the exact asset instead of retaining or loading from draw. */
 free(native_header);native_header=header(PV_QUAKE1);model.extradata[PV_QUAKE1]=(byte *)native_header;
 EditorModelPreview_BeginFrame();EditorModelPreview_Prepare(&placement);
 assert(record(&cbx,&placement,&view) && last_header==native_header);
 model.name[0]=0;assert(!record(&cbx,&placement,&view));strcpy(model.name,placement.model);
 EditorModelPreview_BeginFrame();assert(!record(&cbx,&placement,&view));
 missing=true;EditorModelPreview_Prepare(&placement);assert(!record(&cbx,&placement,&view));missing=false;
 native_header->numframes=2;EditorModelPreview_BeginFrame();EditorModelPreview_Prepare(&placement);
 assert(!record(&cbx,&placement,&view));native_header->numframes=1;
 /* Native MD3 and both MD5 streams retain frame0/materialphase0 without
  * an enhanced preference dependency. */
 for(unsigned kind=0;kind<3;++kind) {
  const char *extension=kind==0?"md3":"md5mesh";
  snprintf(placement.model,sizeof(placement.model),"progs/preview.%s",extension);strcpy(model.name,placement.model);
  poseverttype_t format=kind==0?PV_QUAKE3:kind==1?PV_MD5:PV_MD5_8;
  native_header->poseverttype=format;model.extradata[kind==0?PV_QUAKE3:PV_MD5]=(byte *)native_header;
  EditorModelPreview_BeginFrame();EditorModelPreview_Prepare(&placement);gl_fullbrights.value=0;
  assert(record(&cbx,&placement,&view) && last_header==native_header);
 }
 EditorModelPreview_Clear();assert(!record(&cbx,&placement,&view));
 free(native_header);free(replacement_header);
}
int main(void) { projection();assets_and_draw();puts("PASS: editor model projection, frame0 materials, exact asset reload and scoped draw state");return 0; }
'''

with tempfile.TemporaryDirectory(prefix='editor-model-preview-') as directory:
    directory = Path(directory)
    source, binary = directory / 'test.c', directory / 'test'
    source.write_text(PREFIX + BODY + SUFFIX)
    flags = subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True).split()
    extra = shlex.split(os.environ.get('EDITOR_PREVIEW_TEST_CFLAGS', ''))
    command = [os.environ.get('CC', 'cc'), '-std=gnu2x', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-DUSE_SDL3', '-I', str(ROOT / 'Quake'), '-I', str(ROOT / 'Quake/mimalloc'),
               *flags, *extra, str(source), str(ROOT / 'Quake/editor_model_preview.c'),
               str(ROOT / 'Quake/strlcpy.c'), '-lm', '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
