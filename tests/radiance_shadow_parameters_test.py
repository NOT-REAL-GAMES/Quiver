#!/usr/bin/env python3
"""Check real receiver shadow sanitization, std140 ABI and upload validation."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
runtime = (root / 'Quake/radiance_runtime.c').read_text()
inputs = (root / 'Quake/radiance_gpu_inputs.c').read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


for name, default in [('r_radiance_soft_shadows', '0'),
                      ('r_radiance_shadow_radius', '2'),
                      ('r_radiance_shadow_sun_angle', '0.5'),
                      ('r_radiance_shadow_filter', '1')]:
    if not re.search(r'cvar_t\s+' + name + r'\s*=\s*\{"' + name + r'",\s*"' + re.escape(default) + r'",\s*CVAR_ARCHIVE\}', runtime):
        raise RuntimeError(f'{name}: archived default changed')

prefix = r'''
#include "radiance_gpu_inputs.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
typedef struct { float value; } cvar_t;
static cvar_t r_radiance_soft_shadows={0},r_radiance_shadow_radius={4},r_radiance_shadow_sun_angle={.5f},r_radiance_shadow_filter={1};
'''
body = function(runtime, 'void R_RadianceShadowParameters')
body += function(inputs, 'static bool RadianceInputs_ValidateSnapshot')
suffix = r'''
int main(void) {
 _Static_assert(sizeof(radiance_lighting_parameters_t)==160,"std140 lighting block size");
 _Static_assert(offsetof(radiance_lighting_parameters_t,receiver_filter)==128,"existing field offset");
 _Static_assert(offsetof(radiance_lighting_parameters_t,shadow_parameters)==144,"new shadow offset");
 float out[4]; R_RadianceShadowParameters(out);
 assert(out[0]==0 && out[1]==4 && fabsf(out[2]-.00872664626f)<1e-9f && out[3]==1);
 r_radiance_soft_shadows.value=.01f; r_radiance_shadow_radius.value=100; r_radiance_shadow_sun_angle.value=90;
 R_RadianceShadowParameters(out);
 const float maximum=15.0f*.01745329251994329577f;
 assert(out[0]==1 && out[1]==64 && out[2]==maximum && out[3]==1);
 /* Rectangle emitters still use their own source geometry at zero global radii. */
 r_radiance_shadow_radius.value=0;r_radiance_shadow_sun_angle.value=0;
 R_RadianceShadowParameters(out);assert(out[0]==1 && out[1]==0 && out[2]==0);
 const float invalid[]={NAN,INFINITY,-INFINITY,-1};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
  r_radiance_soft_shadows.value=invalid[i];r_radiance_shadow_radius.value=invalid[i];r_radiance_shadow_sun_angle.value=invalid[i];r_radiance_shadow_filter.value=invalid[i];
  out[3]=123;R_RadianceShadowParameters(out);assert(out[0]==0 && out[1]==0 && out[2]==0 && out[3]==0);
 }
 r_radiance_soft_shadows.value=1;r_radiance_shadow_radius.value=4;r_radiance_shadow_sun_angle.value=.5f;
 const float strengths[]={0,.25f,1,1.5f,2,3};
 for(unsigned i=0;i<sizeof(strengths)/sizeof(*strengths);++i) {
  r_radiance_shadow_filter.value=strengths[i];R_RadianceShadowParameters(out);
  assert(out[3]==fminf(strengths[i],2));
 }
 R_RadianceShadowParameters(out);
 uint8_t rgba[4]={0};VkDeviceSize bytes=0;
 radiance_gpu_inputs_t inputs={.limits={.maxImageDimension2D=1,.maxImageArrayLayers=1}};
 radiance_input_snapshot_t snapshot={.atlas_rgba=rgba,.atlas_width=1,.atlas_height=1,.atlas_layers=1};
 snapshot.parameters.counts[2]=1;
 memcpy(snapshot.parameters.shadow_parameters,out,sizeof(out));
 assert(RadianceInputs_ValidateSnapshot(&inputs,&snapshot,&bytes) && bytes==4);
 const float bad[][4]={{-.1f,0,0,0},{.5f,0,0,0},{2,0,0,0},{NAN,0,0,0},{INFINITY,0,0,0},
  {1,-.1f,0,0},{1,64.01f,0,0},{1,NAN,0,0},{1,INFINITY,0,0},
  {1,4,-.1f,0},{1,4,.262f,0},{1,4,NAN,0},{1,4,INFINITY,0},
  {1,4,0,2.01f},{1,4,0,-1},{1,4,0,NAN},{1,4,0,INFINITY}};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
  memcpy(snapshot.parameters.shadow_parameters,bad[i],sizeof(out));
  assert(!RadianceInputs_ValidateSnapshot(&inputs,&snapshot,&bytes));
 }
 const float valid[][4]={{0,0,0,0},{1,0,0,0},{1,64,0,0},{1,0,maximum,0},{1,64,maximum,0},
  {0,0,0,1},{1,4,0,.25f},{1,64,maximum,2}};
 for(unsigned i=0;i<sizeof(valid)/sizeof(valid[0]);++i) {
  memcpy(snapshot.parameters.shadow_parameters,valid[i],sizeof(out));
  assert(RadianceInputs_ValidateSnapshot(&inputs,&snapshot,&bytes));
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='radiance-shadow-parameters-') as directory:
    folder = Path(directory)
    source = folder / 'test.c'
    binary = folder / 'test'
    source.write_text(prefix + body + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'Quake'),
                    str(source), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: shadow defaults/sanitization,160-byte ABI and malformed upload rejection')
