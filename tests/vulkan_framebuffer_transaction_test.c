/* Linux fault-injection test of the real framebuffer transaction. */
#include <stdlib.h>
#include <stdint.h>
static void *test_calloc(size_t count, size_t size);
#define calloc test_calloc
#include "../Quake/r_passes.c"
#undef calloc
#include <assert.h>

vulkanglobals_t vulkan_globals;
oit_mode_t frame_oit_mode;
static unsigned step, fail_at, serial, live;
static unsigned char live_fb[64];
static void *test_calloc(size_t count, size_t size)
{ if (++step == fail_at) return NULL; return calloc(count, size); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer (VkDevice device, const VkFramebufferCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkFramebuffer *out)
{
	(void)device; (void)allocator;
	assert (info->renderPass && info->attachmentCount && info->pAttachments && info->width && info->height);
	if (++step == fail_at) { *out = (VkFramebuffer)(uintptr_t)63; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
	unsigned id = ++serial; assert (id < 63 && !live_fb[id]); live_fb[id] = 1; ++live;
	*out = (VkFramebuffer)(uintptr_t)id; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer (VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *allocator)
{
	(void)device; (void)allocator; unsigned id = (unsigned)(uintptr_t)fb;
	assert (id && id < 63 && live_fb[id]); live_fb[id] = 0; --live;
}
static void setup(main_render_pass_variant_t variant)
{
	memset (&current_layout, 0, sizeof (current_layout));
	current_layout.samples = VK_SAMPLE_COUNT_4_BIT;
	frame_desc_t *frame = &current_layout.variants[variant];
	frame->pass_count = 2; frame->passes[0].target = FRAME_TARGET_SCENE; frame->passes[1].target = FRAME_TARGET_UI;
	physical_passes[variant][0].attachment_count = variant == MAIN_RENDER_PASS_MBOIT ? 6 : variant == MAIN_RENDER_PASS_OIT ? 5 : 3;
	physical_passes[variant][1].attachment_count = 3;
	for (unsigned i = 0; i < 2; ++i) physical_passes[variant][i].handles[MAIN_RENDER_PASS_STENCIL_CLEAR] = (VkRenderPass)(uintptr_t)1;
}
int main(void)
{
	VkImageView swaps[3] = {(VkImageView)(uintptr_t)1, (VkImageView)(uintptr_t)2, (VkImageView)(uintptr_t)3};
	render_framebuffer_images_t images = {.width = 640, .height = 360, .render_width = 320, .render_height = 200,
		.swapchain_count = 3, .swapchain = swaps};
	for (int mode = OIT_MODE_NONE; mode <= OIT_MODE_MBOIT; ++mode)
	{
		frame_oit_mode = (oit_mode_t)mode;
		main_render_pass_variant_t variant = mode == OIT_MODE_MBOIT ? MAIN_RENDER_PASS_MBOIT : mode == OIT_MODE_WBOIT ? MAIN_RENDER_PASS_OIT : MAIN_RENDER_PASS_STANDARD;
		setup(variant);
		/* Two calloc calls and five framebuffer creations. */
		for (fail_at = 1; fail_at <= 7; ++fail_at)
		{
			step = serial = 0; assert (R_TryCreateFrameBuffers (&images) != VK_SUCCESS); assert (!live);
			for (unsigned p = 0; p < 2; ++p) assert (!physical_passes[variant][p].framebuffers && !physical_passes[variant][p].framebuffer_count);
			step = serial = 0; unsigned saved = fail_at; fail_at = 0;
			assert (R_TryCreateFrameBuffers (&images) == VK_SUCCESS && live == 5);
			assert (R_TryCreateFrameBuffers (&images) == VK_ERROR_INITIALIZATION_FAILED && live == 5);
			R_DestroyFrameBuffers (); assert (!live); fail_at = saved;
		}
		step = serial = fail_at = 0;
		images.swapchain = NULL;
		assert (R_TryCreateFrameBuffers (&images) == VK_ERROR_INITIALIZATION_FAILED && !live);
		for (unsigned p = 0; p < 2; ++p) assert (!physical_passes[variant][p].framebuffers && !physical_passes[variant][p].framebuffer_count);
		images.swapchain = swaps;
	}
	puts("PASS: framebuffer failures roll back all resources and allow retry in every transparency mode");
	return 0;
}
