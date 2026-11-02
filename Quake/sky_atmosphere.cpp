/* Cached atmospheric scattering. GPL-2.0-or-later. */
#include "quakedef.h"
#include "sky_atmosphere.h"
#include "sky_profile.h"
#include "../Shaders/shaders.h"

extern cvar_t		   r_sun_angle, r_sun_azimuth;
extern atomic_uint32_t num_vulkan_misc_allocations;

namespace
{
constexpr uint32_t LUT_WIDTH = 256;
constexpr uint32_t LUT_HEIGHT = 128;
constexpr VkFormat LUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
cvar_t			   r_atmosphere_lut = {"r_atmosphere_lut", "1", CVAR_ARCHIVE};
VkImage			   lut_image;
VkImageView		   lut_view;
vulkan_memory_t	   lut_memory;
VkSampler		   lut_sampler;
VkDescriptorSet	   lut_read_set;
VkDescriptorSet	   lut_write_set;
VkPipelineLayout   lut_pipeline_layout;
VkPipeline		   lut_pipeline;
qboolean		   lut_valid;
float			   cached_sun[4];
uint64_t		   rebuild_count;

void Check (VkResult result, const char *operation)
{
	if (result != VK_SUCCESS)
		Sys_Error ("Atmosphere LUT: %s failed (%d)", operation, result);
}

void ComputeSun (float sun[4])
{
	const float elevation = r_sun_angle.value * M_PI / 180.0f;
	const float azimuth = r_sun_azimuth.value * M_PI / 180.0f;
	const float horizontal = cosf (elevation);
	sun[0] = horizontal * sinf (azimuth);
	sun[1] = horizontal * cosf (azimuth);
	sun[2] = sinf (elevation);
	sun[3] = 0.0f;
}
} // namespace

void Sky_AtmosphereInit (void)
{
	Cvar_RegisterVariable (&r_atmosphere_lut);
}

void Sky_AtmosphereCreate (VkPhysicalDevice physical_device)
{
	assert (!lut_image);
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties (physical_device, LUT_FORMAT, &properties);
	const VkFormatFeatureFlags required =
		VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	VkImageFormatProperties image_properties;
	if ((properties.optimalTilingFeatures & required) != required ||
		vkGetPhysicalDeviceImageFormatProperties (
			physical_device, LUT_FORMAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0,
			&image_properties) != VK_SUCCESS ||
		image_properties.maxExtent.width < LUT_WIDTH || image_properties.maxExtent.height < LUT_HEIGHT)
	{
		Con_Printf ("Atmosphere LUT unavailable; using direct scattering.\n");
		return;
	}

	ZEROED_STRUCT (VkImageCreateInfo, image_info);
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = LUT_FORMAT;
	image_info.extent = {LUT_WIDTH, LUT_HEIGHT, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	Check (vkCreateImage (vulkan_globals.device, &image_info, nullptr, &lut_image), "vkCreateImage");
	GL_SetObjectName ((uint64_t)lut_image, VK_OBJECT_TYPE_IMAGE, "atmosphere_lut");

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements (vulkan_globals.device, lut_image, &requirements);
	ZEROED_STRUCT (VkMemoryAllocateInfo, allocation);
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = GL_MemoryTypeFromProperties (requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	R_AllocateVulkanMemory (&lut_memory, &allocation, VULKAN_MEMORY_TYPE_DEVICE, &num_vulkan_misc_allocations);
	Check (vkBindImageMemory (vulkan_globals.device, lut_image, lut_memory.handle, 0), "vkBindImageMemory");

	ZEROED_STRUCT (VkImageViewCreateInfo, view_info);
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = lut_image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = LUT_FORMAT;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	Check (vkCreateImageView (vulkan_globals.device, &view_info, nullptr, &lut_view), "vkCreateImageView");

	ZEROED_STRUCT (VkSamplerCreateInfo, sampler_info);
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_LINEAR;
	sampler_info.minFilter = VK_FILTER_LINEAR;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.maxAnisotropy = 1.0f;
	Check (vkCreateSampler (vulkan_globals.device, &sampler_info, nullptr, &lut_sampler), "vkCreateSampler");

	lut_read_set = R_AllocateDescriptorSet (&vulkan_globals.single_texture_set_layout);
	lut_write_set = R_AllocateDescriptorSet (&vulkan_globals.single_texture_cs_write_set_layout);
	VkDescriptorImageInfo image_descriptors[2] = {
		{lut_sampler, lut_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, lut_view, VK_IMAGE_LAYOUT_GENERAL}};
	ZEROED_STRUCT_ARRAY (VkWriteDescriptorSet, writes, 2);
	for (int i = 0; i < 2; ++i)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = i == 0 ? lut_read_set : lut_write_set;
		writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].descriptorCount = 1;
		writes[i].pImageInfo = &image_descriptors[i];
	}
	vkUpdateDescriptorSets (vulkan_globals.device, 2, writes, 0, nullptr);

	VkPushConstantRange constants = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (cached_sun)};
	ZEROED_STRUCT (VkPipelineLayoutCreateInfo, layout_info);
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &vulkan_globals.single_texture_cs_write_set_layout.handle;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &constants;
	Check (vkCreatePipelineLayout (vulkan_globals.device, &layout_info, nullptr, &lut_pipeline_layout), "vkCreatePipelineLayout");

	ZEROED_STRUCT (VkShaderModuleCreateInfo, shader_info);
	shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = sky_atmosphere_lut_comp_spv_size;
	shader_info.pCode = reinterpret_cast<const uint32_t *> (sky_atmosphere_lut_comp_spv);
	VkShaderModule shader;
	Check (vkCreateShaderModule (vulkan_globals.device, &shader_info, nullptr, &shader), "vkCreateShaderModule");
	ZEROED_STRUCT (VkComputePipelineCreateInfo, pipeline_info);
	pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.layout = lut_pipeline_layout;
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = shader;
	pipeline_info.stage.pName = "main";
	const VkResult pipeline_result = vkCreateComputePipelines (vulkan_globals.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &lut_pipeline);
	vkDestroyShaderModule (vulkan_globals.device, shader, nullptr);
	Check (pipeline_result, "vkCreateComputePipelines");
	GL_SetObjectName ((uint64_t)lut_pipeline, VK_OBJECT_TYPE_PIPELINE, "atmosphere_lut");
	lut_valid = false;
}

void Sky_AtmosphereDestroy (void)
{
	if (lut_read_set)
		R_FreeDescriptorSet (lut_read_set, &vulkan_globals.single_texture_set_layout);
	if (lut_write_set)
		R_FreeDescriptorSet (lut_write_set, &vulkan_globals.single_texture_cs_write_set_layout);
	vkDestroyPipeline (vulkan_globals.device, lut_pipeline, nullptr);
	vkDestroyPipelineLayout (vulkan_globals.device, lut_pipeline_layout, nullptr);
	vkDestroySampler (vulkan_globals.device, lut_sampler, nullptr);
	vkDestroyImageView (vulkan_globals.device, lut_view, nullptr);
	vkDestroyImage (vulkan_globals.device, lut_image, nullptr);
	if (lut_memory.handle)
		R_FreeVulkanMemory (&lut_memory, &num_vulkan_misc_allocations);
	lut_read_set = lut_write_set = VK_NULL_HANDLE;
	lut_pipeline = VK_NULL_HANDLE;
	lut_pipeline_layout = VK_NULL_HANDLE;
	lut_sampler = VK_NULL_HANDLE;
	lut_view = VK_NULL_HANDLE;
	lut_image = VK_NULL_HANDLE;
	lut_valid = false;
}

qboolean Sky_AtmosphereUseLUT (void)
{
	return lut_image != VK_NULL_HANDLE && r_atmosphere_lut.value != 0;
}

void Sky_AtmospherePrepare (VkCommandBuffer command_buffer)
{
	if (!Sky_AtmosphereUseLUT ())
		return;
	float sun[4];
	ComputeSun (sun);
	if (lut_valid && memcmp (sun, cached_sun, sizeof (sun)) == 0)
		return;

	SkyProfile_BeginPass (command_buffer, SKY_PROFILE_LUT);
	ZEROED_STRUCT (VkImageMemoryBarrier, barrier);
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = lut_valid ? VK_ACCESS_SHADER_READ_BIT : 0;
	barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.oldLayout = lut_valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = lut_image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier (
		command_buffer, lut_valid ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
		nullptr, 0, nullptr, 1, &barrier);
	vkCmdBindPipeline (command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, lut_pipeline);
	vkCmdBindDescriptorSets (command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, lut_pipeline_layout, 0, 1, &lut_write_set, 0, nullptr);
	vkCmdPushConstants (command_buffer, lut_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (sun), sun);
	vkCmdDispatch (command_buffer, LUT_WIDTH / 8, LUT_HEIGHT / 8, 1);
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier (command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	SkyProfile_EndPass (command_buffer, SKY_PROFILE_LUT);
	memcpy (cached_sun, sun, sizeof (sun));
	lut_valid = true;
	++rebuild_count;
}

void Sky_AtmosphereBind (cb_context_t *cbx)
{
	const VkDescriptorSet descriptor = Sky_AtmosphereUseLUT () ? lut_read_set : whitetexture->descriptor_set;
	vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_atmosphere_pipeline_layout.handle, 0, 1, &descriptor, 0, nullptr);
}

sky_atmosphere_stats_t SkyAtmosphere_GetStats (void)
{
	return {lut_image != VK_NULL_HANDLE, Sky_AtmosphereUseLUT (), rebuild_count, lut_memory.size};
}
