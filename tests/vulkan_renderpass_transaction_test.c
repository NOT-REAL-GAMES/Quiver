/* Linux fault-injection test of the actual render-pass transaction. */
#include "../Quake/r_passes.c"
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

vulkanglobals_t vulkan_globals;
viddef_t vid;
cvar_t r_ssao;
static unsigned step, fail_at, serial, live;
static unsigned char live_pass[512];
void Sys_Error (const char *fmt, ...) { (void)fmt; abort(); }
void Con_Printf (const char *fmt, ...) { (void)fmt; }
void Cvar_SetValueQuick (cvar_t *var, float value) { var->value = value; }
void GL_SetObjectName (uint64_t object, VkObjectType type, const char *name) { (void)object; (void)type; (void)name; }
void R_RecordECSVisualCull (VkCommandBuffer commands) { (void)commands; }
int r_framecount;
uint32_t GL_RenderFrameSlot (void) { return 0; }
void RCExperimentalCaptureFinish (VkCommandBuffer c, VkRenderPass p, VkFramebuffer f, uint32_t s, VkRenderPass mp, VkFramebuffer mf, uint32_t ms) { (void)c; (void)p; (void)f; (void)s; (void)mp; (void)mf; (void)ms; }
const rc_receiver_targets_t *RCReceiverFrame_PendingTargets(uint64_t s) { (void)s; return NULL; }
bool RCReceiverLive_CanUse(uint32_t s,const rc_receiver_targets_t *t) { (void)s; (void)t; return false; }
void RCReceiverFrame_Stage(const rc_receiver_frame_plan_t *p) { (void)p; }
bool RCReceiverFrame_Record(VkCommandBuffer c,uint64_t s) { (void)c; (void)s; return false; }
const rc_receiver_targets_t *RCReceiverFrame_ActiveTargets(uint64_t s) { (void)s; return NULL; }
bool RCReceiverLive_MarkUsed(uint32_t s,const rc_receiver_targets_t *t) { (void)s; (void)t; return false; }

void R_PrepareSSAOWorldDepth (cb_context_t *cbx) { (void)cbx; }
void R_ComputeSSAO (cb_context_t *cbx) { (void)cbx; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass (VkDevice device, const VkRenderPassCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkRenderPass *out)
{
	(void)device; (void)allocator; assert (info->attachmentCount && info->subpassCount && info->pAttachments && info->pSubpasses);
	if (++step == fail_at) { *out = (VkRenderPass)(uintptr_t)511; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
	unsigned id = ++serial; assert (id < 511 && !live_pass[id]); live_pass[id] = 1; ++live;
	*out = (VkRenderPass)(uintptr_t)id; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass (VkDevice device, VkRenderPass pass, const VkAllocationCallbacks *allocator)
{
	(void)device; (void)allocator; unsigned id = (unsigned)(uintptr_t)pass;
	assert (id && id < 511 && live_pass[id]); live_pass[id] = 0; --live;
}
static void finish(bool had_warp)
{
	R_DestroyRenderPasses ();
	if (!had_warp) { vkDestroyRenderPass (vulkan_globals.device, vulkan_globals.warp_render_pass, NULL); vulkan_globals.warp_render_pass = VK_NULL_HANDLE; }
	assert (live == (unsigned)had_warp);
}
static void check_unanchored_transitions(void)
{
	frame_desc_t *frame = &current_layout.variants[MAIN_RENDER_PASS_STANDARD];
	*frame = (frame_desc_t){.pass_count=1,.passes={{.target=FRAME_TARGET_SCENE,.subpass_count=1,.subpasses={SUBPASS_MAIN}}}};
	VkAttachmentDescription attachments[2]={
		{.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
		{.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
	VkAttachmentReference colors[2]={{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
	uint32_t preserve=1;
	VkSubpassDescription stages[SUBPASS_COUNT]={0};
	stages[SUBPASS_MAIN]=(VkSubpassDescription){.colorAttachmentCount=1,.pColorAttachments=colors,
		.preserveAttachmentCount=1,.pPreserveAttachments=&preserve};
	assert(R_CreateGraphicsPasses(MAIN_RENDER_PASS_STANDARD,FRAME_TARGET_SCENE,attachments,2,stages)==VK_SUCCESS);
	assert(physical_passes[MAIN_RENDER_PASS_STANDARD][0].unanchored_layout_transition);
	R_DestroyRenderPasses(); assert(!live);
	attachments[1].initialLayout=attachments[1].finalLayout;
	assert(R_CreateGraphicsPasses(MAIN_RENDER_PASS_STANDARD,FRAME_TARGET_SCENE,attachments,2,stages)==VK_SUCCESS);
	assert(!physical_passes[MAIN_RENDER_PASS_STANDARD][0].unanchored_layout_transition);
	R_DestroyRenderPasses(); assert(!live);
	/* A genuine use anchors the transition, and a later pass's adjusted
	 * initial layout must eliminate an otherwise unused transition. */
	attachments[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
	stages[SUBPASS_MAIN].colorAttachmentCount=2; stages[SUBPASS_MAIN].preserveAttachmentCount=0;
	stages[SUBPASS_ENTITY_SSAO]=(VkSubpassDescription){.colorAttachmentCount=1,.pColorAttachments=colors};
	frame->pass_count=2;
	frame->passes[1]=(graphics_pass_desc_t){.target=FRAME_TARGET_SCENE,.subpass_count=1,.subpasses={SUBPASS_ENTITY_SSAO}};
	assert(R_CreateGraphicsPasses(MAIN_RENDER_PASS_STANDARD,FRAME_TARGET_SCENE,attachments,2,stages)==VK_SUCCESS);
	assert(!physical_passes[MAIN_RENDER_PASS_STANDARD][0].unanchored_layout_transition);
	assert(!physical_passes[MAIN_RENDER_PASS_STANDARD][1].unanchored_layout_transition);
	R_DestroyRenderPasses(); assert(!live);
	step=serial=0;
}
int main(void)
{
	for (int i = 0; i < SCBX_NUM; ++i)
	{
		vulkan_globals.secondary_cb_contexts[i] = calloc (SECONDARY_CB_MULTIPLICITY[i], sizeof (cb_context_t));
		assert (vulkan_globals.secondary_cb_contexts[i]);
	}
	check_unanchored_transitions();
	vid.width = 640; vid.height = 360; vid.render_width = 320; vid.render_height = 200;
	vulkan_globals.color_format = VK_FORMAT_R8G8B8A8_UNORM;
	vulkan_globals.depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
	vulkan_globals.swap_chain_format = VK_FORMAT_R8G8B8A8_UNORM;
	vulkan_globals.sample_count = VK_SAMPLE_COUNT_4_BIT;
	/* Determine the real number of pass creations, including the warp pass. */
	assert (R_TryCreateRenderPasses () == VK_SUCCESS); unsigned total = step;
	assert (total > 1 && live == total); finish(false);
	for (unsigned existing = 0; existing < 2; ++existing)
	{
		const unsigned failures = total - existing;
		for (fail_at = 1; fail_at <= failures; ++fail_at)
		{
			step = serial = 0;
			if (existing) { live_pass[510] = 1; live = 1; vulkan_globals.warp_render_pass = (VkRenderPass)(uintptr_t)510; }
			frame_layout_t old_current = current_layout, old_pending = pending_layout;
			physical_pass_t old_physical[MAIN_RENDER_PASS_VARIANT_COUNT][MAX_FRAME_PASSES];
			context_binding_t old_contexts[MAIN_RENDER_PASS_VARIANT_COUNT][SCBX_NUM];
			memcpy (old_physical, physical_passes, sizeof (old_physical)); memcpy (old_contexts, context_bindings, sizeof (old_contexts));
			VkRenderPass old_warp = vulkan_globals.warp_render_pass;
			assert (R_TryCreateRenderPasses () == VK_ERROR_OUT_OF_DEVICE_MEMORY);
			assert (!memcmp (&current_layout, &old_current, sizeof (old_current)) && !memcmp (&pending_layout, &old_pending, sizeof (old_pending)));
			assert (!memcmp (physical_passes, old_physical, sizeof (old_physical)) && !memcmp (context_bindings, old_contexts, sizeof (old_contexts)));
			assert (vulkan_globals.warp_render_pass == old_warp && live == existing);
			step = serial = 0; unsigned saved = fail_at; fail_at = 0;
			assert (R_TryCreateRenderPasses () == VK_SUCCESS);
			assert (R_TryCreateRenderPasses () == VK_ERROR_INITIALIZATION_FAILED);
			finish(existing != 0);
			if (existing) { vkDestroyRenderPass (vulkan_globals.device, old_warp, NULL); vulkan_globals.warp_render_pass = VK_NULL_HANDLE; }
			assert (!live); fail_at = saved;
		}
	}
	/* Captured opaque models must inherit the actual entity pass, including
	 * its split SSAO segment rather than the first world's pass. */
	vulkan_globals.screen_effects_sops=true;
	vulkan_globals.device_features.shaderStorageImageExtendedFormats=true;
	r_ssao.value=1;step=serial=0;fail_at=0;
	assert(R_TryCreateRenderPasses()==VK_SUCCESS);
	for(unsigned variant=0;variant<MAIN_RENDER_PASS_VARIANT_COUNT;++variant) {
		assert(context_bindings[variant][SCBX_MODELS].pass==context_bindings[variant][SCBX_ENTITIES].pass);
		assert(context_bindings[variant][SCBX_MODELS].pass!=context_bindings[variant][SCBX_WORLD].pass);
		assert(context_bindings[variant][SCBX_MODELS].type==SUBPASS_MAIN);
		assert(context_bindings[variant][SCBX_RECEIVER_BRUSHES].pass==context_bindings[variant][SCBX_ENTITIES].pass);
		assert(context_bindings[variant][SCBX_RECEIVER_BRUSHES].type==SUBPASS_MAIN);
	}
	finish(false);
	puts ("PASS: render-pass failures restore layouts, bindings and persistent warp ownership and allow retry");
	for (int i = 0; i < SCBX_NUM; ++i) free (vulkan_globals.secondary_cb_contexts[i]);
	return 0;
}
