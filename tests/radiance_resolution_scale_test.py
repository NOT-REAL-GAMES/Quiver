#!/usr/bin/env python3
"""Test actual scale normalization and compatibility callbacks without a game."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'Quake/radiance_runtime.c').read_text()
body = source[source.index('float R_RadianceResolutionDivisor'):source.index('static radiance_runtime_stats_t')]
prefix = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { float value; char string[32]; } cvar_t;
static cvar_t r_radiance_resolution_scale={1,"1"}, r_radiance_half_resolution={0,"0"};
static void R_RadianceResolutionChanged(cvar_t *var);
static void Con_Printf(const char *format, ...) { (void)format; }
static void Cvar_SetQuick(cvar_t *var,const char *value) {
 if(!strcmp(var->string,value)) return;
 strcpy(var->string,value); var->value=strtof(value,NULL); R_RadianceResolutionChanged(var);
}
'''
suffix = r'''
int main(void) {
 const char *requests[]={"0.5","0.333","0.333333333","0.25","0.1","0.74","0.6","0.42","0.4","0.3","0.29","1","0","-1","nan","inf",
  "0.666","0.666666667","0.667","0.8","0.84","0.58","0.59"};
 const float divisors[]={2,3,3,4,4,1.5f,1.5f,2,3,3,4,1,1,1,1,1,1.5f,1.5f,1.5f,1.5f,1,2,1.5f};
 for(unsigned i=0;i<sizeof(divisors)/sizeof(divisors[0]);i++) {
  Cvar_SetQuick(&r_radiance_resolution_scale,requests[i]);
  assert(R_RadianceResolutionDivisor()==divisors[i]);
  assert(r_radiance_resolution_scale.value==1.0f/divisors[i]);
  assert(r_radiance_half_resolution.value==(divisors[i]==1.5f?4:divisors[i]-1));
 }
 const char *reduced[]={"0.333","0.25","0.666"};
 for(unsigned i=0;i<sizeof(reduced)/sizeof(reduced[0]);i++) {
  Cvar_SetQuick(&r_radiance_resolution_scale,reduced[i]);
  Cvar_SetQuick(&r_radiance_half_resolution,"1"); assert(R_RadianceResolutionDivisor()==2);
  Cvar_SetQuick(&r_radiance_half_resolution,"0"); assert(R_RadianceResolutionDivisor()==1);
  Cvar_SetQuick(&r_radiance_resolution_scale,reduced[i]);
  Cvar_SetQuick(&r_radiance_half_resolution,"0"); assert(R_RadianceResolutionDivisor()==1);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='radiance-scale-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(prefix + body + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-lm', '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: scale normalization and legacy alias transitions')
