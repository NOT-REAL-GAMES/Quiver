/* GPU visibility and draw-argument generation for homogeneous ECS visuals.
 * Each slot is reused only after GL_BeginRenderingTask has waited for its
 * previous submission's fence. Input is host coherent; GPU output never
 * shares mapped allocations with another frame. */
#include "quakedef.h"

#define ECS_GPU_SLOTS DOUBLE_BUFFERED
#define ECS_GPU_INSTANCE_BYTES 24u
#define ECS_GPU_LOCAL_SIZE 64u

typedef struct
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
} ecs_gpu_buffer_t;

typedef struct
{
	ecs_gpu_buffer_t input, output, indirect;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet descriptor;
	uint32_t capacity, count, index_count;
	float constants[24];
	qboolean prepared;
} ecs_gpu_slot_t;

static ecs_gpu_slot_t ecs_gpu_slots[ECS_GPU_SLOTS];
static atomic_uint64_t ecs_gpu_dispatches;

static ecs_gpu_slot_t *R_ECSGPUSlot (void)
{
	int index = R_ECSGPUFrameIndex ();
	assert (index >= 0 && index < ECS_GPU_SLOTS);
	return &ecs_gpu_slots[index];
}

static void R_ECSGPUFreeBuffer (ecs_gpu_buffer_t *buffer)
{
	if (buffer->mapped) vkUnmapMemory (vulkan_globals.device, buffer->memory);
	if (buffer->buffer) vkDestroyBuffer (vulkan_globals.device, buffer->buffer, NULL);
	if (buffer->memory) vkFreeMemory (vulkan_globals.device, buffer->memory, NULL);
	memset (buffer, 0, sizeof (*buffer));
}

/* Unlike the engine's fatal R_CreateBuffer helper, allocation failure here
 * is optional: the ordinary CPU-culling path must remain usable. */
static qboolean R_ECSGPUCreateBuffer (ecs_gpu_buffer_t *buffer, VkDeviceSize size,
	VkBufferUsageFlags usage, VkMemoryPropertyFlags required, qboolean map)
{
	VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (vkCreateBuffer (vulkan_globals.device, &info, NULL, &buffer->buffer) != VK_SUCCESS) return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, buffer->buffer, &requirements);
	uint32_t type;
	for (type = 0; type < vulkan_globals.memory_properties.memoryTypeCount; ++type)
		if ((requirements.memoryTypeBits & (1u << type)) &&
			(vulkan_globals.memory_properties.memoryTypes[type].propertyFlags & required) == required) break;
	if (type == vulkan_globals.memory_properties.memoryTypeCount) goto failed;
	VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size, .memoryTypeIndex = type};
	if (vkAllocateMemory (vulkan_globals.device, &allocation, NULL, &buffer->memory) != VK_SUCCESS) goto failed;
	if (vkBindBufferMemory (vulkan_globals.device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) goto failed;
	if (map && vkMapMemory (vulkan_globals.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS) goto failed;
	return true;
failed:
	R_ECSGPUFreeBuffer (buffer);
	return false;
}

static void R_ECSGPUFreeSlot (ecs_gpu_slot_t *slot)
{
	if (slot->descriptor_pool)
		vkDestroyDescriptorPool (vulkan_globals.device, slot->descriptor_pool, NULL);
	R_ECSGPUFreeBuffer (&slot->input);
	R_ECSGPUFreeBuffer (&slot->output);
	R_ECSGPUFreeBuffer (&slot->indirect);
	memset (slot, 0, sizeof (*slot));
}

void R_ECSGPUShutdown (void)
{
	for (int i = 0; i < ECS_GPU_SLOTS; ++i) R_ECSGPUFreeSlot (&ecs_gpu_slots[i]);
}

qboolean R_ECSGPUAvailable (void)
{
	return vulkan_globals.ecs_cull_set_layout.handle && vulkan_globals.ecs_cull_pipeline.layout.handle &&
		vulkan_globals.ecs_cull_pipeline.handle;
}

void R_ECSGPUReset (void)
{
	R_ECSGPUSlot ()->prepared = false;
}

qboolean R_ECSGPUAllocate (uint32_t count, uint32_t index_count, void **input, VkBuffer *output,
	VkDeviceSize *output_offset, VkBuffer *indirect, VkDeviceSize *indirect_offset)
{
	ecs_gpu_slot_t *slot = R_ECSGPUSlot ();
	const VkDeviceSize bytes = (VkDeviceSize)count * ECS_GPU_INSTANCE_BYTES;
	slot->prepared = false;
	if (!count || !R_ECSGPUAvailable () || bytes > vulkan_globals.device_properties.limits.maxStorageBufferRange ||
		(count / ECS_GPU_LOCAL_SIZE + (count % ECS_GPU_LOCAL_SIZE != 0)) > vulkan_globals.device_properties.limits.maxComputeWorkGroupCount[0])
		return false;
	if (count > slot->capacity)
	{
		/* This slot's previous GPU work has completed before preparation. */
		R_ECSGPUFreeSlot (slot);
		if (!R_ECSGPUCreateBuffer (&slot->input, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true) ||
			!R_ECSGPUCreateBuffer (&slot->output, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false) ||
			!R_ECSGPUCreateBuffer (&slot->indirect, sizeof (VkDrawIndexedIndirectCommand),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false))
		{
			R_ECSGPUFreeSlot (slot);
			return false;
		}
		/* A slot-local pool avoids racing other workers allocating from the
		 * engine's shared descriptor pool. */
		VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
		VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
		if (vkCreateDescriptorPool (vulkan_globals.device, &pool, NULL, &slot->descriptor_pool) != VK_SUCCESS)
		{
			R_ECSGPUFreeSlot (slot);
			return false;
		}
		VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = slot->descriptor_pool, .descriptorSetCount = 1,
			.pSetLayouts = &vulkan_globals.ecs_cull_set_layout.handle};
		if (vkAllocateDescriptorSets (vulkan_globals.device, &allocate, &slot->descriptor) != VK_SUCCESS)
		{
			R_ECSGPUFreeSlot (slot);
			return false;
		}
		slot->capacity = count;
		VkDescriptorBufferInfo buffers[3] = {{slot->input.buffer, 0, bytes}, {slot->output.buffer, 0, bytes},
			{slot->indirect.buffer, 0, sizeof (VkDrawIndexedIndirectCommand)}};
		VkWriteDescriptorSet writes[3] = {0};
		for (uint32_t i = 0; i < 3; ++i)
		{
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = slot->descriptor;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &buffers[i];
		}
		vkUpdateDescriptorSets (vulkan_globals.device, 3, writes, 0, NULL);
	}
	slot->count = count;
	slot->index_count = index_count;
	*input = slot->input.mapped;
	*output = slot->output.buffer;
	*output_offset = 0;
	*indirect = slot->indirect.buffer;
	*indirect_offset = 0;
	return true;
}

void R_ECSGPUSetPlanes (const float planes[4][4], const float mins[3], const float maxs[3])
{
	ecs_gpu_slot_t *slot = R_ECSGPUSlot ();
	memcpy (slot->constants, planes, sizeof (float) * 16);
	memcpy (&slot->constants[16], mins, sizeof (float) * 3);
	memcpy (&slot->constants[19], &slot->count, sizeof (slot->count));
	memcpy (&slot->constants[20], maxs, sizeof (float) * 3);
	slot->constants[23] = 0;
	slot->prepared = true;
}

uint64_t R_ECSGPUDispatchCount (void)
{
	return Atomic_LoadUInt64 (&ecs_gpu_dispatches);
}

void R_RecordECSVisualCull (VkCommandBuffer commands)
{
	ecs_gpu_slot_t *slot = R_ECSGPUSlot ();
	if (!slot->prepared || !R_ECSGPUPathActive () || !R_ECSGPUAvailable ()) return;
	VkDrawIndexedIndirectCommand draw = {slot->index_count, 0, 0, 0, 0};
	vkCmdUpdateBuffer (commands, slot->indirect.buffer, 0, sizeof (draw), &draw);
	VkMemoryBarrier transfer = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
	vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &transfer, 0, NULL, 0, NULL);
	vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_globals.ecs_cull_pipeline.handle);
	vkCmdBindDescriptorSets (commands, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_globals.ecs_cull_pipeline.layout.handle,
		0, 1, &slot->descriptor, 0, NULL);
	vkCmdPushConstants (commands, vulkan_globals.ecs_cull_pipeline.layout.handle, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (slot->constants), slot->constants);
	vkCmdDispatch (commands, slot->count / ECS_GPU_LOCAL_SIZE + (slot->count % ECS_GPU_LOCAL_SIZE != 0), 1, 1);
	VkMemoryBarrier complete = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
	vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &complete, 0, NULL, 0, NULL);
	Atomic_IncrementUInt64 (&ecs_gpu_dispatches);
}
