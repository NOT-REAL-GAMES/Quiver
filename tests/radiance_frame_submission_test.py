#!/usr/bin/env python3
"""Exercise the actual frame recorder's optional radiance submission.

The recorder is compiled with inert graphics stubs so this check does not
need a display or modify game state. Only prepared and record-work steps are
executed; no Vulkan command is sent to a device.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Quake/r_passes.c').read_text()
start = source.index('uint32_t R_RecordFrame (')
opening = source.index('{', start)
depth = 1
end = opening + 1
while depth:
    if source[end] == '{':
        depth += 1
    elif source[end] == '}':
        depth -= 1
    end += 1
recorder = source[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "r_passes.h"
#define MAX_PASS_ATTACHMENTS 8
enum { FRAME_BEGIN_GRAPHICS, FRAME_NEXT_SUBPASS, FRAME_GRAPHICS_WORK, FRAME_RECORD_WORK,
       FRAME_SCREEN_EFFECTS, FRAME_READBACK, FRAME_END_GRAPHICS, FRAME_PREPARED_COMMANDS };
enum { FRAME_TARGET_SCENE, FRAME_TARGET_UI };
enum { DRAW_WORLD = 1, DRAW_MODELS };
enum { PCBX_FIRST, PCBX_RADIANCE, PCBX_LAST, PCBX_RENDER_PASSES, PCBX_NUM };
typedef struct { VkCommandBuffer cb; } cb_context_t;
typedef struct { int type; void (*recorder)(cb_context_t *); uint32_t pass, subpass; int first_context, last_context, draw_stage; } frame_step_t;
typedef struct { int target; } graphics_pass_desc_t;
typedef struct { uint32_t step_count; frame_step_t steps[8]; graphics_pass_desc_t passes[2]; } frame_desc_t;
typedef struct { VkSampleCountFlagBits samples; bool upscale; frame_desc_t variants[MAIN_RENDER_PASS_VARIANT_COUNT]; } frame_layout_t;
typedef struct { VkRenderPass handles[MAIN_RENDER_PASS_STENCIL_COUNT]; uint32_t attachment_count; bool unanchored_layout_transition;
                 VkFramebuffer *framebuffers; uint32_t framebuffer_count; } physical_pass_t;
static frame_layout_t current_layout;
static physical_pass_t physical_passes[MAIN_RENDER_PASS_VARIANT_COUNT][2];
static struct { cb_context_t primary_cb_contexts[PCBX_NUM]; VkImage color_buffers[1]; } vulkan_globals;
static bool ready;
static int recorded;
static VkRenderPass rc_experimental_pass;
static VkFramebuffer rc_experimental_framebuffer, rc_experimental_model_framebuffer;
static VkRenderPass rc_experimental_model_pass;
static uint32_t rc_experimental_model_subpass;
static bool model_select_result;
static unsigned model_selections;
static uint32_t r_framecount = 12;
static float receiver_divisor=2;
static bool receiver_checkerboard, receiver_texture_filter, submitted_texture_filter;
static bool receiver_active, submitted_half, submitted_checker, select_result, receiver_soft, submitted_soft;
static int classic_world, selections;
static float submitted_divisor;
static float receiver_shadow_filter, submitted_shadow_filter;
static void R_RadianceRecordReceiverUse(float divisor, bool checker, bool texture_filter) { submitted_divisor=divisor; submitted_half=divisor>1; submitted_checker=checker; submitted_texture_filter=texture_filter; }
static void R_RadianceRecordReceiverSoftUse(bool active) { submitted_soft=active; }
static void R_RadianceRecordReceiverShadowFilterUse(float strength) { submitted_shadow_filter=strength; }
static float RCReceiverFrame_ActiveShadowFilter(uint64_t frame) { assert(frame==r_framecount);return receiver_shadow_filter; }
static bool RCReceiverFrame_ActiveSoftShadows(uint64_t frame) { assert(frame==r_framecount); return receiver_soft; }
static uint64_t RCReceiverFrame_CompletedSerial(uint32_t frame) { return frame; }
static bool RCReceiverFrame_Active(uint64_t frame) { assert(frame==r_framecount); return receiver_active; }
static float RCReceiverFrame_ActiveDivisor(uint64_t frame) { assert(frame==r_framecount); return receiver_divisor; }
static bool RCReceiverFrame_ActiveCheckerboard(uint64_t frame) { assert(frame==r_framecount); return receiver_checkerboard; }
static uint32_t RCReceiverFrame_ActiveFilterMode(uint64_t frame) { assert(frame==r_framecount); return receiver_texture_filter ? 1 : 0; }
static bool RCReceiverSelect_Submit(VkCommandBuffer cb, uint64_t frame, bool active, VkRenderPass pass, VkFramebuffer fb, uint32_t subpass) {
    (void)cb; (void)pass; (void)fb; (void)subpass; assert(frame==r_framecount); ++selections; return active && select_result;
}
static bool RCExperimentalSubmitModels(VkCommandBuffer cb,uint64_t serial,VkRenderPass pass,VkFramebuffer fb,uint32_t subpass) {
 (void)cb;(void)pass;(void)fb;(void)subpass;assert(serial==r_framecount);++model_selections;return receiver_active && model_select_result;
}
static bool GL_RadianceCommandsReady(void) { return ready; }
static bool Sky_NeedStencil(void) { return false; }
static void Sys_Error(const char *format, ...) { (void)format; abort(); }
static void R_SubmitContexts(VkCommandBuffer cb, int first, int last) { (void)cb; (void)first; (void)last; ++classic_world; }
static void R_ScreenEffects(cb_context_t *cb, bool enabled, end_rendering_parms_t *p) { (void)cb; (void)enabled; (void)p; abort(); }
static void MarkRecorded(cb_context_t *cb) { assert(cb == &vulkan_globals.primary_cb_contexts[PCBX_RENDER_PASSES]); ++recorded; }
static unsigned scene_barriers;
static bool expect_unanchored_transition;
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t memories, const VkMemoryBarrier *memory, uint32_t buffers,
    const VkBufferMemoryBarrier *buffer, uint32_t images, const VkImageMemoryBarrier *image) {
    (void)cb; (void)flags; (void)buffer; (void)image;
    assert(src & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    assert(dst == (expect_unanchored_transition ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
    assert(memories == 1 && !buffers && !images);
    assert(memory->srcAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
    assert(memory->dstAccessMask == (expect_unanchored_transition ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT :
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));
    ++scene_barriers;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *begin, VkSubpassContents contents) {
    (void)cb; assert(begin->renderPass && begin->framebuffer && contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer cb) { (void)cb; }
'''
suffix = r'''
int main(void) {
    for (uintptr_t i = 0; i < PCBX_NUM; ++i)
        vulkan_globals.primary_cb_contexts[i].cb = (VkCommandBuffer)(i + 1);
    current_layout.samples = VK_SAMPLE_COUNT_1_BIT;
    frame_desc_t *frame = &current_layout.variants[MAIN_RENDER_PASS_STANDARD];
    frame->step_count = 4;
    frame->passes[0].target = FRAME_TARGET_SCENE;
    frame->steps[0] = (frame_step_t){.type=FRAME_PREPARED_COMMANDS, .first_context=PCBX_FIRST};
    frame->steps[1] = (frame_step_t){.type=FRAME_PREPARED_COMMANDS, .first_context=PCBX_RADIANCE};
    frame->steps[2] = (frame_step_t){.type=FRAME_PREPARED_COMMANDS, .first_context=PCBX_LAST};
    frame->steps[3] = (frame_step_t){.type=FRAME_RECORD_WORK, .recorder=MarkRecorded};
    end_rendering_parms_t parms = {0};
    VkCommandBuffer submit[PCBX_NUM] = {0};
    ready = false;
    uint32_t n = R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(n == 3 && recorded == 1);
    assert(submit[0] == (VkCommandBuffer)(uintptr_t)1);
    assert(submit[1] == (VkCommandBuffer)(uintptr_t)3);
    assert(submit[2] == (VkCommandBuffer)(uintptr_t)4);
    ready = true;
    n = R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(n == 4 && recorded == 2);
    for (uintptr_t i = 0; i < 4; ++i) assert(submit[i] == (VkCommandBuffer)(i + 1));
    /* Missing a swapchain image must not suppress non-UI prepared work. */
    n = R_RecordFrame(&parms, false, 0, submit, PCBX_NUM, NULL, NULL);
    assert(n == 4 && recorded == 3);
    frame->step_count = 1;
    frame->steps[0] = (frame_step_t){.type=FRAME_GRAPHICS_WORK, .draw_stage=DRAW_WORLD};
    select_result = true; receiver_active = false;
    R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(classic_world == 1 && selections == 1 && !submitted_half && !submitted_checker);
    receiver_shadow_filter=1.5f; receiver_soft = true; receiver_active = true; receiver_checkerboard = true; receiver_texture_filter = true; receiver_divisor = 4;
    R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(classic_world == 1 && selections == 2 && submitted_half && submitted_checker && submitted_divisor == 4 && submitted_texture_filter && submitted_soft);
    assert(submitted_shadow_filter==1.5f);
    receiver_divisor = 1.5f;
    R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(classic_world == 1 && selections == 3 && submitted_half && submitted_checker && submitted_divisor == 1.5f && submitted_texture_filter && submitted_soft);
    select_result = false;
    R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(classic_world == 2 && selections == 4 && !submitted_half && !submitted_checker && submitted_divisor == 1 && !submitted_texture_filter && !submitted_soft);
    assert(submitted_shadow_filter==0);
    /* A model-only receiver submission is still active if the world takes
       its forward fallback, and full1 texture reuse must report truthfully. */
    frame->step_count=2;frame->steps[1]=(frame_step_t){.type=FRAME_GRAPHICS_WORK,.draw_stage=DRAW_MODELS,.pass=1};
    receiver_active=true;model_select_result=true;receiver_divisor=1;receiver_checkerboard=false;
    R_RecordFrame(&parms,true,0,submit,PCBX_NUM,NULL,NULL);
    assert(model_selections==1 && !submitted_half && submitted_divisor==1 && submitted_texture_filter);
    assert(submitted_shadow_filter==1.5f);
    model_select_result=false;
    R_RecordFrame(&parms,true,0,submit,PCBX_NUM,NULL,NULL);
    assert(model_selections==2 && !submitted_texture_filter && !submitted_soft);
    assert(submitted_shadow_filter==0);
    /* An unused MSAA resolve attachment can transition only at pass end;
       COLOR_ATTACHMENT_OUTPUT alone does not cover that implicit write. */
    current_layout.samples = VK_SAMPLE_COUNT_4_BIT;
    VkFramebuffer framebuffer = (VkFramebuffer)(uintptr_t)1;
    physical_pass_t *physical = &physical_passes[MAIN_RENDER_PASS_STANDARD][0];
    physical->handles[0] = physical->handles[1] = (VkRenderPass)(uintptr_t)1;
    physical->framebuffers = &framebuffer; physical->framebuffer_count = 1; physical->attachment_count = 3;
    frame->step_count = 2;
    frame->steps[0] = (frame_step_t){.type=FRAME_BEGIN_GRAPHICS, .pass=0};
    frame->steps[1] = (frame_step_t){.type=FRAME_END_GRAPHICS, .pass=0};
    physical->unanchored_layout_transition = expect_unanchored_transition = true;
    R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(scene_barriers == 1);
    physical->unanchored_layout_transition = expect_unanchored_transition = false;
    current_layout.samples = VK_SAMPLE_COUNT_1_BIT;
    R_RecordFrame(&parms, true, 0, submit, PCBX_NUM, NULL, NULL);
    assert(scene_barriers == 2);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='radiance-frame-submit-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prefix + recorder + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'Quake'), str(path / 'test.c'), '-lvulkan', '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: optional radiance command buffer submission and classic fallback')
