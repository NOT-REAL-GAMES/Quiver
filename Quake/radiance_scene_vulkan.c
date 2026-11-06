#include "radiance_scene_vulkan.h"

#include <string.h>
#include <stdlib.h>

static void RCSceneVk_FreeBuffer (rc_scene_vk_t *gpu, rc_scene_vk_buffer_t *buffer)
{
	if (buffer->mapped) vkUnmapMemory (gpu->device, buffer->memory);
	if (buffer->buffer) vkDestroyBuffer (gpu->device, buffer->buffer, NULL);
	if (buffer->memory) vkFreeMemory (gpu->device, buffer->memory, NULL);
	free (buffer->shadow);
	memset (buffer, 0, sizeof (*buffer));
}

static bool RCSceneVk_Buffer (rc_scene_vk_t *gpu, rc_scene_vk_buffer_t *buffer, VkDeviceSize requested)
{
	VkDeviceSize capacity = 16;
	while (capacity < requested && capacity <= gpu->max_range / 2) capacity *= 2;
	if (capacity < requested) capacity = requested;
	if (!capacity || capacity > gpu->max_range) return false;
	VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = capacity,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (vkCreateBuffer (gpu->device, &info, NULL, &buffer->buffer) != VK_SUCCESS) return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements (gpu->device, buffer->buffer, &requirements);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type;
	for (type = 0; type < gpu->memory_properties.memoryTypeCount; ++type)
		if ((requirements.memoryTypeBits & (1u << type)) &&
			(gpu->memory_properties.memoryTypes[type].propertyFlags & wanted) == wanted) break;
	if (type == gpu->memory_properties.memoryTypeCount) goto fail;
	VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size, .memoryTypeIndex = type};
	if (vkAllocateMemory (gpu->device, &alloc, NULL, &buffer->memory) != VK_SUCCESS ||
		vkBindBufferMemory (gpu->device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS ||
		vkMapMemory (gpu->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS) goto fail;
	buffer->shadow = malloc ((size_t)capacity);
	if (!buffer->shadow) goto fail;
	buffer->capacity = capacity;
	buffer->allocation_size = requirements.size;
	return true;
fail:
	RCSceneVk_FreeBuffer (gpu, buffer);
	return false;
}

static bool RCSceneVk_Layout (rc_scene_vk_t *gpu, bool hardware, VkDescriptorSetLayout *layout)
{
	VkDescriptorSetLayoutBinding bindings[5] = {0};
	for (uint32_t i = 0; i < 4; ++i)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
#ifdef VK_KHR_acceleration_structure
	if (hardware)
	{
		bindings[4].binding = 4;
		bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		bindings[4].descriptorCount = 1;
		bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
#else
	if (hardware) return false;
#endif
	VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = hardware ? 5u : 4u, .pBindings = bindings};
	return vkCreateDescriptorSetLayout (gpu->device, &info, NULL, layout) == VK_SUCCESS;
}

static bool RCSceneVk_Pool (rc_scene_vk_t *gpu, bool hardware, VkDescriptorSetLayout layout,
	VkDescriptorPool *pool, VkDescriptorSet sets[RC_SCENE_VK_MAX_FRAMES])
{
	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gpu->frames * 4}, {0, 0}};
#ifdef VK_KHR_acceleration_structure
	if (hardware) sizes[1] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, gpu->frames};
#else
	if (hardware) return false;
#endif
	VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = gpu->frames, .poolSizeCount = hardware ? 2u : 1u, .pPoolSizes = sizes};
	if (vkCreateDescriptorPool (gpu->device, &info, NULL, pool) != VK_SUCCESS) return false;
	for (uint32_t frame = 0; frame < gpu->frames; ++frame)
	{
		VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &layout};
		if (vkAllocateDescriptorSets (gpu->device, &alloc, &sets[frame]) != VK_SUCCESS) return false;
	}
	return true;
}

void RCSceneVk_Destroy (rc_scene_vk_t *gpu)
{
	if (!gpu || !gpu->device) return;
	if (gpu->software_pool) vkDestroyDescriptorPool (gpu->device, gpu->software_pool, NULL);
	if (gpu->hardware_pool) vkDestroyDescriptorPool (gpu->device, gpu->hardware_pool, NULL);
	if (gpu->software_layout) vkDestroyDescriptorSetLayout (gpu->device, gpu->software_layout, NULL);
	if (gpu->hardware_layout) vkDestroyDescriptorSetLayout (gpu->device, gpu->hardware_layout, NULL);
	for (uint32_t frame = 0; frame < RC_SCENE_VK_MAX_FRAMES; ++frame)
		for (uint32_t binding = 0; binding < 4; ++binding) RCSceneVk_FreeBuffer (gpu, &gpu->buffers[frame][binding]);
	memset (gpu, 0, sizeof (*gpu));
}

bool RCSceneVk_Create (rc_scene_vk_t *gpu, VkDevice device, const VkPhysicalDeviceMemoryProperties *memory_properties,
	const VkPhysicalDeviceLimits *limits, uint32_t frames, bool hardware_supported)
{
	if (!gpu || !device || !memory_properties || !limits || !frames || frames > RC_SCENE_VK_MAX_FRAMES ||
		limits->maxStorageBufferRange < 16 || limits->maxPerStageDescriptorStorageBuffers < 4) return false;
	memset (gpu, 0, sizeof (*gpu));
	gpu->device = device;
	gpu->memory_properties = *memory_properties;
	gpu->max_range = limits->maxStorageBufferRange;
	gpu->frames = frames;
	if (!RCSceneVk_Layout (gpu, false, &gpu->software_layout) ||
		!RCSceneVk_Pool (gpu, false, gpu->software_layout, &gpu->software_pool, gpu->software_sets)) goto fail;
	if (hardware_supported && RCSceneVk_Layout (gpu, true, &gpu->hardware_layout))
	{
		if (!RCSceneVk_Pool (gpu, true, gpu->hardware_layout, &gpu->hardware_pool, gpu->hardware_sets))
		{
			if (gpu->hardware_pool) vkDestroyDescriptorPool (device, gpu->hardware_pool, NULL);
			vkDestroyDescriptorSetLayout (device, gpu->hardware_layout, NULL);
			gpu->hardware_pool = VK_NULL_HANDLE;
			gpu->hardware_layout = VK_NULL_HANDLE;
			memset (gpu->hardware_sets, 0, sizeof (gpu->hardware_sets));
		}
	}
	return true;
fail:
	RCSceneVk_Destroy (gpu);
	return false;
}

static bool RCSceneVk_UpdateInternal (rc_scene_vk_t *gpu, uint32_t frame, const rc_scene_t *scene, bool split)
{
	if (!gpu || !scene || frame >= gpu->frames || (scene->node_count && !scene->nodes) ||
		(scene->index_count && !scene->indices) || (scene->triangle_count && !scene->triangles) ||
		scene->index_count != scene->triangle_count || (!!scene->node_count != !!scene->triangle_count)) return false;
	if (split && scene->index_count > UINT32_MAX / 2) return false;
	const VkDeviceSize sizes[4] = {(VkDeviceSize)scene->node_count * sizeof (*scene->nodes),
		(VkDeviceSize)scene->index_count * sizeof (*scene->indices) * (split ? 2u : 1u),
		(VkDeviceSize)scene->triangle_count * sizeof (*scene->triangles), 4 * sizeof (uint32_t)};
	const uint32_t counts[4] = {scene->node_count, scene->index_count * (split ? 2u : 1u), scene->triangle_count, 0};
	const void *sources[4] = {scene->nodes, scene->indices, scene->triangles, counts};
	rc_scene_vk_buffer_t replacements[4] = {0};
	gpu->last_upload_bytes[frame] = 0;
	for (uint32_t i = 0; i < 4; ++i)
	{
		if (sizes[i] > gpu->max_range || sizes[i] > SIZE_MAX) goto fail;
		if (!gpu->buffers[frame][i].buffer || sizes[i] > gpu->buffers[frame][i].capacity)
			if (!RCSceneVk_Buffer (gpu, &replacements[i], sizes[i])) goto fail;
	}
	for (uint32_t i = 0; i < 4; ++i)
	{
		if (replacements[i].buffer)
		{
			RCSceneVk_FreeBuffer (gpu, &gpu->buffers[frame][i]);
			gpu->buffers[frame][i] = replacements[i];
		}
		rc_scene_vk_buffer_t *buffer = &gpu->buffers[frame][i];
		/* Frame slots are fenced by the caller. Avoid rewriting unchanged map
		 * data when only a few moving objects changed. Comparing fixed-size
		 * blocks also detects changed emissive flags in static triangles. */
		VkDeviceSize source_size = (split && i == 1) ? sizes[i] / 2 : sizes[i];
		if (source_size)
		{
			const unsigned char *src = sources[i];
			unsigned char *dst = buffer->mapped;
			unsigned char *shadow = buffer->shadow;
			for (VkDeviceSize offset = 0; offset < source_size; offset += 4096)
			{
				const VkDeviceSize length = source_size - offset < 4096 ? source_size - offset : 4096;
				if (offset + length > buffer->initialized || memcmp (shadow + offset, src + offset, (size_t)length))
				{
					memcpy (dst + offset, src + offset, (size_t)length);
					memcpy (shadow + offset, src + offset, (size_t)length);
					gpu->last_upload_bytes[frame] += length;
				}
			}
			if (split && i == 1 && buffer->remap_count != scene->index_count)
			{
				uint32_t *mapped = (uint32_t *)(dst + source_size);
				uint32_t *copy = (uint32_t *)(shadow + source_size);
				for (uint32_t j = 0; j < scene->index_count; ++j) mapped[j] = copy[j] = j;
				gpu->last_upload_bytes[frame] += source_size;
				buffer->remap_count = scene->index_count;
			}
			else if (!split && i == 1) buffer->remap_count = 0;
			buffer->initialized = sizes[i];
		}
		else if (!buffer->initialized)
		{
			memset (buffer->mapped, 0, 16);
			buffer->initialized = 16;
			gpu->last_upload_bytes[frame] += 16;
		}
	}
	VkDescriptorBufferInfo buffers[4];
	VkWriteDescriptorSet writes[8] = {0};
	uint32_t write_count = 0;
	for (uint32_t pass = 0; pass < 2; ++pass)
	{
		VkDescriptorSet set = pass ? gpu->hardware_sets[frame] : gpu->software_sets[frame];
		if (!set) continue;
		for (uint32_t i = 0; i < 4; ++i)
		{
			buffers[i] = (VkDescriptorBufferInfo){gpu->buffers[frame][i].buffer, 0, gpu->buffers[frame][i].capacity};
			writes[write_count++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = set, .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
		}
	}
	vkUpdateDescriptorSets (gpu->device, write_count, writes, 0, NULL);
	gpu->software_ready[frame] = true;
	/* A new CPU scene needs a matching freshly built/refitted hardware AS. */
	gpu->hardware_ready[frame] = false;
#ifdef VK_KHR_acceleration_structure
	gpu->accelerations[frame] = VK_NULL_HANDLE;
#endif
	return true;
fail:
	for (uint32_t i = 0; i < 4; ++i) RCSceneVk_FreeBuffer (gpu, &replacements[i]);
	return false;
}

bool RCSceneVk_Update (rc_scene_vk_t *gpu, uint32_t frame, const rc_scene_t *scene)
{
	return RCSceneVk_UpdateInternal (gpu, frame, scene, false);
}

bool RCSceneVk_UpdateSplit (rc_scene_vk_t *gpu, uint32_t frame, const rc_scene_t *scene)
{
	return RCSceneVk_UpdateInternal (gpu, frame, scene, true);
}

#ifdef VK_KHR_acceleration_structure
bool RCSceneVk_SetAcceleration (rc_scene_vk_t *gpu, uint32_t frame, VkAccelerationStructureKHR acceleration)
{
	if (!gpu || frame >= gpu->frames || !gpu->software_ready[frame] || !gpu->hardware_sets[frame] || !acceleration) return false;
	VkWriteDescriptorSetAccelerationStructureKHR info = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
		.accelerationStructureCount = 1, .pAccelerationStructures = &acceleration};
	VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &info,
		.dstSet = gpu->hardware_sets[frame], .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
	vkUpdateDescriptorSets (gpu->device, 1, &write, 0, NULL);
	gpu->accelerations[frame] = acceleration;
	gpu->hardware_ready[frame] = true;
	return true;
}
#endif

VkDescriptorSet RCSceneVk_Descriptor (const rc_scene_vk_t *gpu, uint32_t frame, bool hardware)
{
	if (!gpu || frame >= gpu->frames) return VK_NULL_HANDLE;
	if (hardware) return gpu->hardware_ready[frame] ? gpu->hardware_sets[frame] : VK_NULL_HANDLE;
	return gpu->software_ready[frame] ? gpu->software_sets[frame] : VK_NULL_HANDLE;
}
