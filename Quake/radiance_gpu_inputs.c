/* Vulkan upload resources for runtime radiance lights and material inputs. */
#include "radiance_gpu_inputs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(radiance_lighting_parameters_t)==176 &&
	offsetof(radiance_lighting_parameters_t,receiver_filter)==128 &&
	offsetof(radiance_lighting_parameters_t,shadow_parameters)==144,
	"lighting parameters must match the std140 shader block");

static bool RadianceInputs_MemoryType (const radiance_gpu_inputs_t *inputs, uint32_t allowed,
	VkMemoryPropertyFlags required, uint32_t *index)
{
	for (uint32_t i = 0; i < inputs->memory_properties.memoryTypeCount; ++i)
		if ((allowed & (1u << i)) &&
			(inputs->memory_properties.memoryTypes[i].propertyFlags & required) == required)
		{ *index = i; return true; }
	return false;
}

static void RadianceInputs_FreeBuffer (radiance_gpu_inputs_t *inputs, radiance_input_buffer_t *buffer)
{
	if (buffer->mapped) vkUnmapMemory (inputs->device, buffer->memory);
	if (buffer->buffer) vkDestroyBuffer (inputs->device, buffer->buffer, NULL);
	if (buffer->memory) vkFreeMemory (inputs->device, buffer->memory, NULL);
	memset (buffer, 0, sizeof (*buffer));
}

static bool RadianceInputs_EnsureBuffer (radiance_gpu_inputs_t *inputs, radiance_input_buffer_t *buffer,
	VkDeviceSize requested, VkBufferUsageFlags usage, VkDeviceSize maximum)
{
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkMemoryRequirements requirements;
	uint32_t memory_type;
	if (requested < 16) requested = 16;
	if (requested > maximum) return false;
	if (buffer->buffer && buffer->size >= requested) return true;
	RadianceInputs_FreeBuffer (inputs, buffer);
	VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = requested, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (vkCreateBuffer (inputs->device, &info, NULL, &buffer->buffer) != VK_SUCCESS) return false;
	vkGetBufferMemoryRequirements (inputs->device, buffer->buffer, &requirements);
	if (!RadianceInputs_MemoryType (inputs, requirements.memoryTypeBits, host, &memory_type)) goto fail;
	VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size, .memoryTypeIndex = memory_type};
	if (vkAllocateMemory (inputs->device, &allocation, NULL, &buffer->memory) != VK_SUCCESS) goto fail;
	if (vkBindBufferMemory (inputs->device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) goto fail;
	if (vkMapMemory (inputs->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS) goto fail;
	buffer->size = requested;
	buffer->allocation_size = requirements.size;
	return true;
fail:
	RadianceInputs_FreeBuffer (inputs, buffer);
	return false;
}

static void RadianceInputs_FreeAtlas (radiance_gpu_inputs_t *inputs, radiance_input_frame_t *frame)
{
	if (frame->atlas_view) vkDestroyImageView (inputs->device, frame->atlas_view, NULL);
	if (frame->atlas) vkDestroyImage (inputs->device, frame->atlas, NULL);
	if (frame->atlas_memory) vkFreeMemory (inputs->device, frame->atlas_memory, NULL);
	frame->atlas = VK_NULL_HANDLE;
	frame->atlas_memory = VK_NULL_HANDLE;
	frame->atlas_view = VK_NULL_HANDLE;
	frame->atlas_allocation_size = 0;
	frame->width = frame->height = frame->layers = 0;
	frame->atlas_uploaded = frame->atlas_dirty = frame->atlas_recorded = false;
}

static bool RadianceInputs_CreateAtlas (radiance_gpu_inputs_t *inputs, radiance_input_frame_t *frame,
	uint32_t width, uint32_t height, uint32_t layers)
{
	VkFormatProperties format_properties;
	VkMemoryRequirements requirements;
	uint32_t memory_type;
	vkGetPhysicalDeviceFormatProperties (inputs->physical_device, VK_FORMAT_R8G8B8A8_SRGB, &format_properties);
	const VkFormatFeatureFlags features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if ((format_properties.optimalTilingFeatures & features) != features) return false;
	VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_SRGB, .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = layers,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	if (vkCreateImage (inputs->device, &image, NULL, &frame->atlas) != VK_SUCCESS) return false;
	vkGetImageMemoryRequirements (inputs->device, frame->atlas, &requirements);
	if (!RadianceInputs_MemoryType (inputs, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type) &&
		!RadianceInputs_MemoryType (inputs, requirements.memoryTypeBits, 0, &memory_type)) goto fail;
	VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size, .memoryTypeIndex = memory_type};
	if (vkAllocateMemory (inputs->device, &allocation, NULL, &frame->atlas_memory) != VK_SUCCESS) goto fail;
	if (vkBindImageMemory (inputs->device, frame->atlas, frame->atlas_memory, 0) != VK_SUCCESS) goto fail;
	VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = frame->atlas, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R8G8B8A8_SRGB,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
	if (vkCreateImageView (inputs->device, &view, NULL, &frame->atlas_view) != VK_SUCCESS) goto fail;
	frame->width = width; frame->height = height; frame->layers = layers;
	frame->atlas_allocation_size = requirements.size;
	frame->atlas_dirty = true;
	return true;
fail:
	RadianceInputs_FreeAtlas (inputs, frame);
	return false;
}

void RadianceInputs_Destroy (radiance_gpu_inputs_t *inputs)
{
	if (!inputs || !inputs->device) return;
	if (inputs->pool) vkDestroyDescriptorPool (inputs->device, inputs->pool, NULL);
	if (inputs->layout) vkDestroyDescriptorSetLayout (inputs->device, inputs->layout, NULL);
	if (inputs->sampler) vkDestroySampler (inputs->device, inputs->sampler, NULL);
	for (uint32_t i = 0; i < RADIANCE_INPUT_MAX_FRAMES; ++i)
	{
		for (uint32_t j = 0; j < 5; ++j) RadianceInputs_FreeBuffer (inputs, &inputs->frame[i].buffers[j]);
		RadianceInputs_FreeBuffer (inputs, &inputs->frame[i].staging);
		RadianceInputs_FreeAtlas (inputs, &inputs->frame[i]);
	}
	memset (inputs, 0, sizeof (*inputs));
}

bool RadianceInputs_Create (radiance_gpu_inputs_t *inputs, VkPhysicalDevice physical_device, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory_properties, const VkPhysicalDeviceLimits *limits, uint32_t frames)
{
	if (!inputs || !physical_device || !device || !memory_properties || !limits || !frames || frames > RADIANCE_INPUT_MAX_FRAMES ||
		limits->maxPerStageDescriptorStorageBuffers < 4 || limits->maxDescriptorSetStorageBuffers < 4 ||
		limits->maxPerStageDescriptorSamplers < 1 || limits->maxUniformBufferRange < sizeof (radiance_lighting_parameters_t)) return false;
	memset (inputs, 0, sizeof (*inputs));
	inputs->physical_device = physical_device; inputs->device = device;
	inputs->memory_properties = *memory_properties; inputs->limits = *limits; inputs->frames = frames;
	VkDescriptorSetLayoutBinding bindings[6] = {0};
	for (uint32_t i = 0; i < 6; ++i)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
			(i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 6, .pBindings = bindings};
	if (vkCreateDescriptorSetLayout (device, &layout, NULL, &inputs->layout) != VK_SUCCESS) goto fail;
	VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frames * 4},
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frames}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frames}};
	VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = frames, .poolSizeCount = 3, .pPoolSizes = sizes};
	if (vkCreateDescriptorPool (device, &pool, NULL, &inputs->pool) != VK_SUCCESS) goto fail;
	VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
	if (vkCreateSampler (device, &sampler, NULL, &inputs->sampler) != VK_SUCCESS) goto fail;
	for (uint32_t i = 0; i < frames; ++i)
	{
		VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = inputs->pool, .descriptorSetCount = 1, .pSetLayouts = &inputs->layout};
		if (vkAllocateDescriptorSets (device, &allocation, &inputs->frame[i].descriptor) != VK_SUCCESS) goto fail;
	}
	return true;
fail:
	RadianceInputs_Destroy (inputs);
	return false;
}

static bool RadianceInputs_ValidateSnapshot (const radiance_gpu_inputs_t *inputs, const radiance_input_snapshot_t *s,
	VkDeviceSize *atlas_bytes)
{
	if (!s || (!s->lights && s->light_count) || (!s->materials && s->material_count) ||
		(!s->grid_offsets && s->grid_offset_count) || (!s->grid_indices && s->grid_index_count) || !s->atlas_rgba ||
		s->light_count > UINT32_MAX || s->material_count > UINT32_MAX || s->grid_index_count > UINT32_MAX ||
		s->parameters.counts[0] != s->light_count || s->parameters.counts[1] != s->material_count ||
		s->parameters.counts[2] != s->atlas_layers || (s->parameters.counts[3] & ~255u) ||
		!s->atlas_width || !s->atlas_height || !s->atlas_layers || s->atlas_width > inputs->limits.maxImageDimension2D ||
		s->atlas_height > inputs->limits.maxImageDimension2D || s->atlas_layers > inputs->limits.maxImageArrayLayers) return false;
	if ((s->parameters.counts[3] & 4u) && (!isfinite(s->parameters.grid_origin[3]) || s->parameters.grid_origin[3] < 0 ||
		!isfinite(s->parameters.grid_inv_cell_size[3]) || s->parameters.grid_inv_cell_size[3] < 0)) return false;
	if ((s->parameters.counts[3] & 8u) && (!isfinite (s->parameters.bounce_parameters[0]) ||
		s->parameters.bounce_parameters[0] < 0)) return false;
	if (!isfinite (s->parameters.bounce_parameters[1]) || s->parameters.bounce_parameters[1] < 0 ||
		s->parameters.bounce_parameters[1] > 1) return false;
	for (unsigned i = 0; i < 4; ++i)
		if (!isfinite (s->parameters.gi_camera_fade[i])) return false;
	if (s->parameters.gi_camera_fade[3] < 0) return false;
	if (!isfinite (s->parameters.receiver_filter[0]) || s->parameters.receiver_filter[0] < 0 ||
		s->parameters.receiver_filter[0] > 2) return false;
	if (!isfinite (s->parameters.receiver_filter[3]) || s->parameters.receiver_filter[3] < 0 ||
		s->parameters.receiver_filter[3] > 128) return false;
	const float *shadows = s->parameters.shadow_parameters;
	if (!isfinite (shadows[0]) || (shadows[0] != 0.0f && shadows[0] != 1.0f) ||
		!isfinite (shadows[1]) || shadows[1] < 0 || shadows[1] > 64 ||
		!isfinite (shadows[2]) || shadows[2] < 0 || shadows[2] > 15.0f * 0.01745329251994329577f ||
		!isfinite (shadows[3]) || shadows[3] < 0 || shadows[3] > 2) return false;
	uint64_t pixels = (uint64_t)s->atlas_width * s->atlas_height;
	if (pixels > UINT64_MAX / s->atlas_layers || pixels * s->atlas_layers > SIZE_MAX / 4) return false;
	*atlas_bytes = pixels * s->atlas_layers * 4;
	if (s->light_count > SIZE_MAX / sizeof (*s->lights) || s->material_count > SIZE_MAX / sizeof (*s->materials) ||
		s->grid_offset_count > SIZE_MAX / sizeof (uint32_t) || s->grid_index_count > SIZE_MAX / sizeof (uint32_t)) return false;
	if ((s->parameters.counts[3] & 1u) != 0)
	{
		uint64_t cells = 1;
		for (int a = 0; a < 3; ++a)
		{
			uint32_t dim = s->parameters.grid_dimensions[a];
			if (!dim || cells > UINT32_MAX / dim || !isfinite (s->parameters.grid_origin[a]) ||
				!isfinite (s->parameters.grid_inv_cell_size[a]) || s->parameters.grid_inv_cell_size[a] <= 0) return false;
			cells *= dim;
		}
		if (s->grid_offset_count != cells + 1 || s->parameters.grid_dimensions[3] != s->grid_index_count ||
			!s->grid_offsets || s->grid_offsets[0] != 0 || s->grid_offsets[cells] != s->grid_index_count) return false;
		for (size_t i = 0; i < cells; ++i) if (s->grid_offsets[i] > s->grid_offsets[i + 1]) return false;
		for (size_t i = 0; i < s->grid_index_count; ++i) if (s->grid_indices[i] >= s->light_count) return false;
	}
	return true;
}

bool RadianceInputs_Update (radiance_gpu_inputs_t *inputs, uint32_t frame_index, const radiance_input_snapshot_t *snapshot)
{
	if (!inputs || !inputs->device || frame_index >= inputs->frames) return false;
	radiance_input_frame_t *frame = &inputs->frame[frame_index];
	if (frame->atlas_recorded) return false;
	frame->valid = false;
	VkDeviceSize atlas_bytes;
	if (!RadianceInputs_ValidateSnapshot (inputs, snapshot, &atlas_bytes)) return false;
	const void *sources[5] = {snapshot->lights, &snapshot->parameters, snapshot->materials,
		snapshot->grid_offsets, snapshot->grid_indices};
	VkDeviceSize sizes[5] = {snapshot->light_count * sizeof (*snapshot->lights), sizeof (snapshot->parameters),
		snapshot->material_count * sizeof (*snapshot->materials), snapshot->grid_offset_count * sizeof (uint32_t),
		snapshot->grid_index_count * sizeof (uint32_t)};
	bool same_grid = snapshot->grid_revision && frame->grid_revision == snapshot->grid_revision &&
		frame->grid_offset_count == snapshot->grid_offset_count && frame->grid_index_count == snapshot->grid_index_count &&
		frame->buffers[3].buffer && frame->buffers[3].mapped && frame->buffers[3].size >= sizes[3] &&
		frame->buffers[4].buffer && frame->buffers[4].mapped && frame->buffers[4].size >= sizes[4];
	/* A failed allocation must never leave the old identity associated with
	 * partially replaced buffers. */
	if (!same_grid) frame->grid_revision = 0;
	for (uint32_t i = 0; i < 5; ++i)
	{
		if (same_grid && i >= 3) continue;
		VkBufferUsageFlags usage = i == 1 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		VkDeviceSize max = i == 1 ? inputs->limits.maxUniformBufferRange : inputs->limits.maxStorageBufferRange;
		if (!RadianceInputs_EnsureBuffer (inputs, &frame->buffers[i], sizes[i], usage, max)) return false;
		if (sizes[i]) memcpy (frame->buffers[i].mapped, sources[i], (size_t)sizes[i]);
		else memset (frame->buffers[i].mapped, 0, 16);
	}
	frame->grid_revision = snapshot->grid_revision;
	frame->grid_offset_count = snapshot->grid_offset_count;
	frame->grid_index_count = snapshot->grid_index_count;
	bool same_size = frame->atlas && frame->width == snapshot->atlas_width && frame->height == snapshot->atlas_height &&
		frame->layers == snapshot->atlas_layers;
	/* Vulkan host-visible staging memory can be write-combined. Reading the
	 * entire atlas from it every frame is prohibitively slow on discrete GPUs.
	 * A nonzero revision is a caller-provided guarantee about immutable bytes;
	 * unversioned callers retain the previous exact bytewise behavior. */
	bool same_pixels = same_size && frame->staging.mapped && frame->staging.size >= atlas_bytes &&
		(snapshot->atlas_revision ? frame->atlas_revision == snapshot->atlas_revision :
		 memcmp (frame->staging.mapped, snapshot->atlas_rgba, (size_t)atlas_bytes) == 0);
	if (!RadianceInputs_EnsureBuffer (inputs, &frame->staging, atlas_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, SIZE_MAX)) return false;
	if (!same_size)
	{
		RadianceInputs_FreeAtlas (inputs, frame);
		if (!RadianceInputs_CreateAtlas (inputs, frame, snapshot->atlas_width, snapshot->atlas_height, snapshot->atlas_layers)) return false;
	}
	if (!same_pixels)
	{
		memcpy (frame->staging.mapped, snapshot->atlas_rgba, (size_t)atlas_bytes);
		frame->atlas_dirty = true;
	}
	frame->atlas_revision = snapshot->atlas_revision;
	VkDescriptorBufferInfo buffers[5];
	for (uint32_t i = 0; i < 5; ++i) buffers[i] = (VkDescriptorBufferInfo){frame->buffers[i].buffer, 0, frame->buffers[i].size};
	VkDescriptorImageInfo image = {inputs->sampler, frame->atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[6] = {0};
	for (uint32_t i = 0; i < 6; ++i)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = frame->descriptor; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
		writes[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
			(i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		if (i == 3) writes[i].pImageInfo = &image;
		else writes[i].pBufferInfo = &buffers[i < 3 ? i : i - 1];
	}
	vkUpdateDescriptorSets (inputs->device, 6, writes, 0, NULL);
	frame->valid = true;
	return true;
}

bool RadianceInputs_RecordUpload (radiance_gpu_inputs_t *inputs, uint32_t frame_index, VkCommandBuffer commands)
{
	if (!inputs || frame_index >= inputs->frames || !commands || !inputs->frame[frame_index].valid ||
		inputs->frame[frame_index].atlas_recorded) return false;
	radiance_input_frame_t *frame = &inputs->frame[frame_index];
	VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
	vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_HOST_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &host, 0, NULL, 0, NULL);
	if (frame->atlas_dirty)
	{
		VkImageMemoryBarrier transition = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = frame->atlas_uploaded ? VK_ACCESS_SHADER_READ_BIT : 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = frame->atlas_uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = frame->atlas,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, frame->layers}};
		vkCmdPipelineBarrier (commands, frame->atlas_uploaded ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
		VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, frame->layers},
			.imageExtent = {frame->width, frame->height, 1}};
		vkCmdCopyBufferToImage (commands, frame->staging.buffer, frame->atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		transition.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, NULL, 0, NULL, 1, &transition);
		frame->atlas_recorded = true;
	}
	return frame->atlas_uploaded || frame->atlas_recorded;
}

void RadianceInputs_MarkSubmitted (radiance_gpu_inputs_t *inputs, uint32_t frame)
{
	if (!inputs || frame >= inputs->frames || !inputs->frame[frame].atlas_recorded) return;
	inputs->frame[frame].atlas_uploaded = true;
	inputs->frame[frame].atlas_dirty = false;
	inputs->frame[frame].atlas_recorded = false;
}

void RadianceInputs_DiscardRecorded (radiance_gpu_inputs_t *inputs, uint32_t frame)
{
	if (!inputs || frame >= inputs->frames) return;
	inputs->frame[frame].atlas_recorded = false;
}

VkDescriptorSet RadianceInputs_Descriptor (const radiance_gpu_inputs_t *inputs, uint32_t frame)
{
	return inputs && frame < inputs->frames && inputs->frame[frame].valid ? inputs->frame[frame].descriptor : VK_NULL_HANDLE;
}

uint64_t RadianceInputs_AllocationBytes (const radiance_gpu_inputs_t *inputs)
{
	uint64_t bytes = 0;
	if (!inputs) return 0;
	for (uint32_t i = 0; i < inputs->frames; ++i)
	{
		bytes += inputs->frame[i].atlas_allocation_size + inputs->frame[i].staging.allocation_size;
		for (uint32_t j = 0; j < 5; ++j) bytes += inputs->frame[i].buffers[j].allocation_size;
	}
	return bytes;
}
