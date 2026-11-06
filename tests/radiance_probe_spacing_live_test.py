#!/usr/bin/env python3
"""Exercise the real live-spacing transaction with real sparse allocations."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'Quake/radiance_engine.c').read_text()
start = source.index('bool R_RadianceFrameApplySpacing (')
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
apply = source[start:end]
prefix = source[:source.index('bool R_RadianceGeometryCounts (')]
vulkan = (ROOT / 'Quake/radiance_vulkan.c').read_text()
start = vulkan.index('if (!vk->recorded_commands && (vk->needs_clear || vk->clear_count[frame]))')
brace = vulkan.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (vulkan[end] == '{') - (vulkan[end] == '}')
    end += 1
clear = 'static void FirstRecordClear(rc_vulkan_t *vk,VkCommandBuffer commands,uint32_t frame) {\n' + vulkan[start:end] + '\n}\n'
video = (ROOT / 'Quake/gl_vidsdl.c').read_text()
begin = video[video.index('qboolean GL_BeginRendering ('):video.index('qboolean GL_AcquireNextSwapChainImage (')]
assert begin.index('R_RadianceFrameApplySpacing ()') < begin.index('Task_AllocateAndAssignFunc (GL_BeginRenderingTask')
assert 'R_RadianceFrameApplySpacing' not in video[video.index('void GL_BeginRenderingTask ('):video.index('VkPhysicalDevice GL_RadiancePhysicalDevice (')]

fixture = r'''
#include <assert.h>
#include <stdarg.h>
#define H(type,value) ((type)(uintptr_t)(value))
#define RC_SparseCreate RealSparseCreate
#include "radiance_sparse.c"
#undef RC_SparseCreate
static unsigned creates,waits,publications,messages,filled;
static bool allocation_fails,keep_busy;
static rc_sparse_t *old_during_wait;
cvar_t r_radiance,r_radiance_probe_spacing;
qboolean Tasks_IsWorker(void) { return false; }
void Con_Printf(const char *format,...) { (void)format;++messages; }
rc_sparse_t *RC_SparseCreate(uint32_t side,uint32_t directions,float spacing,float interval,const uint32_t capacity[RC_CASCADE_COUNT]) {
 ++creates;return allocation_fails?NULL:RealSparseCreate(side,directions,spacing,interval,capacity);
}
void GL_WaitForDeviceIdle(void) {
 ++waits;assert(rc_engine.sparse==old_during_wait && !rc_engine.cascades.needs_clear);
 if(!keep_busy)for(unsigned i=0;i<DOUBLE_BUFFERED;++i)rc_engine.pending[i]=rc_engine.refresh_reserved[i]=false;
}
static void RCFrame_ClearPublication(const char *reason) { assert(waits && reason);++publications; }
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commands,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,uint32_t value) {
 assert(commands && !offset && size==8192 && !value);
 assert(buffer==H(VkBuffer,104) || buffer==H(VkBuffer,105));filled|=buffer==H(VkBuffer,104)?1:2;
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer commands,VkPipelineStageFlags source,VkPipelineStageFlags dest,
 VkDependencyFlags dependency,uint32_t count,const VkMemoryBarrier *memory,uint32_t buffers,const VkBufferMemoryBarrier *buffer,
 uint32_t images,const VkImageMemoryBarrier *image) {
 (void)dependency;(void)buffer;(void)image;
 assert(commands && source==VK_PIPELINE_STAGE_TRANSFER_BIT && !buffers && !images && count==1);
 assert((dest&(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT))==
  (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
 assert(memory->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && (memory->dstAccessMask&VK_ACCESS_SHADER_READ_BIT));
}
'''
suffix = r'''
static void setup(float spacing) {
 const uint32_t capacities[5]={512,64,8,8,8};
 memset(&rc_engine,0,sizeof(rc_engine));
 rc_engine.sparse=RealSparseCreate(32,2,spacing,spacing*2,capacities);assert(rc_engine.sparse);
 rc_engine.initialized=true;
 rc_engine.cascades.record_count=RC_SparseLayout(rc_engine.sparse)->record_count;
 RC_SparseTable(rc_engine.sparse,&rc_engine.cascades.table_capacity);
 for(unsigned i=0;i<6;++i) { rc_engine.cascades.records[i].buffer=H(VkBuffer,100+i);rc_engine.cascades.records[i].size=8192; }
 for(unsigned i=0;i<DOUBLE_BUFFERED;++i) {
  rc_engine.cascades.metadata[i].buffer=H(VkBuffer,200+i);
  rc_engine.cascades.descriptor_sets[i]=H(VkDescriptorSet,300+i);
  rc_engine.receiver.hardware_sets[i]=H(VkDescriptorSet,400+i);
 }
 rc_engine.world=H(const qmodel_t *,500);
 r_radiance.value=1;r_radiance_probe_spacing.value=spacing;
 creates=waits=publications=messages=filled=0;allocation_fails=keep_busy=false;
 old_during_wait=rc_engine.sparse;
}
static void seed_history(void) {
 memset(&rc_engine.scheduler,1,sizeof(rc_engine.scheduler));rc_engine.light_history=true;rc_engine.timing_available=true;
 rc_engine.cascades.submitted_revision=71;rc_engine.cascades.submitted_dirty_mask=31;
 rc_engine.cascades.recorded_dirty_mask=31;rc_engine.cascades.recorded_merge_intervals=100;
 for(unsigned i=0;i<DOUBLE_BUFFERED;++i) {
  rc_engine.cascades.snapshot_ready[i]=true;rc_engine.cascades.snapshot_revision[i]=71;
  rc_engine.cascades.clear_count[i]=2;rc_engine.cascades.work[i][0].active_count=50;
  rc_engine.timestamps_written[i]=true;rc_engine.refresh[i].count=3;
  rc_engine.pending[i]=rc_engine.refresh_reserved[i]=true;
 }
}
int main(void) {
 const float desired[]={16,24,32,64,128,8,19.5f};
 for(unsigned choice=0;choice<sizeof(desired)/sizeof(desired[0]);++choice) {
  setup(desired[choice]==32?64:32);seed_history();
  rc_vk_buffer_t buffers[6];memcpy(buffers,rc_engine.cascades.records,sizeof(buffers));
  r_radiance_probe_spacing.value=desired[choice];
  assert(R_RadianceFrameApplySpacing() && creates==1 && waits==1 && publications==1);
  assert(RC_SparseLayout(rc_engine.sparse)->levels[0].origin_spacing[3]==desired[choice]);
  assert(RC_SparseLayout(rc_engine.sparse)->levels[0].interval[1]==2*desired[choice]);
  assert(!memcmp(buffers,rc_engine.cascades.records,sizeof(buffers)) && rc_engine.world==H(const qmodel_t *,500));
  assert(rc_engine.cascades.needs_clear && !rc_engine.light_history && !rc_engine.timing_available);
  assert(!rc_engine.cascades.submitted_revision && !rc_engine.cascades.submitted_dirty_mask && !rc_engine.cascades.recorded_dirty_mask);
  assert(!rc_engine.cascades.recorded_merge_intervals && rc_engine.scheduler.initial_sweep);
  FirstRecordClear(&rc_engine.cascades,H(VkCommandBuffer,1),0);assert(filled==3);
  for(unsigned i=0;i<DOUBLE_BUFFERED;++i) {
   assert(!rc_engine.cascades.snapshot_ready[i] && !rc_engine.cascades.snapshot_revision[i] && !rc_engine.cascades.clear_count[i]);
   assert(!rc_engine.cascades.work[i][0].active_count && !rc_engine.timestamps_written[i] && !rc_engine.refresh[i].count);
   assert(rc_engine.cascades.metadata[i].buffer==H(VkBuffer,200+i) && rc_engine.cascades.descriptor_sets[i]==H(VkDescriptorSet,300+i));
   assert(rc_engine.receiver.hardware_sets[i]==H(VkDescriptorSet,400+i));
  }
  for(unsigned repeat=0;repeat<100;++repeat)assert(R_RadianceFrameApplySpacing());
  assert(creates==1 && waits==1);RC_SparseDestroy(rc_engine.sparse);
 }
 setup(32);r_radiance_probe_spacing.value=16;allocation_fails=true;
 assert(!R_RadianceFrameApplySpacing() && creates==1 && !waits && !publications && rc_engine.sparse==old_during_wait);
 for(unsigned repeat=0;repeat<100;++repeat)assert(!R_RadianceFrameApplySpacing());
 assert(creates==1 && messages==1);
 r_radiance_probe_spacing.value=32;assert(R_RadianceFrameApplySpacing());
 r_radiance_probe_spacing.value=16;allocation_fails=false;assert(R_RadianceFrameApplySpacing());
 assert(creates==2 && waits==1);RC_SparseDestroy(rc_engine.sparse);
 setup(32);r_radiance_probe_spacing.value=24;allocation_fails=true;assert(!R_RadianceFrameApplySpacing());
 r_radiance.value=0;assert(R_RadianceFrameApplySpacing());r_radiance.value=1;allocation_fails=false;
 assert(R_RadianceFrameApplySpacing() && creates==2 && waits==1);RC_SparseDestroy(rc_engine.sparse);
 setup(32);r_radiance_probe_spacing.value=64;++rc_engine.cascades.table_capacity;
 assert(!R_RadianceFrameApplySpacing() && !waits && rc_engine.sparse==old_during_wait);RC_SparseDestroy(rc_engine.sparse);
 setup(32);r_radiance_probe_spacing.value=64;rc_engine.pending[0]=true;keep_busy=true;
 assert(!R_RadianceFrameApplySpacing() && waits==1 && !publications && rc_engine.sparse==old_during_wait);
 assert(RC_SparseLayout(rc_engine.sparse)->levels[0].origin_spacing[3]==32);RC_SparseDestroy(rc_engine.sparse);
 memset(&rc_engine,0,sizeof(rc_engine));assert(R_RadianceFrameApplySpacing());
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rc-live-spacing-') as directory:
    directory = Path(directory)
    code, binary = directory / 'spacing.c', directory / 'spacing'
    code.write_text(prefix + fixture + apply + clear + suffix)
    flags = shlex.split(os.environ.get('RC_TEST_CFLAGS', '-O2'))
    sdl_flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True))
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu2x', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    *flags, *sdl_flags, '-DUSE_SDL3', '-I' + str(ROOT / 'Quake'), str(code), str(ROOT / 'Quake/radiance_cascades.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: live spacing transaction reuses GPU resources and fences old probes')
