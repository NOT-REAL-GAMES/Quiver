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
	/* Depth, normal and ID at the prepass sample count. Unused at one sample. */
	rc_receiver_image_t multisampled[3];
	/* The prepass. Multisampled targets add a second pass that collapses them
	 * into the first three images, which every later stage reads. */
	VkRenderPass pass, resolve_pass;
	VkFramebuffer framebuffer, resolve_framebuffer;
	VkSampleCountFlagBits samples;
	uint32_t width, height;
} rc_receiver_targets_t;

/* Caller must ensure old targets are no longer in flight before replacement or
 * destruction. On failure the old target remains untouched. samples is 1, 2, 4
 * or 8; the classifier rebuilds positions from the standard sample locations. */
bool RCReceiverTargets_Create(rc_receiver_targets_t *out, VkPhysicalDevice physical, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory, uint32_t width, uint32_t height, VkSampleCountFlagBits samples);
void RCReceiverTargets_Destroy(rc_receiver_targets_t *targets);
/* One device-local image with its view; only t->device is read. */
bool RCReceiverTargets_Image(rc_receiver_targets_t *t, VkPhysicalDevice physical,
	const VkPhysicalDeviceMemoryProperties *memory, rc_receiver_image_t *image, VkFormat format,
	VkImageUsageFlags usage, VkFormatFeatureFlags required, VkImageAspectFlags aspect,
	uint32_t width, uint32_t height, VkSampleCountFlagBits samples);
static inline bool RCReceiverTargets_ValidSamples(uint32_t samples)
{
	return samples==1 || samples==2 || samples==4 || samples==8;
}
#endif
