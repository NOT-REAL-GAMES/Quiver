#include "radiance_receiver_vulkan.h"
#include "radiance_matrix.h"

#include <string.h>

static bool RCReceiverVk_Buffer (rc_receiver_vk_t *receiver, rc_vk_buffer_t *buffer)
{
	VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16 * sizeof (float),
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (vkCreateBuffer (receiver->device, &info, NULL, &buffer->buffer) != VK_SUCCESS) return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements (receiver->device, buffer->buffer, &requirements);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type;
	for (type = 0; type < receiver->memory_properties.memoryTypeCount; ++type)
		if ((requirements.memoryTypeBits & (1u << type)) &&
			(receiver->memory_properties.memoryTypes[type].propertyFlags & wanted) == wanted) break;
	if (type == receiver->memory_properties.memoryTypeCount) return false;
	VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size, .memoryTypeIndex = type};
	if (vkAllocateMemory (receiver->device, &allocation, NULL, &buffer->memory) != VK_SUCCESS ||
		vkBindBufferMemory (receiver->device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS ||
		vkMapMemory (receiver->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS) return false;
	buffer->size = info.size;
	buffer->allocation_size = requirements.size;
	return true;
}

static bool RCReceiverVk_Layout (rc_receiver_vk_t *receiver, bool hardware, VkDescriptorSetLayout *layout)
{
	VkDescriptorSetLayoutBinding bindings[21] = {0};
	uint32_t count = 0;
	for (uint32_t binding = 0; binding <= 20; ++binding)
	{
		if (binding == 13 && !hardware) continue;
		VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (binding == 0 || binding == 15 || binding == 20) type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		else if (binding == 17) type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		else if (binding == 13)
		{
#ifdef VK_KHR_acceleration_structure
			type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
#else
			return false;
#endif
		}
		bindings[count++] = (VkDescriptorSetLayoutBinding){.binding = binding, .descriptorType = type,
			.descriptorCount = 1, .stageFlags = binding == 20 ? VK_SHADER_STAGE_VERTEX_BIT : (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)};
	}
	VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = count, .pBindings = bindings};
	return vkCreateDescriptorSetLayout (receiver->device, &info, NULL, layout) == VK_SUCCESS;
}

static bool RCReceiverVk_Pool (rc_receiver_vk_t *receiver, bool hardware, VkDescriptorSetLayout layout,
	VkDescriptorPool *pool, VkDescriptorSet sets[RC_VK_MAX_FRAMES])
{
	VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, receiver->frames * 16},
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, receiver->frames * 3}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, receiver->frames}, {0, 0}};
#ifdef VK_KHR_acceleration_structure
	if (hardware) sizes[3] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, receiver->frames};
#else
	if (hardware) return false;
#endif
	VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = receiver->frames, .poolSizeCount = hardware ? 4u : 3u, .pPoolSizes = sizes};
	if (vkCreateDescriptorPool (receiver->device, &info, NULL, pool) != VK_SUCCESS) return false;
	for (uint32_t frame = 0; frame < receiver->frames; ++frame)
	{
		VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &layout};
		if (vkAllocateDescriptorSets (receiver->device, &allocation, &sets[frame]) != VK_SUCCESS) return false;
	}
	return true;
}

void RCReceiverVk_Destroy (rc_receiver_vk_t *receiver)
{
	if (!receiver || !receiver->device) return;
	if (receiver->software_pool) vkDestroyDescriptorPool (receiver->device, receiver->software_pool, NULL);
	if (receiver->hardware_pool) vkDestroyDescriptorPool (receiver->device, receiver->hardware_pool, NULL);
	if (receiver->software_layout) vkDestroyDescriptorSetLayout (receiver->device, receiver->software_layout, NULL);
	if (receiver->hardware_layout) vkDestroyDescriptorSetLayout (receiver->device, receiver->hardware_layout, NULL);
	for (uint32_t frame = 0; frame < RC_VK_MAX_FRAMES; ++frame)
	{
		rc_vk_buffer_t *buffer = &receiver->inverse_view_projection[frame];
		if (buffer->mapped) vkUnmapMemory (receiver->device, buffer->memory);
		if (buffer->buffer) vkDestroyBuffer (receiver->device, buffer->buffer, NULL);
		if (buffer->memory) vkFreeMemory (receiver->device, buffer->memory, NULL);
	}
	memset (receiver, 0, sizeof (*receiver));
}

bool RCReceiverVk_Create (rc_receiver_vk_t *receiver, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory_properties, const VkPhysicalDeviceLimits *limits,
	uint32_t frames, bool hardware_supported)
{
	if (!receiver || !device || !memory_properties || !limits || !frames || frames > RC_VK_MAX_FRAMES ||
		limits->maxBoundDescriptorSets < 6 || limits->maxPerStageDescriptorStorageBuffers < 16 ||
		limits->maxDescriptorSetStorageBuffers < 18 || limits->maxPerStageDescriptorUniformBuffers < 3 ||
		limits->maxPerStageDescriptorSampledImages < 4 || limits->maxUniformBufferRange < 16 * sizeof (float)) return false;
	memset (receiver, 0, sizeof (*receiver));
	receiver->device = device; receiver->memory_properties = *memory_properties; receiver->frames = frames;
	for (uint32_t frame = 0; frame < frames; ++frame)
		if (!RCReceiverVk_Buffer (receiver, &receiver->inverse_view_projection[frame])) goto fail;
	if (!RCReceiverVk_Layout (receiver, false, &receiver->software_layout) ||
		!RCReceiverVk_Pool (receiver, false, receiver->software_layout, &receiver->software_pool, receiver->software_sets)) goto fail;
	if (hardware_supported && RCReceiverVk_Layout (receiver, true, &receiver->hardware_layout))
	{
		if (!RCReceiverVk_Pool (receiver, true, receiver->hardware_layout, &receiver->hardware_pool, receiver->hardware_sets))
		{
			if (receiver->hardware_pool) vkDestroyDescriptorPool (device, receiver->hardware_pool, NULL);
			vkDestroyDescriptorSetLayout (device, receiver->hardware_layout, NULL);
			receiver->hardware_pool = VK_NULL_HANDLE; receiver->hardware_layout = VK_NULL_HANDLE;
			memset (receiver->hardware_sets, 0, sizeof (receiver->hardware_sets));
		}
	}
	return true;
fail:
	RCReceiverVk_Destroy (receiver);
	return false;
}

bool RCReceiverVk_Update (rc_receiver_vk_t *receiver, uint32_t frame, const rc_vulkan_t *cascades,
	const rc_scene_vk_t *scene, const radiance_gpu_inputs_t *lighting, const float view_projection[16])
{
	if (!receiver || frame >= receiver->frames) return false;
	receiver->software_ready[frame] = receiver->hardware_ready[frame] = false;
	if (!cascades || !scene || !lighting || frame >= cascades->frames || frame >= scene->frames || frame >= lighting->frames ||
		!cascades->snapshot_ready[frame] || !scene->software_ready[frame] || !lighting->frame[frame].valid ||
		!lighting->sampler || !lighting->frame[frame].atlas_view || !view_projection) return false;
	float inverse[16];
	if (!Radiance_InvertMatrix (view_projection, inverse)) return false;
	memcpy (receiver->inverse_view_projection[frame].mapped, inverse, sizeof (inverse));
	const radiance_input_frame_t *input = &lighting->frame[frame];
	VkDescriptorBufferInfo buffers[21] = {0};
	buffers[0] = (VkDescriptorBufferInfo){cascades->metadata[frame].buffer, 0, cascades->metadata[frame].size};
	for (uint32_t i = 1; i <= 6; ++i)
		buffers[i] = (VkDescriptorBufferInfo){cascades->records[i-1].buffer, 0, cascades->records[i-1].size};
	buffers[7] = (VkDescriptorBufferInfo){cascades->brick_table[frame].buffer, 0, cascades->brick_table[frame].size};
	buffers[8] = (VkDescriptorBufferInfo){cascades->active_bricks[frame].buffer, 0, cascades->active_bricks[frame].size};
	for (uint32_t i = 9; i <= 12; ++i)
		buffers[i] = (VkDescriptorBufferInfo){scene->buffers[frame][i-9].buffer, 0, scene->buffers[frame][i-9].capacity};
	buffers[14] = (VkDescriptorBufferInfo){input->buffers[0].buffer, 0, input->buffers[0].size};
	buffers[15] = (VkDescriptorBufferInfo){input->buffers[1].buffer, 0, input->buffers[1].size};
	buffers[16] = (VkDescriptorBufferInfo){input->buffers[2].buffer, 0, input->buffers[2].size};
	buffers[18] = (VkDescriptorBufferInfo){input->buffers[3].buffer, 0, input->buffers[3].size};
	buffers[19] = (VkDescriptorBufferInfo){input->buffers[4].buffer, 0, input->buffers[4].size};
	buffers[20] = (VkDescriptorBufferInfo){receiver->inverse_view_projection[frame].buffer, 0, 16 * sizeof (float)};
	VkDescriptorImageInfo image = {lighting->sampler, input->atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	for (uint32_t pass = 0; pass < 2; ++pass)
	{
		VkDescriptorSet set = pass ? receiver->hardware_sets[frame] : receiver->software_sets[frame];
		if (!set || (pass && !scene->hardware_ready[frame])) continue;
		VkWriteDescriptorSet writes[21] = {0};
		uint32_t count = 0;
#ifdef VK_KHR_acceleration_structure
		VkWriteDescriptorSetAccelerationStructureKHR acceleration = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			.accelerationStructureCount = 1, .pAccelerationStructures = &scene->accelerations[frame]};
#endif
		for (uint32_t binding = 0; binding <= 20; ++binding)
		{
			if (binding == 13 && !pass) continue;
			VkWriteDescriptorSet *write = &writes[count++];
			write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write->dstSet = set; write->dstBinding = binding; write->descriptorCount = 1;
			if (binding == 13)
			{
#ifdef VK_KHR_acceleration_structure
				write->descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; write->pNext = &acceleration;
#endif
			}
			else if (binding == 17) { write->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write->pImageInfo = &image; }
			else { write->descriptorType = binding == 0 || binding == 15 || binding == 20 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				write->pBufferInfo = &buffers[binding]; }
		}
		vkUpdateDescriptorSets (receiver->device, count, writes, 0, NULL);
		if (pass) receiver->hardware_ready[frame] = true; else receiver->software_ready[frame] = true;
	}
	return receiver->software_ready[frame];
}

VkDescriptorSet RCReceiverVk_Descriptor (const rc_receiver_vk_t *receiver, uint32_t frame, bool hardware)
{
	if (!receiver || frame >= receiver->frames) return VK_NULL_HANDLE;
	return hardware ? (receiver->hardware_ready[frame] ? receiver->hardware_sets[frame] : VK_NULL_HANDLE) :
		(receiver->software_ready[frame] ? receiver->software_sets[frame] : VK_NULL_HANDLE);
}
