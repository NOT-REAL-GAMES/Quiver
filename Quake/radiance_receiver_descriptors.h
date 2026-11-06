#ifndef RADIANCE_RECEIVER_DESCRIPTORS_H
#define RADIANCE_RECEIVER_DESCRIPTORS_H
#include "radiance_receiver_targets.h"
typedef struct {
	VkDevice device;
	VkDescriptorSetLayout layouts[4];
	VkDescriptorPool pool;
	VkDescriptorSet classify, direct, forward, forward_resolved, reconstruct, reconstruct_reverse;
} rc_receiver_descriptors_t;
/* Caller owns targets, a nearest sampler and a 32-byte std140 viewport UBO.
 * Forward sets retain image bindings0..6 and UBO7; binding8 samples the full
 * prepass depth in DEPTH_STENCIL_READ_ONLY_OPTIMAL for opaque model culling.
 * Replacement is transactional; old sets must already be fence-idle. */
bool RCReceiverDescriptors_Create(rc_receiver_descriptors_t *out, const rc_receiver_targets_t *targets,
	VkSampler nearest, VkBuffer viewport_buffer, VkDeviceSize viewport_offset, VkDeviceSize viewport_range);
void RCReceiverDescriptors_Destroy(rc_receiver_descriptors_t *descriptors);
#endif
