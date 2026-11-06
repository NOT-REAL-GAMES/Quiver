/* Vulkan allocation and compute dispatch for radiance intervals. */
#include "radiance_vulkan.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct rc_vk_push_s
{
	uint32_t level, first, count, flags;
} rc_vk_push_t;

static bool RCVk_MemoryType (const rc_vulkan_t *vk, uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *index)
{
	for (uint32_t i = 0; i < vk->memory_properties.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (vk->memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
		{
			*index = i;
			return true;
		}
	return false;
}

static void RCVk_FreeBuffer (rc_vulkan_t *vk, rc_vk_buffer_t *buffer)
{
	if (buffer->mapped) vkUnmapMemory (vk->device, buffer->memory);
	if (buffer->buffer) vkDestroyBuffer (vk->device, buffer->buffer, NULL);
	if (buffer->memory) vkFreeMemory (vk->device, buffer->memory, NULL);
	memset (buffer, 0, sizeof (*buffer));
}

static bool RCVk_Buffer (rc_vulkan_t *vk, rc_vk_buffer_t *buffer, VkDeviceSize size, VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memory_flags, bool mapped)
{
	VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkMemoryRequirements requirements;
	uint32_t memory_type;
	if (!size || vkCreateBuffer (vk->device, &info, NULL, &buffer->buffer) != VK_SUCCESS) return false;
	vkGetBufferMemoryRequirements (vk->device, buffer->buffer, &requirements);
	if (!RCVk_MemoryType (vk, requirements.memoryTypeBits, memory_flags, &memory_type)) goto fail;
	VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size, .memoryTypeIndex = memory_type};
	if (vkAllocateMemory (vk->device, &allocation, NULL, &buffer->memory) != VK_SUCCESS) goto fail;
	if (vkBindBufferMemory (vk->device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) goto fail;
	if (mapped && vkMapMemory (vk->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS) goto fail;
	buffer->size = size;
	buffer->allocation_size = requirements.size;
	return true;
fail:
	RCVk_FreeBuffer (vk, buffer);
	return false;
}

static bool RCVk_ComputePipeline (VkDevice device, VkPipelineCache cache, VkPipelineLayout layout, rc_vk_shader_t shader, VkPipeline *pipeline)
{
	if (!shader.words || !shader.size_bytes || shader.size_bytes % sizeof (uint32_t)) return false;
	VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shader.size_bytes, .pCode = shader.words};
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule (device, &module_info, NULL, &module) != VK_SUCCESS) return false;
	VkPipelineShaderStageCreateInfo stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"};
	VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = stage, .layout = layout};
	VkResult result = vkCreateComputePipelines (device, cache, 1, &info, NULL, pipeline);
	vkDestroyShaderModule (device, module, NULL);
	return result == VK_SUCCESS;
}

static bool RCVk_PipelineLayout (rc_vulkan_t *vk, VkDescriptorSetLayout scene, VkDescriptorSetLayout lights,
	VkPipelineLayout *layout)
{
	VkDescriptorSetLayout sets[3] = {vk->cascade_layout, scene, lights};
	VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof (rc_vk_push_t)};
	VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 3, .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
	return vkCreatePipelineLayout (vk->device, &info, NULL, layout) == VK_SUCCESS;
}

void RCVk_Destroy (rc_vulkan_t *vk)
{
	if (!vk || !vk->device) return;
	if (vk->trace_software) vkDestroyPipeline (vk->device, vk->trace_software, NULL);
	if (vk->trace_hardware) vkDestroyPipeline (vk->device, vk->trace_hardware, NULL);
	if (vk->merge_software) vkDestroyPipeline (vk->device, vk->merge_software, NULL);
	if (vk->merge_hardware) vkDestroyPipeline (vk->device, vk->merge_hardware, NULL);
	if (vk->software_layout) vkDestroyPipelineLayout (vk->device, vk->software_layout, NULL);
	if (vk->hardware_layout) vkDestroyPipelineLayout (vk->device, vk->hardware_layout, NULL);
	if (vk->descriptor_pool) vkDestroyDescriptorPool (vk->device, vk->descriptor_pool, NULL);
	if (vk->cascade_layout) vkDestroyDescriptorSetLayout (vk->device, vk->cascade_layout, NULL);
	for (uint32_t i = 0; i < RC_VK_MAX_FRAMES; ++i)
	{
		RCVk_FreeBuffer (vk, &vk->metadata[i]);
		RCVk_FreeBuffer (vk, &vk->brick_table[i]);
		RCVk_FreeBuffer (vk, &vk->active_bricks[i]);
		free (vk->clears[i]);
	}
	for (uint32_t i = 0; i < 6; ++i) RCVk_FreeBuffer (vk, &vk->records[i]);
	memset (vk, 0, sizeof (*vk));
}

bool RCVk_Create (rc_vulkan_t *vk, VkDevice device, const VkPhysicalDeviceMemoryProperties *memory_properties,
	const VkPhysicalDeviceLimits *limits, const rc_sparse_t *sparse, uint32_t frames,
	VkDescriptorSetLayout software_scene_layout, VkDescriptorSetLayout hardware_scene_layout,
	VkDescriptorSetLayout lights_layout, rc_vk_shader_t trace_software, rc_vk_shader_t trace_hardware,
	rc_vk_shader_t merge_shader, VkPipelineCache pipeline_cache)
{
	uint32_t table_count = 0;
	const rc_layout_t *layout = RC_SparseLayout (sparse);
	if (sparse) RC_SparseTable (sparse, &table_count);
	if (!vk || !device || !memory_properties || !limits || !layout || !layout->record_count || !table_count || !frames ||
		frames > RC_VK_MAX_FRAMES || !software_scene_layout || !lights_layout || limits->maxBoundDescriptorSets < 3 ||
		/* Cascade: 8; scene: 4; lights/material grid: 4 storage buffers. */
		limits->maxPerStageDescriptorStorageBuffers < 16 || limits->maxDescriptorSetStorageBuffers < 16 ||
		limits->maxPushConstantsSize < sizeof (rc_vk_push_t) || !limits->maxComputeWorkGroupCount[0]) return false;
	memset (vk, 0, sizeof (*vk));
	vk->device = device;
	vk->memory_properties = *memory_properties;
	vk->frames = frames;
	vk->record_count = layout->record_count;
	vk->table_capacity = table_count;
	vk->max_workgroups = limits->maxComputeWorkGroupCount[0];
	VkDeviceSize large = (VkDeviceSize)layout->record_count * 4 * sizeof (float);
	VkDeviceSize small = (VkDeviceSize)layout->record_count * sizeof (float);
	if (large > limits->maxStorageBufferRange || (VkDeviceSize)table_count * sizeof (rc_sparse_brick_t) > limits->maxStorageBufferRange ||
		sizeof (layout->levels) > limits->maxUniformBufferRange) goto fail;
	for (uint32_t i = 0; i < 6; ++i)
		if (!RCVk_Buffer (vk, &vk->records[i], i < 2 ? large : small,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false)) goto fail;
	for (uint32_t i = 0; i < frames; ++i)
	{
		const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if (!RCVk_Buffer (vk, &vk->metadata[i], sizeof (layout->levels), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, true) ||
			!RCVk_Buffer (vk, &vk->brick_table[i], (VkDeviceSize)table_count * sizeof (uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, true) ||
			!RCVk_Buffer (vk, &vk->active_bricks[i], (VkDeviceSize)table_count * sizeof (rc_sparse_brick_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, true) ||
			!(vk->clears[i] = calloc (table_count, sizeof (*vk->clears[i])))) goto fail;
	}

	VkDescriptorSetLayoutBinding bindings[9] = {0};
	for (uint32_t i = 0; i < 9; ++i)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 9, .pBindings = bindings};
	if (vkCreateDescriptorSetLayout (device, &set_info, NULL, &vk->cascade_layout) != VK_SUCCESS) goto fail;
	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frames}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frames * 8}};
	VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = frames, .poolSizeCount = 2, .pPoolSizes = sizes};
	if (vkCreateDescriptorPool (device, &pool_info, NULL, &vk->descriptor_pool) != VK_SUCCESS) goto fail;
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = vk->descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &vk->cascade_layout};
		if (vkAllocateDescriptorSets (device, &alloc, &vk->descriptor_sets[frame]) != VK_SUCCESS) goto fail;
		VkDescriptorBufferInfo buffers[9] = {0};
		VkWriteDescriptorSet writes[9] = {0};
		for (uint32_t i = 0; i < 9; ++i)
		{
			rc_vk_buffer_t *buffer = !i ? &vk->metadata[frame] : i <= 6 ? &vk->records[i - 1] :
				(i == 7 ? &vk->brick_table[frame] : &vk->active_bricks[frame]);
			buffers[i] = (VkDescriptorBufferInfo){buffer->buffer, 0, buffer->size};
			writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = vk->descriptor_sets[frame], .dstBinding = i, .descriptorCount = 1,
				.descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &buffers[i]};
		}
		vkUpdateDescriptorSets (device, 9, writes, 0, NULL);
		memcpy (vk->metadata[frame].mapped, layout->levels, sizeof (layout->levels));
	}
	if (!RCVk_PipelineLayout (vk, software_scene_layout, lights_layout, &vk->software_layout) ||
		!RCVk_ComputePipeline (device, pipeline_cache, vk->software_layout, trace_software, &vk->trace_software) ||
		!RCVk_ComputePipeline (device, pipeline_cache, vk->software_layout, merge_shader, &vk->merge_software)) goto fail;
	/* A device may expose ray queries but lack resources for the optional RT
	 * pipeline; keep the fully functional software backend in that case. */
	if (hardware_scene_layout && trace_hardware.words &&
		RCVk_PipelineLayout (vk, hardware_scene_layout, lights_layout, &vk->hardware_layout))
	{
		if (!RCVk_ComputePipeline (device, pipeline_cache, vk->hardware_layout, trace_hardware, &vk->trace_hardware) ||
			!RCVk_ComputePipeline (device, pipeline_cache, vk->hardware_layout, merge_shader, &vk->merge_hardware))
		{
			if (vk->trace_hardware) vkDestroyPipeline (device, vk->trace_hardware, NULL);
			if (vk->merge_hardware) vkDestroyPipeline (device, vk->merge_hardware, NULL);
			vkDestroyPipelineLayout (device, vk->hardware_layout, NULL);
			vk->trace_hardware = vk->merge_hardware = VK_NULL_HANDLE;
			vk->hardware_layout = VK_NULL_HANDLE;
		}
	}
	vk->needs_clear = true;
	return true;
fail:
	RCVk_Destroy (vk);
	return false;
}

bool RCVk_UpdateSparse (rc_vulkan_t *vk, uint32_t frame, const rc_sparse_t *sparse)
{
	uint32_t table_count = 0, active_count = 0, clear_count = 0;
	const rc_layout_t *layout = RC_SparseLayout (sparse);
	const uint32_t *table = RC_SparseTable (sparse, &table_count);
	const rc_sparse_brick_t *active = RC_SparseActive (sparse, &active_count);
	const rc_sparse_range_t *clears = RC_SparseClears (sparse, &clear_count);
	if (!vk || !layout || frame >= vk->frames || vk->recorded_commands || layout->record_count != vk->record_count || !vk->metadata[frame].mapped ||
		!table || table_count != vk->table_capacity || active_count > vk->table_capacity || clear_count > vk->table_capacity ||
		(active_count && !active) || (clear_count && !clears)) return false;
	rc_vk_work_t work[RC_CASCADE_COUNT];
	for (uint32_t level = 0; level < RC_CASCADE_COUNT; ++level)
	{
		if (!RC_SparseLevelWork (sparse, level, &work[level].active_base, &work[level].active_count,
			&work[level].records_per_brick, &work[level].overflow) || work[level].active_base > active_count ||
			work[level].active_count > active_count - work[level].active_base ||
			(uint64_t)work[level].active_count * work[level].records_per_brick > UINT32_MAX) return false;
	}
	for (uint32_t i = 0; i < clear_count; ++i)
		if (clears[i].first > vk->record_count || clears[i].count > vk->record_count - clears[i].first) return false;
	memcpy (vk->metadata[frame].mapped, layout->levels, sizeof (layout->levels));
	memcpy (vk->brick_table[frame].mapped, table, (size_t)table_count * sizeof (*table));
	if (active_count) memcpy (vk->active_bricks[frame].mapped, active, (size_t)active_count * sizeof (*active));
	if (clear_count) memcpy (vk->clears[frame], clears, (size_t)clear_count * sizeof (*clears));
	memcpy (vk->work[frame], work, sizeof (work));
	vk->clear_count[frame] = clear_count;
	vk->snapshot_revision[frame] = RC_SparseTopologyRevision (sparse);
	vk->snapshot_ready[frame] = true;
	return true;
}

VkDescriptorSet RCVk_DescriptorSet (const rc_vulkan_t *vk, uint32_t frame)
{
	return vk && frame < vk->frames ? vk->descriptor_sets[frame] : VK_NULL_HANDLE;
}

bool RCVk_Record (rc_vulkan_t *vk, VkCommandBuffer commands, uint32_t frame,
	VkDescriptorSet scene, VkDescriptorSet lights, bool hardware, uint32_t level, uint32_t first, uint32_t count)
{
	if (!vk || !commands || frame >= vk->frames || !vk->snapshot_ready[frame] || !scene || !lights || level >= RC_CASCADE_COUNT || !count ||
		(vk->recorded_commands && (vk->recorded_commands != commands || vk->recorded_frame != frame))) return false;
	const rc_vk_work_t *work = vk->work[frame];
	const uint32_t level_count = work[level].active_count * work[level].records_per_brick;
	if (first > level_count || count > level_count - first ||
		(count / 64u + (count % 64u != 0)) > vk->max_workgroups) return false;
	VkPipeline trace = hardware ? vk->trace_hardware : vk->trace_software;
	VkPipeline merge = hardware ? vk->merge_hardware : vk->merge_software;
	VkPipelineLayout pipeline_layout = hardware ? vk->hardware_layout : vk->software_layout;
	if (!trace || !merge || !pipeline_layout) return false;
	const bool new_recording = !vk->recorded_commands;
	uint32_t dirty_mask = new_recording ? vk->submitted_dirty_mask : vk->recorded_dirty_mask;
	if (new_recording && (vk->needs_clear || vk->clear_count[frame] ||
		vk->snapshot_revision[frame] != vk->submitted_revision))
		dirty_mask = (1u << RC_CASCADE_COUNT) - 1u;
	/* A changed sparse topology can invalidate interpolation in a coarser
	 * level even when this frame traces only a fine one. Resolve every dirty
	 * ancestor first; otherwise the finer merge can publish stale radiance
	 * through a still-valid merged record from the old topology. */
	uint32_t highest_merge = level;
	for (uint32_t i = level + 1u; i < RC_CASCADE_COUNT; ++i)
		if (dirty_mask & (1u << i)) highest_merge = i;
	for (uint32_t i = 0; i <= highest_merge; ++i)
	{
		const uint32_t merge_count = work[i].active_count * work[i].records_per_brick;
		if ((merge_count / 64u + (merge_count % 64u != 0)) > vk->max_workgroups) return false;
	}
	VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
	vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
	if (!vk->recorded_commands && (vk->needs_clear || vk->clear_count[frame]))
	{
		if (vk->needs_clear)
		{
			vkCmdFillBuffer (commands, vk->records[4].buffer, 0, vk->records[4].size, 0);
			vkCmdFillBuffer (commands, vk->records[5].buffer, 0, vk->records[5].size, 0);
		}
		else for (uint32_t i = 0; i < vk->clear_count[frame]; ++i)
		{
			const VkDeviceSize offset = (VkDeviceSize)vk->clears[frame][i].first * sizeof (uint32_t);
			const VkDeviceSize size = (VkDeviceSize)vk->clears[frame][i].count * sizeof (uint32_t);
			if (!size) continue;
			vkCmdFillBuffer (commands, vk->records[4].buffer, offset, size, 0);
			vkCmdFillBuffer (commands, vk->records[5].buffer, offset, size, 0);
		}
		VkMemoryBarrier clear = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
		/* Receivers can read raw validity records that no trace dispatch
		 * touches this frame. Make the transfer clear visible to graphics as
		 * well as compute; the later shader-write barrier cannot publish an
		 * untouched transfer-written record to the fragment shader. */
		vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 1, &clear, 0, NULL, 0, NULL);
	}
	vk->recorded_commands = commands;
	vk->recorded_frame = frame;
	if (new_recording) vk->recorded_merge_intervals = 0;
	VkDescriptorSet sets[3] = {vk->descriptor_sets[frame], scene, lights};
	vkCmdBindDescriptorSets (commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 3, sets, 0, NULL);
	VkMemoryBarrier compute = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
	if (dirty_mask && !vk->needs_clear)
	{
		/* The trace shader itself samples the previous merged level zero for
		 * multi-bounce transport. Refresh the hierarchy before tracing as well
		 * as before receiver shading; otherwise stale topology can be baked
		 * into the newly traced raw interval. A complete clear already makes
		 * every previous sample invalid, so it does not need this prepass. */
		vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_COMPUTE, merge);
		for (uint32_t i = highest_merge + 1; i-- > 0;)
		{
			const uint32_t merge_count = work[i].active_count * work[i].records_per_brick;
			if (!merge_count) continue;
			vk->recorded_merge_intervals += merge_count;
			rc_vk_push_t merge_push = {i, 0, merge_count, work[i].active_base};
			vkCmdPushConstants (commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (merge_push), &merge_push);
			vkCmdDispatch (commands, merge_count / 64u + (merge_count % 64u != 0), 1, 1);
			vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 1, &compute, 0, NULL, 0, NULL);
		}
		dirty_mask = 0;
		highest_merge = level;
	}
	rc_vk_push_t push = {level, first, count, work[level].active_base};
	vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_COMPUTE, trace);
	vkCmdPushConstants (commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push), &push);
	vkCmdDispatch (commands, count / 64u + (count % 64u != 0), 1, 1);
	vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &compute, 0, NULL, 0, NULL);
	vkCmdBindPipeline (commands, VK_PIPELINE_BIND_POINT_COMPUTE, merge);
	for (uint32_t i = highest_merge + 1; i-- > 0;)
	{
		/* Only the traced records can change at this level with stable
		 * topology and no dirty ancestor. Finer levels still need a full
		 * merge because any can depend on changed coarser radiance. */
		const bool partial = i == level && highest_merge == level && !(dirty_mask & (1u << i));
		uint32_t merge_count = partial ? count : work[i].active_count * work[i].records_per_brick;
		if (!merge_count) continue;
		vk->recorded_merge_intervals += merge_count;
		push = (rc_vk_push_t){i, partial ? first : 0, merge_count, work[i].active_base};
		vkCmdPushConstants (commands, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push), &push);
		vkCmdDispatch (commands, merge_count / 64u + (merge_count % 64u != 0), 1, 1);
		vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &compute, 0, NULL, 0, NULL);
	}
	vk->recorded_dirty_mask = dirty_mask & ~((2u << highest_merge) - 1u);
	VkMemoryBarrier ready = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
	vkCmdPipelineBarrier (commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &ready, 0, NULL, 0, NULL);
	return true;
}

void RCVk_MarkSubmitted (rc_vulkan_t *vk, VkCommandBuffer commands)
{
	if (!vk || !commands || vk->recorded_commands != commands) return;
	vk->needs_clear = false;
	vk->submitted_revision = vk->snapshot_revision[vk->recorded_frame];
	vk->submitted_dirty_mask = vk->recorded_dirty_mask;
	vk->clear_count[vk->recorded_frame] = 0;
	vk->recorded_commands = VK_NULL_HANDLE;
}

void RCVk_DiscardRecorded (rc_vulkan_t *vk, VkCommandBuffer commands)
{
	if (!vk || !commands || vk->recorded_commands != commands) return;
	vk->recorded_commands = VK_NULL_HANDLE;
}
