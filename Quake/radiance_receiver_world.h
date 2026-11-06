#ifndef RADIANCE_RECEIVER_WORLD_H
#define RADIANCE_RECEIVER_WORLD_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
/* Direct, single-instance indexed opaque world draws only. The draw list must
 * be a frame-stable snapshot with the same buffers, transforms, alpha-test
 * state and primitive order as the matching forward draws. Tokens must be strictly increasing within this list and must also be
 * supplied to those forward draws for safe per-pixel receiver ID comparisons.
 * Indirect, mesh/task, transparent, liquids, and unresolved draws must keep the
 * full-resolution path. All handles and buffer ranges are caller-owned. */
typedef struct {
	VkBuffer vertex_buffer, index_buffer;
	VkDeviceSize vertex_offset, index_offset;
	VkIndexType index_type;
	uint32_t index_count, first_index, token;
	int32_t vertex_offset_index;
	VkDescriptorSet diffuse_set, instance_set, camera_set;
	float mvp[16];
	uint32_t instance_base;
	bool alpha_test;
	float depth_bias_constant, depth_bias_slope;
} rc_receiver_world_draw_t;
bool RCReceiverWorld_CanRecord(VkCommandBuffer command, VkPipelineLayout layout,
	const VkPipeline pipelines[2], const rc_receiver_world_draw_t *draws, uint32_t count);
/* Call inside the receiver render pass after the full list has passed preflight.
 * Token uniqueness must be maintained across every participating renderer. */
void RCReceiverWorld_Record(VkCommandBuffer command, VkPipelineLayout layout,
	const VkPipeline pipelines[2], const rc_receiver_world_draw_t *draws, uint32_t count);
#endif
