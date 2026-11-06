/* Linux standalone fault-injection test of the actual scene/UI target helpers.
 * Build with function/data sections and --gc-sections; see
 * vulkan_color_target_test.md. No Vulkan device or game data is needed. */
#include "../Quake/gl_vidsdl.c"
#include <assert.h>
#include <stdint.h>

vulkanglobals_t vulkan_globals;
viddef_t vid;
cvar_t r_ssao;
oit_mode_t frame_oit_mode;
gltexture_t *bluenoisetexture;
atomic_uint32_t num_vulkan_misc_allocations;
static unsigned step, fail_at, images, memories, views, serial;
static unsigned descriptors;
static bool ssao_live, passes_live, framebuffers_live, pipelines_live;
static unsigned finalizations;
static unsigned char live_descriptor[128];
static unsigned char live_image[128], live_memory[128], live_view[128];
static int fault(void) { return ++step == fail_at; }
#define ID(h) ((unsigned)(uintptr_t)(h))
#define HANDLE(t, n) ((t)(uintptr_t)(n))
void Sys_Printf (const char *fmt, ...) { (void)fmt; }
void Con_Printf (const char *fmt, ...) { (void)fmt; }
VkResult R_TryAllocateDescriptorSet (vulkan_desc_set_layout_t *layout, VkDescriptorSet *out)
{
	(void)layout; assert (!*out);
	if (fault ()) return VK_ERROR_OUT_OF_POOL_MEMORY;
	unsigned id = ++serial; assert (id < 128 && !live_descriptor[id]);
	live_descriptor[id] = 1; ++descriptors; *out = HANDLE (VkDescriptorSet, id); return VK_SUCCESS;
}
void R_FreeDescriptorSet (VkDescriptorSet set, vulkan_desc_set_layout_t *layout)
{
	(void)layout; unsigned id = ID (set); assert (id && id < 128 && live_descriptor[id]);
	live_descriptor[id] = 0; --descriptors;
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets (VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
	uint32_t copies, const VkCopyDescriptorSet *copy)
{ (void)device; (void)copies; (void)copy; for (uint32_t i = 0; i < count; ++i) assert (live_descriptor[ID (writes[i].dstSet)]); }
VkResult R_TryCreateSSAO (VkImage depth)
{ assert (depth && !ssao_live); if (fault ()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; ssao_live = true; return VK_SUCCESS; }
void R_FinalizeSSAO (void)
{ assert (ssao_live && passes_live && framebuffers_live && pipelines_live && render_resources_created); ++finalizations; }
void R_DestroySSAO (void) { ssao_live = false; }
VkResult R_TryCreateRenderPasses (void)
{ assert (ssao_live && !passes_live); if (fault ()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; passes_live = true; return VK_SUCCESS; }
void R_DestroyRenderPasses (void) { assert (passes_live); passes_live = false; }
VkResult R_TryCreateFrameBuffers (const render_framebuffer_images_t *info)
{ assert (info && passes_live && !framebuffers_live); if (fault ()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; framebuffers_live = true; return VK_SUCCESS; }
void R_DestroyFrameBuffers (void) { framebuffers_live = false; }
bool R_TryCreatePipelines (void)
{ assert (framebuffers_live && !pipelines_live); if (fault ()) return false; pipelines_live = true; return true; }
void R_DestroyPipelines (void) { assert (pipelines_live); pipelines_live = false; }
char *va (const char *fmt, ...) { (void)fmt; return "test"; }
int GL_TryMemoryTypeFromProperties (uint32_t bits, VkFlags required, VkFlags preferred)
{ (void)bits; (void)required; (void)preferred; return fault() ? -1 : 0; }
VkResult R_TryAllocateVulkanMemory (vulkan_memory_t *memory, const VkMemoryAllocateInfo *info,
	vulkan_memory_type_t type, atomic_uint32_t *counter)
{
	(void)counter;
	if (fault()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	unsigned id = ++serial; assert (id < 128 && !live_memory[id]);
	live_memory[id] = 1; ++memories;
	memory->handle = HANDLE(VkDeviceMemory, id); memory->type = type; memory->size = info->allocationSize;
	return VK_SUCCESS;
}
void R_FreeVulkanMemory (vulkan_memory_t *memory, atomic_uint32_t *counter)
{
	(void)counter; unsigned id = ID(memory->handle); assert (id && id < 128 && live_memory[id]);
	live_memory[id] = 0; --memories; memset (memory, 0, sizeof (*memory));
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage (VkDevice device, const VkImageCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkImage *image)
{
	(void)device; (void)info; (void)allocator;
	if (fault()) { *image = HANDLE(VkImage, 127); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
	unsigned id = ++serial; assert (id < 128 && !live_image[id]); live_image[id] = 1; ++images;
	*image = HANDLE(VkImage, id); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage (VkDevice device, VkImage image, const VkAllocationCallbacks *allocator)
{
	(void)device; (void)allocator; unsigned id = ID(image); assert (id && id < 128 && live_image[id]);
	live_image[id] = 0; --images;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements (VkDevice device, VkImage image, VkMemoryRequirements *requirements)
{ (void)device; assert (live_image[ID(image)]); *requirements = (VkMemoryRequirements){.size = 4096, .alignment = 64, .memoryTypeBits = 1}; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory (VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
{ (void)device; (void)offset; assert (live_image[ID(image)] && live_memory[ID(memory)]); return fault() ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView (VkDevice device, const VkImageViewCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkImageView *view)
{
	(void)device; (void)allocator; assert (live_image[ID(info->image)]);
	if (fault()) { *view = HANDLE(VkImageView, 127); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
	unsigned id = ++serial; assert (id < 128 && !live_view[id]); live_view[id] = 1; ++views;
	*view = HANDLE(VkImageView, id); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView (VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator)
{
	(void)device; (void)allocator; unsigned id = ID(view); assert (id && id < 128 && live_view[id]);
	live_view[id] = 0; --views;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties (VkPhysicalDevice physical, VkFormat format,
	VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *properties)
{
	(void)physical; (void)format; (void)type; (void)tiling; (void)usage; (void)flags;
	memset (properties, 0, sizeof (*properties)); properties->sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT; return VK_SUCCESS;
}
static void clean(void) { assert (!images && !memories && !views); }
static void clean_scene(void)
{
	assert (!render_resources_created && !ssao_live && !passes_live && !framebuffers_live && !pipelines_live && !descriptors);
	clean ();
}
static void destroy_scene(void)
{
	render_resources_created = false;
	R_DestroyPipelines (); R_DestroyFrameBuffers (); R_DestroyRenderPasses (); R_DestroySSAO ();
	if (vulkan_globals.particle_depth_descriptor_set) R_FreeDescriptorSet (vulkan_globals.particle_depth_descriptor_set, &vulkan_globals.input_attachment_set_layout);
	if (scene_upscale_descriptor_set) R_FreeDescriptorSet (scene_upscale_descriptor_set, &vulkan_globals.single_texture_set_layout);
	if (postprocess_descriptor_set) R_FreeDescriptorSet (postprocess_descriptor_set, &vulkan_globals.input_attachment_set_layout);
	if (wboit_resolve_descriptor_set) R_FreeDescriptorSet (wboit_resolve_descriptor_set, &vulkan_globals.oit_input_attachment_set_layout);
	if (vulkan_globals.mboit_input_attachment_descriptor_set) R_FreeDescriptorSet (vulkan_globals.mboit_input_attachment_descriptor_set, &vulkan_globals.mboit_input_attachment_set_layout);
	if (vulkan_globals.screen_effects_desc_set) R_FreeDescriptorSet (vulkan_globals.screen_effects_desc_set, &vulkan_globals.screen_effects_set_layout);
#if defined(_DEBUG)
	if (vulkan_globals.ray_debug_desc_set) R_FreeDescriptorSet (vulkan_globals.ray_debug_desc_set, &vulkan_globals.ray_debug_set_layout);
	vulkan_globals.ray_debug_desc_set = VK_NULL_HANDLE;
#endif
	vulkan_globals.particle_depth_descriptor_set = scene_upscale_descriptor_set = postprocess_descriptor_set = wboit_resolve_descriptor_set = VK_NULL_HANDLE;
	vulkan_globals.mboit_input_attachment_descriptor_set = vulkan_globals.screen_effects_desc_set = VK_NULL_HANDLE;
	GL_DestroyAttachment (&ui_depth_buffer, &ui_depth_buffer_memory, &ui_depth_buffer_view);
	GL_DestroyAttachment (&depth_buffer, &depth_buffer_memory, &depth_buffer_view);
	GL_DestroyUIColorBuffer (); GL_DestroyOITBuffers (); GL_DestroyColorBuffers (); clean_scene ();
}
int main(void)
{
	vid.width = vid.render_width = 640; vid.height = vid.render_height = 360;
	vulkan_globals.color_format = VK_FORMAT_R8G8B8A8_UNORM; vid_fsaa.value = 4;
	/* Each image uses five failure sites: image, memory type, allocation,
	 * bind and view. This includes every scene buffer and the MSAA image. */
	for (fail_at = 1; fail_at <= 5 * (NUM_COLOR_BUFFERS + 1); ++fail_at)
	{
		step = serial = 0; assert (GL_TryCreateColorBuffer () != VK_SUCCESS); clean();
		step = serial = 0; unsigned saved = fail_at; fail_at = 0;
		assert (GL_TryCreateColorBuffer () == VK_SUCCESS);
		assert (GL_TryCreateColorBuffer () == VK_ERROR_INITIALIZATION_FAILED);
		GL_DestroyColorBuffers (); clean();
		fail_at = saved;
	}
	for (fail_at = 1; fail_at <= 5; ++fail_at)
	{
		step = serial = 0; assert (GL_TryCreateUIColorBuffer () != VK_SUCCESS); clean();
		step = serial = 0; unsigned saved = fail_at; fail_at = 0;
		assert (GL_TryCreateUIColorBuffer () == VK_SUCCESS);
		assert (GL_TryCreateUIColorBuffer () == VK_ERROR_INITIALIZATION_FAILED);
		GL_DestroyUIColorBuffer (); clean();
		fail_at = saved;
	}
	for (int depth = 0; depth < 2; ++depth)
		for (fail_at = 1; fail_at <= 5; ++fail_at)
		{
			step = serial = 0;
			assert ((depth ? GL_TryCreateUIDepthBuffer () : GL_TryCreateDepthImage ()) != VK_SUCCESS); clean ();
			step = serial = 0; unsigned saved = fail_at; fail_at = 0;
			assert ((depth ? GL_TryCreateUIDepthBuffer () : GL_TryCreateDepthImage ()) == VK_SUCCESS);
			assert ((depth ? GL_TryCreateUIDepthBuffer () : GL_TryCreateDepthImage ()) == VK_ERROR_INITIALIZATION_FAILED);
			if (depth) GL_DestroyAttachment (&ui_depth_buffer, &ui_depth_buffer_memory, &ui_depth_buffer_view);
			else GL_DestroyAttachment (&depth_buffer, &depth_buffer_memory, &depth_buffer_view);
			clean (); fail_at = saved;
		}
	for (int mode = OIT_MODE_WBOIT; mode <= OIT_MODE_MBOIT; ++mode)
	{
		frame_oit_mode = mode;
		for (fail_at = 1; fail_at <= 5u * (mode == OIT_MODE_WBOIT ? 2u : 3u); ++fail_at)
		{
			step = serial = 0; assert (GL_TryCreateOITBuffers () != VK_SUCCESS); clean ();
			step = serial = 0; unsigned saved = fail_at; fail_at = 0;
			assert (GL_TryCreateOITBuffers () == VK_SUCCESS);
			assert (GL_TryCreateOITBuffers () == VK_ERROR_INITIALIZATION_FAILED);
			GL_DestroyOITBuffers (); clean (); fail_at = saved;
		}
	}
	for (fail_at = 1; fail_at <= 6; ++fail_at)
	{
		step = serial = 0; assert (GL_TryCreateDepthBuffer () != VK_SUCCESS); clean (); assert (!descriptors);
		step = serial = 0; unsigned saved = fail_at; fail_at = 0;
		assert (GL_TryCreateDepthBuffer () == VK_SUCCESS);
		assert (GL_TryCreateDepthBuffer () == VK_ERROR_INITIALIZATION_FAILED);
		R_FreeDescriptorSet (vulkan_globals.particle_depth_descriptor_set, &vulkan_globals.input_attachment_set_layout);
		vulkan_globals.particle_depth_descriptor_set = VK_NULL_HANDLE;
		GL_DestroyAttachment (&depth_buffer, &depth_buffer_memory, &depth_buffer_view);
		clean (); assert (!descriptors); fail_at = saved;
	}
	gltexture_t noise = {0}; bluenoisetexture = &noise;
	render_resources_created = true;
	/* Also exercise the debug-only ray-query descriptor on every allocation
	 * failure, including replacement while the old descriptor is live. */
#if defined(_DEBUG)
	for (int ray = 0; ray < 2; ++ray)
#else
	for (int ray = 0; ray < 1; ++ray)
#endif
	for (int ui = 0; ui < 2; ++ui)
		for (int mode = OIT_MODE_NONE; mode <= OIT_MODE_MBOIT; ++mode)
		{
			vulkan_globals.ray_query = ray;
			frame_oit_mode = mode; ui_color_buffer = ui ? HANDLE (VkImage, 127) : VK_NULL_HANDLE;
			unsigned allocations = 2 + ui + (mode != OIT_MODE_NONE) + ray;
			for (unsigned failure = 1; failure <= allocations; ++failure)
			{
				step = serial = 0; fail_at = failure;
				assert (GL_TryUpdateDescriptorSets () == VK_ERROR_OUT_OF_POOL_MEMORY);
				assert (!descriptors && !scene_upscale_descriptor_set && !postprocess_descriptor_set &&
					!wboit_resolve_descriptor_set && !vulkan_globals.mboit_input_attachment_descriptor_set && !vulkan_globals.screen_effects_desc_set);
#if defined(_DEBUG)
				assert (!vulkan_globals.ray_debug_desc_set);
#endif
				step = serial = fail_at = 0; assert (GL_TryUpdateDescriptorSets () == VK_SUCCESS);
				VkDescriptorSet upscale = scene_upscale_descriptor_set, post = postprocess_descriptor_set;
				VkDescriptorSet wboit = wboit_resolve_descriptor_set, mboit = vulkan_globals.mboit_input_attachment_descriptor_set;
				VkDescriptorSet effects = vulkan_globals.screen_effects_desc_set;
#if defined(_DEBUG)
				VkDescriptorSet ray_debug = vulkan_globals.ray_debug_desc_set;
#endif
				step = 0; fail_at = failure;
				assert (GL_TryUpdateDescriptorSets () == VK_ERROR_OUT_OF_POOL_MEMORY);
				assert (scene_upscale_descriptor_set == upscale && postprocess_descriptor_set == post);
				assert (wboit_resolve_descriptor_set == wboit && vulkan_globals.mboit_input_attachment_descriptor_set == mboit);
				assert (vulkan_globals.screen_effects_desc_set == effects && descriptors == allocations);
#if defined(_DEBUG)
				assert (vulkan_globals.ray_debug_desc_set == ray_debug);
#endif
				step = fail_at = 0; assert (GL_TryUpdateDescriptorSets () == VK_SUCCESS); assert (descriptors == allocations);
				if (scene_upscale_descriptor_set) R_FreeDescriptorSet (scene_upscale_descriptor_set, &vulkan_globals.single_texture_set_layout);
				if (postprocess_descriptor_set) R_FreeDescriptorSet (postprocess_descriptor_set, &vulkan_globals.input_attachment_set_layout);
				if (wboit_resolve_descriptor_set) R_FreeDescriptorSet (wboit_resolve_descriptor_set, &vulkan_globals.oit_input_attachment_set_layout);
				if (vulkan_globals.mboit_input_attachment_descriptor_set) R_FreeDescriptorSet (vulkan_globals.mboit_input_attachment_descriptor_set, &vulkan_globals.mboit_input_attachment_set_layout);
				if (vulkan_globals.screen_effects_desc_set) R_FreeDescriptorSet (vulkan_globals.screen_effects_desc_set, &vulkan_globals.screen_effects_set_layout);
#if defined(_DEBUG)
				if (vulkan_globals.ray_debug_desc_set) R_FreeDescriptorSet (vulkan_globals.ray_debug_desc_set, &vulkan_globals.ray_debug_set_layout);
				vulkan_globals.ray_debug_desc_set = VK_NULL_HANDLE;
#endif
				scene_upscale_descriptor_set = postprocess_descriptor_set = wboit_resolve_descriptor_set = VK_NULL_HANDLE;
				vulkan_globals.mboit_input_attachment_descriptor_set = vulkan_globals.screen_effects_desc_set = VK_NULL_HANDLE;
				assert (!descriptors);
			}
		}
	ui_color_buffer = VK_NULL_HANDLE;
	render_resources_created = false;
	/* A preexisting partial resource belongs to the caller and must not be
	 * destroyed by an attempted transaction over occupied slots. */
	depth_buffer = HANDLE (VkImage, 127); step = 0;
	assert (GL_TryCreateSceneResources () == VK_ERROR_INITIALIZATION_FAILED && step == 0 && depth_buffer == HANDLE (VkImage, 127));
	depth_buffer = VK_NULL_HANDLE;
	vid.render_width = 639; /* exercise the native UI target as well */
	for (int mode = OIT_MODE_NONE; mode <= OIT_MODE_MBOIT; ++mode)
	{
		frame_oit_mode = mode;
		unsigned total = 0;
		for (unsigned failure = 1; failure < 128; ++failure)
		{
			step = serial = 0; fail_at = failure; finalizations = 0;
			VkResult result = GL_TryCreateSceneResources ();
			if (result == VK_SUCCESS)
			{
				total = step; assert (finalizations == 1);
				assert (GL_TryCreateSceneResources () == VK_ERROR_INITIALIZATION_FAILED);
				destroy_scene (); break;
			}
			assert (finalizations == 0); clean_scene ();
			step = serial = fail_at = 0;
			assert (GL_TryCreateSceneResources () == VK_SUCCESS && finalizations == 1);
			destroy_scene ();
		}
		assert (total > 20);
	}
	/* Failed FP16 attempts roll back completely, fall back exactly once, and
	 * remain latched to the classic target until explicitly re-enabled. */
	frame_oit_mode = OIT_MODE_WBOIT; vid.render_width = vid.width;
	vulkan_globals.scene_hdr_supported = true; radiance_hdr_failed = false;
	step = serial = fail_at = finalizations = 0;
	assert (GL_TryCreatePreferredSceneResources (true) == VK_SUCCESS && vulkan_globals.scene_hdr_active && ui_color_buffer);
	unsigned hdr_steps = step; destroy_scene ();
	for (unsigned failure = 1; failure <= hdr_steps; ++failure)
	{
		step = serial = finalizations = 0; fail_at = failure; radiance_hdr_failed = false;
		assert (GL_TryCreatePreferredSceneResources (true) == VK_SUCCESS);
		assert (!vulkan_globals.scene_hdr_active && radiance_hdr_failed && finalizations == 1 && vulkan_globals.scene_color_format == vulkan_globals.color_format);
		destroy_scene ();
		step = serial = fail_at = finalizations = 0;
		assert (GL_TryCreatePreferredSceneResources (true) == VK_SUCCESS && !vulkan_globals.scene_hdr_active);
		destroy_scene ();
		step = serial = finalizations = 0;
		assert (GL_TryCreatePreferredSceneResources (false) == VK_SUCCESS && !radiance_hdr_failed);
		destroy_scene ();
	}
	puts ("PASS: color, depth and OIT attachments recover from every image, memory, bind and view failure");
	return 0;
}
