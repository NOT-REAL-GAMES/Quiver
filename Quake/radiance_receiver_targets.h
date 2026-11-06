#ifndef RADIANCE_RECEIVER_TARGETS_H
#define RADIANCE_RECEIVER_TARGETS_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

enum {
	RC_RECEIVER_DEPTH, RC_RECEIVER_NORMAL, RC_RECEIVER_ID,
	RC_RECEIVER_HALF_POSITION, RC_RECEIVER_HALF_NORMAL, RC_RECEIVER_HALF_STATUS,
	RC_RECEIVER_HALF_INDIRECT, RC_RECEIVER_HALF_DIRECT, RC_RECEIVER_HALF_MINIMUM,
	RC_RECEIVER_RESOLVED_INDIRECT, RC_RECEIVER_RESOLVED_DIRECT, RC_RECEIVER_RESOLVED_MINIMUM,
	RC_RECEIVER_TARGET_COUNT
};
typedef struct {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
} rc_receiver_image_t;
typedef struct {
	VkDevice device;
	rc_receiver_image_t images[RC_RECEIVER_TARGET_COUNT];
	VkRenderPass pass;
	VkFramebuffer framebuffer;
	uint32_t width, height;
} rc_receiver_targets_t;

/* Caller must ensure old targets are no longer in flight before replacement or
 * destruction. On failure the old target remains untouched. Multisampling is
 * deliberately unsupported until actual sample locations are reconstructed. */
bool RCReceiverTargets_Create(rc_receiver_targets_t *out, VkPhysicalDevice physical, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory, uint32_t width, uint32_t height, VkSampleCountFlagBits samples);
void RCReceiverTargets_Destroy(rc_receiver_targets_t *targets);
#endif
