/* Vulkan storage and dispatch for the world-space radiance hierarchy.
 * The renderer owns scene/light descriptors and synchronizes frame-slot reuse.
 * Destroy or recreate this object only after pending GPU work has completed. */
#ifndef QUIVER_RADIANCE_VULKAN_H
#define QUIVER_RADIANCE_VULKAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "radiance_sparse.h"

#define RC_VK_MAX_FRAMES 2u

typedef struct rc_vk_buffer_s
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	VkDeviceSize allocation_size;
	void *mapped;
} rc_vk_buffer_t;

typedef struct rc_vk_work_s
{
	uint32_t active_base, active_count, records_per_brick, overflow;
} rc_vk_work_t;

typedef struct rc_vulkan_s
{
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory_properties;
	VkDescriptorSetLayout cascade_layout;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet descriptor_sets[RC_VK_MAX_FRAMES];
	VkPipelineLayout software_layout, hardware_layout;
	VkPipeline trace_software, trace_hardware;
	VkPipeline merge_software, merge_hardware;
	rc_vk_buffer_t metadata[RC_VK_MAX_FRAMES];
	rc_vk_buffer_t brick_table[RC_VK_MAX_FRAMES], active_bricks[RC_VK_MAX_FRAMES];
	rc_vk_buffer_t records[6]; /* raw/merged radiance, distance, validity */
	rc_vk_work_t work[RC_VK_MAX_FRAMES][RC_CASCADE_COUNT];
	rc_sparse_range_t *clears[RC_VK_MAX_FRAMES];
	uint32_t clear_count[RC_VK_MAX_FRAMES];
	uint64_t snapshot_revision[RC_VK_MAX_FRAMES], submitted_revision;
	uint32_t submitted_dirty_mask, recorded_dirty_mask;
	uint64_t recorded_merge_intervals;
	uint32_t frames, record_count, table_capacity, max_workgroups;
	bool snapshot_ready[RC_VK_MAX_FRAMES];
	VkCommandBuffer recorded_commands;
	uint32_t recorded_frame;
	bool needs_clear;
} rc_vulkan_t;

typedef struct rc_vk_shader_s
{
	const uint32_t *words;
	size_t size_bytes;
} rc_vk_shader_t;

/* Both scene layouts describe set 1, lights_layout describes set 2. The
 * hardware backend is optional; no Vulkan objects are leaked on failure. */
bool RCVk_Create (rc_vulkan_t *vk, VkDevice device, const VkPhysicalDeviceMemoryProperties *memory_properties,
	const VkPhysicalDeviceLimits *limits, const rc_sparse_t *sparse, uint32_t frames,
	VkDescriptorSetLayout software_scene_layout, VkDescriptorSetLayout hardware_scene_layout,
	VkDescriptorSetLayout lights_layout, rc_vk_shader_t trace_software, rc_vk_shader_t trace_hardware,
	rc_vk_shader_t merge_shader, VkPipelineCache pipeline_cache);
void RCVk_Destroy (rc_vulkan_t *vk);

/* Copy a resolved sparse snapshot after waiting for this frame slot's fence.
 * Reused slots are invalidated on GPU before the next interval dispatch. */
bool RCVk_UpdateSparse (rc_vulkan_t *vk, uint32_t frame, const rc_sparse_t *sparse);
VkDescriptorSet RCVk_DescriptorSet (const rc_vulkan_t *vk, uint32_t frame);

/* Records one bounded interval batch and the dependent far-to-near merge.
 * Scene/light descriptors must remain alive through execution. Explicit
 * transfer/compute/graphics barriers protect shared cascade buffers. */
bool RCVk_Record (rc_vulkan_t *vk, VkCommandBuffer command_buffer, uint32_t frame,
	VkDescriptorSet scene, VkDescriptorSet lights, bool hardware, uint32_t level, uint32_t first, uint32_t count);

/* Call exactly once after submitting or discarding a command buffer containing
 * RCVk_Record. Until then, only that buffer and frame may record more work.
 * Submission must preserve queue order for the shared cascade buffers. */
void RCVk_MarkSubmitted (rc_vulkan_t *vk, VkCommandBuffer command_buffer);
void RCVk_DiscardRecorded (rc_vulkan_t *vk, VkCommandBuffer command_buffer);

#endif
