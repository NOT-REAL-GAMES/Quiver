/* Combined frame-local graphics descriptor set for radiance receivers. */
#ifndef QUIVER_RADIANCE_RECEIVER_VULKAN_H
#define QUIVER_RADIANCE_RECEIVER_VULKAN_H

#include "radiance_vulkan.h"
#include "radiance_scene_vulkan.h"
#include "radiance_gpu_inputs.h"

typedef struct rc_receiver_vk_s
{
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory_properties;
	uint32_t frames;
	VkDescriptorSetLayout software_layout, hardware_layout;
	VkDescriptorPool software_pool, hardware_pool;
	VkDescriptorSet software_sets[RC_VK_MAX_FRAMES], hardware_sets[RC_VK_MAX_FRAMES];
	rc_vk_buffer_t inverse_view_projection[RC_VK_MAX_FRAMES];
	bool software_ready[RC_VK_MAX_FRAMES], hardware_ready[RC_VK_MAX_FRAMES];
} rc_receiver_vk_t;

bool RCReceiverVk_Create (rc_receiver_vk_t *receiver, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory_properties, const VkPhysicalDeviceLimits *limits,
	uint32_t frames, bool hardware_supported);
/* All three input snapshots and this frame's resources must be fenced before
 * updating. view_projection is column-major, as in vulkan_globals. */
bool RCReceiverVk_Update (rc_receiver_vk_t *receiver, uint32_t frame, const rc_vulkan_t *cascades,
	const rc_scene_vk_t *scene, const radiance_gpu_inputs_t *lighting, const float view_projection[16]);
VkDescriptorSet RCReceiverVk_Descriptor (const rc_receiver_vk_t *receiver, uint32_t frame, bool hardware);
void RCReceiverVk_Destroy (rc_receiver_vk_t *receiver);

#endif
