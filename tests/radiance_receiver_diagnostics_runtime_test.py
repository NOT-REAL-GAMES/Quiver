#!/usr/bin/env python3
"""Fence-completed coverage survives transient frame-publication resets."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'Quake/radiance_runtime.c').read_text()
body = source[source.index('static radiance_runtime_stats_t radiance_stats;'):source.index('bool R_RadianceActive (void)')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct { float value; } cvar_t;
#include "radiance_runtime.h"
#include "radiance_receiver_scale.h"
typedef struct { uint32_t value; } atomic_uint32_t;
static void Atomic_StoreUInt32(atomic_uint32_t *dst,uint32_t value) { dst->value=value; }
static uint32_t Atomic_IncrementUInt32(atomic_uint32_t *dst) { return ++dst->value; }
static uint32_t Atomic_LoadUInt32(const atomic_uint32_t *src) { return src->value; }
static void q_strlcpy(char *dst,const char *src,size_t size) { snprintf(dst,size,"%s",src); }
'''
suffix = r'''
int main(void) {
 radiance_runtime_stats_t runtime={.active=true,.probe_spacing=32};
 rc_receiver_diagnostic_stats_t completed={.frame_serial=42,.width=641,.height=361,.divisor=4,.use_resolved=true};
 completed.count[RC_DIAG_TOTAL_BLOCKS]=14651;
 R_RadianceSetStats(&runtime,NULL);
 R_RadianceRecordReceiverSoftUse(true);assert(radiance_receiver_soft_use.value==1);
 R_RadianceRecordReceiverShadowFilterUse(1.5f,12);
 assert(R_RadianceActiveShadowFilter()==1.5f && radiance_shadow_filter_active_frames.value==1);
 assert(R_RadianceActiveShadowFilterRadius()==12);
 assert(radiance_soft_active_frames.value==1 && radiance_soft_inactive_frames.value==0);
 R_RadianceRecordReceiverUse(4,true,true);
 assert(radiance_receiver_reduced_frames.value==1 && radiance_receiver_full_frames.value==0);
 R_RadianceSetReceiverDiagnostics(&completed);
 /* This reset happens on every frame after the previous fence is collected. */
 R_RadianceReset("frame resources not ready");
 assert(radiance_receiver_soft_use.value==1);
 assert(R_RadianceActiveShadowFilter()==1.5f);
 assert(radiance_soft_active_frames.value==1 && radiance_soft_inactive_frames.value==0);
 assert(radiance_receiver_diagnostics_valid);
 assert(radiance_receiver_diagnostics.frame_serial==42);
 assert(radiance_receiver_diagnostics.count[RC_DIAG_TOTAL_BLOCKS]==14651);
 assert(radiance_receiver_reduced_frames.value==1 && radiance_receiver_full_frames.value==0);
 R_RadianceSetStats(&runtime,NULL);
 assert(R_RadianceActiveProbeSpacing()==32);
 R_RadianceRecordReceiverUse(3,false,true);
 assert(radiance_receiver_reduced_frames.value==2 && radiance_receiver_full_frames.value==0);
 assert(radiance_receiver_diagnostics_valid && radiance_receiver_diagnostics.divisor==4);
 /* Completed metadata identifies its own older frame; unsupported/reference
    submissions must invalidate it instead of presenting stale estimates. */
 R_RadianceSetReceiverSoftUse(true);
 R_RadianceRecordReceiverUse(1,false,false);
 assert(radiance_receiver_soft_use.value==0);
 assert(R_RadianceActiveShadowFilter()==0);
 assert(radiance_receiver_reduced_frames.value==2 && radiance_receiver_full_frames.value==1);
 assert(!radiance_receiver_diagnostics_valid);
 R_RadianceSetReceiverDiagnostics(&completed);
 R_RadianceSetReceiverSoftUse(true);
 R_RadianceRecordReceiverUse(4,true,false);
 assert(radiance_receiver_soft_use.value==0);
 assert(radiance_receiver_reduced_frames.value==3 && radiance_receiver_full_frames.value==1);
 assert(!radiance_receiver_diagnostics_valid);
 /* Counters span mode changes and transient preparation resets, so a timing
    interval can reject a hidden fallback even if its endpoints are reduced. */
 R_RadianceSetReceiverUse(1,false,false);
 assert(radiance_receiver_reduced_frames.value==3 && radiance_receiver_full_frames.value==1);
 uint32_t full_before=radiance_receiver_full_frames.value;
 R_RadianceRecordReceiverUse(0,false,false);
 R_RadianceRecordReceiverUse(2,false,true);
 assert(radiance_receiver_full_frames.value-full_before==1);
 assert(radiance_receiver_reduced_frames.value==4);
 radiance_receiver_full_frames.value=UINT32_MAX;
 R_RadianceRecordReceiverUse(1,false,false);
 assert(radiance_receiver_full_frames.value==0);
 /* A two-thirds frame uses a private atomic code, but remains a reduced
    submission and preserves the true fractional divisor in coverage. */
 completed.divisor=1.5f;
 R_RadianceSetReceiverDiagnostics(&completed);
 uint32_t reduced_before=radiance_receiver_reduced_frames.value;
 R_RadianceRecordReceiverUse(1.5f,true,true);
 assert(radiance_receiver_reduced_frames.value==reduced_before+1);
 /* Flags and the true fractional divisor are stored separately. */
 float stored;memcpy(&stored,&radiance_receiver_divisor.value,sizeof(stored));
 assert(radiance_receiver_use.value==(8u|16u) && stored==1.5f);
 assert(radiance_receiver_diagnostics_valid && radiance_receiver_diagnostics.divisor==1.5f);
 /* A divisor outside the valid range falls back; any value inside is real. */
 R_RadianceRecordReceiverUse(17,true,true);
 memcpy(&stored,&radiance_receiver_divisor.value,sizeof(stored));
 assert(radiance_receiver_full_frames.value==1 && radiance_receiver_use.value==0 && stored==1);
 assert(!radiance_receiver_diagnostics_valid);
 /* Full-resolution deferred reuse remains distinct from the forward fallback. */
 completed.divisor=1;R_RadianceSetReceiverDiagnostics(&completed);
 R_RadianceRecordReceiverUse(1,true,true);
 assert(radiance_receiver_use.value==16u);
 assert(radiance_receiver_diagnostics_valid && radiance_receiver_diagnostics.divisor==1);
 R_RadianceRecordModelReceiverUse(26,262266,true);
 assert(radiance_model_draws.value==26 && radiance_model_triangles.value==262266 && radiance_model_reused.value==1);
 R_RadianceRecordModelReceiverUse(26,262266,false);
 assert(radiance_model_reused.value==0 && radiance_model_active_frames.value==1 && radiance_model_fallback_frames.value==1);
 /* Actual skipped post work counts inactive independently of lifecycle
    publication resets; even a single skipped pass is visible to timing. */
 R_RadianceRecordReceiverSoftUse(false);
 assert(radiance_soft_active_frames.value==1 && radiance_soft_inactive_frames.value==1);
 R_RadianceRecordReceiverSoftUse(true);
 assert(radiance_receiver_soft_use.value==1 && radiance_soft_active_frames.value==2);
 radiance_soft_active_frames.value=UINT32_MAX;
 R_RadianceRecordReceiverSoftUse(true);
 assert(radiance_soft_active_frames.value==0);
 R_RadianceRecordReceiverShadowFilterUse(0,8);
 R_RadianceRecordReceiverShadowFilterUse(NAN,8);
 R_RadianceRecordReceiverShadowFilterUse(-1,8);
 assert(R_RadianceActiveShadowFilter()==0 && radiance_shadow_filter_inactive_frames.value==3);
 R_RadianceRecordReceiverShadowFilterUse(3,200);
 assert(R_RadianceActiveShadowFilter()==2);
 assert(R_RadianceActiveShadowFilterRadius()==128);
 radiance_shadow_filter_active_frames.value=UINT32_MAX;
 R_RadianceRecordReceiverShadowFilterUse(.25f,8);
 assert(radiance_shadow_filter_active_frames.value==0 && R_RadianceActiveShadowFilter()==.25f);
 R_RadianceRecordReceiverShadowFilterUse(1,0);
 R_RadianceRecordReceiverShadowFilterUse(1,NAN);
 assert(R_RadianceActiveShadowFilter()==0 && R_RadianceActiveShadowFilterRadius()==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='radiance-diagnostic-publication-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + body + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'Quake'),
                    str(path / 'test.c'), '-lm', '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: completed coverage and receiver frame counters survive reset and detect fallback')
