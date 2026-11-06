#!/usr/bin/env python3
"""Simulate next-frame camera writes between real capture begin/finish code."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'Quake/gl_rmain.c').read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


capture = function('static bool RCExperimentalCaptureView (')
begin = function('static void RCExperimentalCaptureBegin (')
finish = function('void RCExperimentalCaptureFinish (')
# A future late read would bypass the snapshot even if its helper still passed.
for late_read in ('vulkan_globals.view_projection_matrix', 'r_scene_vrect', 'vid.render_width', 'vid.render_height'):
    assert late_read not in finish, late_read
view_type = re.search(r'typedef struct \{\s*rc_receiver_params_t params;.*?\} rc_experimental_view_t;', source, re.S).group()
first = finish.index('const uint32_t slot=GL_RenderFrameSlot();')
last = finish.index('static unsigned preflight_reports;', first)
finish_view = finish[first:last]
first = finish.index('const bool texture_filter=rc_capture_view.texture_filter;')
last = finish.index('for(uint32_t i=0;i<snapshot.count;++i)', first)
filter_selection = finish[first:last]
prepare = (ROOT / 'Quake/gl_rmisc.c').read_text()
prepare = prepare[prepare.index('bool R_ReceiverExperimentalPrepare('):prepare.index('bool R_ReceiverExperimentalModelsPrepare(')]
assert 'R_RadianceResolutionDivisor' not in prepare
assert 'R_RadianceShadowParameters' not in prepare
assert 'r_radiance_receiver_filter.value' not in prepare
first = prepare.index('struct { int32_t viewport[4]; float threshold[4]; } data=')
last = prepare.index('memcpy(a->mapped,&data,sizeof(data));', first) + len('memcpy(a->mapped,&data,sizeof(data));')
prepare_uniform = prepare[first:last]

prefix = r'''
#include "radiance_receiver_record.h"
#include "radiance_receiver_capture.h"
#include "radiance_receiver_model.h"
#include "radiance_matrix.h"
#include <assert.h>
#include <math.h>
#include <string.h>
enum { NUM_WORLD_CBX=2, NUM_ENTITIES_CBX=2, RC_EXPERIMENTAL_CAPTURE_CAPACITY=4, RC_EXPERIMENTAL_MODEL_CAPACITY=4 };
static struct { int render_width,render_height; } vid;
static struct { int x,y,width,height; } r_scene_vrect;
static struct { float view_projection_matrix[16]; VkSampleCountFlagBits sample_count; } vulkan_globals;
static struct { float value; } r_radiance_deferred,r_radiance_receiver_filter,r_radiance_checkerboard,r_radiance_bounce_gain,r_radiance_probe_gaussian;
static float live_shadow_parameters[4];
static void R_RadianceShadowParameters(float out[4]) { memcpy(out,live_shadow_parameters,sizeof(live_shadow_parameters)); }
static bool indirect,active=true;
static float divisor;
static uint32_t r_framecount=19;
static float R_RadianceResolutionDivisor(void) { return divisor; }
static bool R_RadianceActive(void) { return active; }
static void Con_Printf(const char *format,...) { (void)format; }
static rc_receiver_world_capture_t rc_capture_workers[NUM_WORLD_CBX];
static rc_receiver_world_draw_t rc_capture_draws[NUM_WORLD_CBX][RC_EXPERIMENTAL_CAPTURE_CAPACITY];
static rc_receiver_forward_material_t rc_capture_materials[NUM_WORLD_CBX][RC_EXPERIMENTAL_CAPTURE_CAPACITY];
static rc_receiver_model_capture_t rc_model_workers[NUM_ENTITIES_CBX];
static rc_receiver_model_draw_t rc_model_draws[NUM_ENTITIES_CBX][RC_EXPERIMENTAL_MODEL_CAPACITY];
static rc_receiver_model_snapshot_t rc_model_snapshot;
static uint32_t rc_model_capture_count,rc_capture_count;
static uint64_t rc_capture_serial;
static bool rc_capture_requested;
static unsigned installs;
void RCReceiverCapture_Reset(void) { installs=0; }
static void RCReceiverModelCapture_Reset(void) { installs=0; }
static bool RCReceiverCapture_Install(uint32_t slot,rc_receiver_world_capture_t *p) { assert(slot<2 && p->frame_serial==19);++installs;return true; }
static bool RCReceiverModelCapture_Install(uint32_t slot,rc_receiver_model_capture_t *p) { assert(slot<2 && p->frame_serial==19);++installs;return true; }
typedef struct { unsigned unused; } rc_experimental_bindings_t;
static uint32_t GL_RenderFrameSlot(void) { return 1; }
static bool R_RadianceExperimentalBindings(uint32_t slot,rc_experimental_bindings_t *bindings) { assert(slot==1);memset(bindings,0,sizeof(*bindings));return true; }
static uint32_t prepared_width,prepared_height;
static bool prepared_texture,prepared_soft;
static int32_t prepared_view[4];
static struct { int32_t viewport[4]; float threshold[4]; } prepared_uniform;
static bool R_ReceiverExperimentalPrepare(uint32_t slot,VkRenderPass pass,uint32_t width,uint32_t height,
 const rc_experimental_bindings_t *bindings,const int32_t viewport[4],float divisor,bool texture_filter,bool soft_shadows,VkCommandPool *pool) {
 (void)bindings;assert(slot==1 && pass && pool);*pool=(VkCommandPool)(uintptr_t)1;
 struct { void *mapped; } aux={&prepared_uniform},*a=&aux;
 PREPARE_UNIFORM
 prepared_texture=texture_filter;prepared_soft=soft_shadows;
 prepared_width=width;prepared_height=height;memcpy(prepared_view,viewport,sizeof(prepared_view));return true;
}
'''
prefix = prefix.replace('PREPARE_UNIFORM', prepare_uniform)
# Headers declare these APIs externally; use the inert definitions in this TU.
prefix = prefix.replace('static void RCReceiverModelCapture_Reset', 'void RCReceiverModelCapture_Reset')
prefix = prefix.replace('static bool RCReceiverCapture_Install', 'bool RCReceiverCapture_Install')
prefix = prefix.replace('static bool RCReceiverModelCapture_Install', 'bool RCReceiverModelCapture_Install')
body = view_type + '\nstatic rc_experimental_view_t rc_capture_view;\n' + capture + begin
body += 'static rc_receiver_params_t FinishView(void) {\nVkRenderPass pass=(VkRenderPass)(uintptr_t)1;\n' + finish_view
body += 'assert(view_ok && bindings_ok && prepared && pool);return params;\n}\n'
body += r'''
static bool ResolvedChoice(bool pipeline) {
 struct { struct { VkPipeline soft_shadows; struct { VkDescriptorSet forward_resolved,forward; } descriptors; } resources; }
  slot={.resources={.soft_shadows=pipeline?(VkPipeline)(uintptr_t)1:VK_NULL_HANDLE,
   .descriptors={.forward_resolved=(VkDescriptorSet)(uintptr_t)2,.forward=(VkDescriptorSet)(uintptr_t)1}}},*s=&slot;
'''
body += filter_selection + 'return receiver_set==s->resources.descriptors.forward_resolved;\n}\n'
suffix = r'''
static void set_view(void) {
 vid.render_width=641;vid.render_height=361;
 r_scene_vrect.x=3;r_scene_vrect.y=7;r_scene_vrect.width=633;r_scene_vrect.height=349;
 memset(vulkan_globals.view_projection_matrix,0,64);
 for(unsigned i=0;i<4;++i)vulkan_globals.view_projection_matrix[i*5]=1;
 vulkan_globals.view_projection_matrix[12]=17;
 vulkan_globals.view_projection_matrix[13]=-23;
 vulkan_globals.view_projection_matrix[14]=31;
 vulkan_globals.sample_count=VK_SAMPLE_COUNT_1_BIT;
 r_radiance_deferred.value=1;r_radiance_receiver_filter.value=1;active=true;indirect=false;
 r_radiance_checkerboard.value=1;r_radiance_bounce_gain.value=1;r_radiance_probe_gaussian.value=2;
 live_shadow_parameters[0]=1;live_shadow_parameters[1]=2;live_shadow_parameters[2]=.01f;live_shadow_parameters[3]=1.5f;
}
int main(void) {
 const float divisors[]={1,1.5,2,3,4};
 for(unsigned mode=0;mode<5;++mode)for(unsigned task=0;task<2;++task) {
  set_view();divisor=divisors[mode];bool tasks=task!=0;
  RCExperimentalCaptureBegin(&tasks);assert(rc_capture_requested && installs==(task?4:2));
  rc_experimental_view_t saved=rc_capture_view;
  assert(saved.params.divisor==divisor && saved.params.inverse_view_projection[12]==-17);
  assert(saved.texture_filter && saved.smooth_indirect && saved.checkerboard==(saved.params.divisor>1));
  assert(!memcmp(saved.shadow_parameters,live_shadow_parameters,sizeof(live_shadow_parameters)));
  assert(saved.params.inverse_view_projection[13]==23 && saved.params.inverse_view_projection[14]==-31);
  assert(saved.params.ndc_transform[0]==2.f/633 && saved.params.ndc_transform[1]==2.f/349);
  assert(saved.params.ndc_transform[2]==-1.f-3*(2.f/633) && saved.params.ndc_transform[3]==-1.f-7*(2.f/349));
  /* Deterministically run the next frame's writes before the old Finish. */
  vid.render_width=1920;vid.render_height=1080;
  r_scene_vrect.x=0;r_scene_vrect.y=0;r_scene_vrect.width=1920;r_scene_vrect.height=1080;
  vulkan_globals.view_projection_matrix[12]=1800;
  divisor=divisors[(mode+1)%5];
  r_radiance_receiver_filter.value=0;r_radiance_checkerboard.value=0;
  r_radiance_bounce_gain.value=0;r_radiance_probe_gaussian.value=0;memset(live_shadow_parameters,0,sizeof(live_shadow_parameters));
  rc_receiver_params_t actual=FinishView();
  assert(!memcmp(&actual,&saved.params,sizeof(actual)));
  assert(prepared_width==641 && prepared_height==361 && !memcmp(prepared_view,saved.params.viewport,sizeof(prepared_view)));
  assert(prepared_uniform.threshold[2]==saved.params.divisor && !memcmp(prepared_uniform.viewport,saved.params.viewport,sizeof(prepared_view)));
  assert(prepared_texture && prepared_soft && !memcmp(&rc_capture_view,&saved,sizeof(saved)));
 }
 for(unsigned invalid=0;invalid<8;++invalid) {
  set_view();divisor=2;bool tasks=true;
  switch(invalid) {
   case 0:r_scene_vrect.width=0;break;case 1:r_scene_vrect.x=-1;break;
   case 2:r_scene_vrect.width=641;break;case 3:r_scene_vrect.height=361;break;
   case 4:vid.render_width=0;break;case 5:vulkan_globals.view_projection_matrix[0]=NAN;break;
   case 6:memset(vulkan_globals.view_projection_matrix,0,64);break;
   case 7:vulkan_globals.view_projection_matrix[0]=INFINITY;break;
  }
  RCExperimentalCaptureBegin(&tasks);assert(!rc_capture_requested && !installs && !rc_capture_view.width);
 }
 set_view();divisor=1;bool tasks=true;RCExperimentalCaptureBegin(&tasks);
 assert(rc_capture_requested && rc_capture_view.params.viewport[2]==633);
 r_scene_vrect.x=r_scene_vrect.y=0;r_scene_vrect.width=641;r_scene_vrect.height=361;
 RCExperimentalCaptureBegin(&tasks);assert(rc_capture_requested && rc_capture_view.params.viewport[2]==641);
 active=false;RCExperimentalCaptureBegin(&tasks);assert(!rc_capture_requested && !installs && !rc_capture_view.width);
 const float strengths[]={0,.5f,1,2};
 for(unsigned d=0;d<5;++d)for(unsigned soft=0;soft<2;++soft)for(unsigned strength=0;strength<4;++strength)
 for(unsigned pipeline=0;pipeline<2;++pipeline)for(unsigned bounce=0;bounce<2;++bounce)
 for(unsigned gaussian=0;gaussian<2;++gaussian)for(unsigned checker=0;checker<2;++checker) {
  set_view();divisor=divisors[d];r_radiance_bounce_gain.value=bounce;r_radiance_probe_gaussian.value=gaussian?2:0;
  r_radiance_checkerboard.value=checker;live_shadow_parameters[0]=soft;live_shadow_parameters[3]=strengths[strength];
  RCExperimentalCaptureBegin(&tasks);assert(rc_capture_requested);
  bool expected=(checker && divisor>1) || (bounce && gaussian) || (soft && strength && pipeline);
  assert(ResolvedChoice(pipeline)==expected);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rc-capture-view-') as directory:
    directory = Path(directory)
    code, binary = directory / 'view.c', directory / 'view'
    code.write_text(prefix + body + suffix)
    flags = shlex.split(os.environ.get('RC_TEST_CFLAGS', '-O2'))
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-Wall', '-Wextra', '-Werror',
                    *flags, '-I' + str(ROOT / 'Quake'), str(code), str(ROOT / 'Quake/radiance_matrix.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: receiver capture holds its camera/viewport across next-frame setup')
