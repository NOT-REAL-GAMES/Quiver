#!/usr/bin/env python3
"""Test scale selection and the block mapping shared by the host and shaders."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'Quake/radiance_runtime.c').read_text()
body = source[source.index('float R_RadianceResolutionDivisor'):source.index('static void R_RadianceProbeSpacingChanged')]
prefix = r"""
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "radiance_receiver_scale.h"
typedef struct { float value; } cvar_t;
static cvar_t r_radiance_resolution_scale={1};
"""
suffix = r"""
static float divisor_for(float scale) { r_radiance_resolution_scale.value=scale; return R_RadianceResolutionDivisor(); }
int main(void) {
 /* Rounded spellings keep selecting the exact fraction they mean. */
 assert(divisor_for(.5f)==2 && divisor_for(.333f)==3 && divisor_for(.333333333f)==3 && divisor_for(.25f)==4);
 assert(divisor_for(.666f)==1.5f && divisor_for(.666666667f)==1.5f && divisor_for(.667f)==1.5f);
 /* Everything between is used as requested. */
 for(float scale=.0626f;scale<.99f;scale+=.0171f) {
  float divisor=divisor_for(scale);
  assert(RCReceiver_ValidDivisor(divisor) && divisor>1 && fabsf(divisor-1/scale)<=.0025f*divisor+.01f);
 }
 assert(fabsf(divisor_for(.37f)-1/.37f)<.0001f && fabsf(divisor_for(.8f)-1.25f)<.0001f);
 /* Invalid or full requests use full resolution; tiny ones clamp. */
 assert(divisor_for(1)==1 && divisor_for(2)==1 && divisor_for(0)==1 && divisor_for(-1)==1 && divisor_for(NAN)==1 && divisor_for(INFINITY)==1);
 assert(divisor_for(.01f)==RC_RECEIVER_MAX_DIVISOR);
 assert(!RCReceiver_ValidDivisor(.5f) && !RCReceiver_ValidDivisor(17) && !RCReceiver_ValidDivisor(NAN));
 /* Established grids keep their exact extents. */
 assert(RCReceiver_ScaledExtent(641,1)==641 && RCReceiver_ScaledExtent(641,2)==321 && RCReceiver_ScaledExtent(641,3)==214 && RCReceiver_ScaledExtent(641,4)==161);
 for(uint32_t w=1;w<4000;++w) assert(RCReceiver_ScaledExtent(w,1.5f)==(w*2+2)/3);
 /* At any divisor, blocks tile the viewport with no gap or overlap. */
 const float divisors[]={1,1.0001f,1.25f,1.37f,1.5f,2,2.7027f,3,3.3f,4,4.5f,7.3f,11.11f,16};
 const uint32_t sizes[]={1,2,3,7,361,641,1280,1440,3440,5120};
 for(size_t d=0;d<sizeof(divisors)/sizeof(divisors[0]);++d) for(size_t s=0;s<sizeof(sizes)/sizeof(sizes[0]);++s) {
  uint32_t extent=RCReceiver_ScaledExtent(sizes[s],divisors[d]);
  assert(extent>=1 && RCReceiver_FirstPixel(0,divisors[d])==0);
  assert(RCReceiver_FirstPixel(extent-1,divisors[d])<sizes[s] && RCReceiver_FirstPixel(extent,divisors[d])>=sizes[s]);
  for(uint32_t b=0;b<extent;++b) {
   uint32_t width=RCReceiver_FirstPixel(b+1,divisors[d])-RCReceiver_FirstPixel(b,divisors[d]);
   assert(width>=1 && width<=(uint32_t)ceilf(divisors[d])+1);
  }
 }
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='radiance-scale-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(prefix + body + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'Quake'), str(path / 'test.c'), '-lm', '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: receiver scale selection and block tiling')
