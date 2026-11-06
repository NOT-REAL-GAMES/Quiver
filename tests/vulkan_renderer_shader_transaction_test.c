/* Fault injection through the renderer's real complete shader-module list. */
#include "../Quake/gl_rmisc.c"
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>

cvar_t r_ssao;
static unsigned calls, fail_at, serial, live;
static bool handles[256], optional[256];

void Sys_Printf (const char *format, ...) { (void)format; }
void GL_SetObjectName (uint64_t object, VkObjectType type, const char *name)
{
	(void)object;
	assert (type == VK_OBJECT_TYPE_SHADER_MODULE);
	optional[calls] = !strcmp (name, "meshlet_cull_comp") || !strcmp (name, "alias_ecs_cull_comp") ||
		!strcmp (name, "meshlet_hiz_comp") || !strcmp (name, "world_mesh_mesh") ||
		!strcmp (name, "world_task_task") || !strcmp (name, "world_task_mesh_mesh");
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule (VkDevice device, const VkShaderModuleCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkShaderModule *output)
{
	(void)device; (void)allocator;
	assert (info->codeSize == 4 && info->pCode);
	if (++calls == fail_at)
	{
		/* Vulkan leaves output undefined on failure: never destroy this value. */
		*output = (VkShaderModule)(uintptr_t)255;
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	assert (++serial < 255 && !handles[serial]);
	handles[serial] = true;
	++live;
	*output = (VkShaderModule)(uintptr_t)serial;
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule (VkDevice device, VkShaderModule module,
	const VkAllocationCallbacks *allocator)
{
	(void)device; (void)allocator;
	unsigned id = (unsigned)(uintptr_t)module;
	assert (id && id < 255 && handles[id]);
	handles[id] = false;
	--live;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool (VkDevice device, const VkDescriptorPoolCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkDescriptorPool *output)
{
	(void)device; (void)allocator;
	assert (info->maxSets >= MAX_GLTEXTURES + MAX_SANITY_LIGHTMAPS + 128 + 5);
	assert (info->poolSizeCount == 9);
	assert (info->pPoolSizes[0].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
		info->pPoolSizes[0].descriptorCount >= MIN_NB_DESCRIPTORS_PER_TYPE + MAX_SANITY_LIGHTMAPS * 2 + MAX_GLTEXTURES + 1 + 3);
	assert (info->pPoolSizes[1].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
		info->pPoolSizes[1].descriptorCount >= MIN_NB_DESCRIPTORS_PER_TYPE + MAX_GLTEXTURES + MAX_SANITY_LIGHTMAPS + 2);
	assert (info->pPoolSizes[6].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
		info->pPoolSizes[6].descriptorCount >= MIN_NB_DESCRIPTORS_PER_TYPE + 4);
	*output = (VkDescriptorPool)(uintptr_t)1;
	return VK_SUCCESS;
}

int main (void)
{
	R_CreateDescriptorPool ();
	unsigned maximum = 0;
	for (int msaa = 0; msaa < 2; ++msaa)
	for (int ssao = 0; ssao < 2; ++ssao)
	for (int ray_query = 0; ray_query < 2; ++ray_query)
	for (int fp16 = 0; fp16 < 2; ++fp16)
	for (int hdr = 0; hdr < 2; ++hdr)
	{
		vulkan_globals.sample_count = msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
		vulkan_globals.ray_query = ray_query;
		vulkan_globals.shader_float16 = fp16;
		vulkan_globals.scene_hdr_active = hdr;
		r_ssao.value = ssao;
		vulkan_globals.meshlet_cull_pipeline.layout.handle = (VkPipelineLayout)(uintptr_t)1;
		vulkan_globals.meshlet_hiz_pipeline.layout.handle = (VkPipelineLayout)(uintptr_t)1;
		vulkan_globals.ecs_cull_pipeline.layout.handle = (VkPipelineLayout)(uintptr_t)1;
#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
		vulkan_globals.world_mesh_pipeline_layout.handle = (VkPipelineLayout)(uintptr_t)1;
#if defined(HAVE_WORLD_TASK_SHADER)
		vulkan_globals.world_task_pipeline_layout.handle = (VkPipelineLayout)(uintptr_t)1;
#endif
#endif
		calls = serial = fail_at = 0;
		memset (optional, 0, sizeof (optional));
		assert (R_TryCreateShaderModules ());
		unsigned total = calls;
		if (total > maximum) maximum = total;
		bool optional_calls[256];
		memcpy (optional_calls, optional, sizeof (optional));
		assert (total > 50 && total < 128 && live == total);
		R_DestroyShaderModules ();
		assert (!live);
		for (unsigned failure = 1; failure <= total; ++failure)
		{
			calls = serial = 0; fail_at = failure;
			bool ok = R_TryCreateShaderModules ();
			assert (ok == optional_calls[failure]);
			if (!ok) assert (!live);
			else { assert (live == total - 1); R_DestroyShaderModules (); assert (!live); }
			calls = serial = fail_at = 0;
			assert (R_TryCreateShaderModules () && live == total);
			R_DestroyShaderModules ();
			assert (!live);
		}
	}
	printf ("PASS: all renderer shader creation failures roll back or degrade optional paths and allow retry (maximum %u/128 modules)\n", maximum);
	return 0;
}
