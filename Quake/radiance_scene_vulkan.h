/* Frame-fenced GPU snapshots of the radiance scene BVH and triangle data. */
#ifndef QUIVER_RADIANCE_SCENE_VULKAN_H
#define QUIVER_RADIANCE_SCENE_VULKAN_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "radiance_scene.h"

#define RC_SCENE_VK_MAX_FRAMES 2u

typedef struct rc_scene_vk_buffer_s
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
	void *shadow;
	VkDeviceSize capacity;
	VkDeviceSize allocation_size;
	VkDeviceSize initialized;
	uint32_t remap_count;
} rc_scene_vk_buffer_t;

typedef struct rc_scene_vk_s
{
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory_properties;
	VkDeviceSize max_range;
	uint32_t frames;
	VkDescriptorSetLayout software_layout, hardware_layout;
	VkDescriptorPool software_pool, hardware_pool;
	VkDescriptorSet software_sets[RC_SCENE_VK_MAX_FRAMES], hardware_sets[RC_SCENE_VK_MAX_FRAMES];
	rc_scene_vk_buffer_t buffers[RC_SCENE_VK_MAX_FRAMES][4];
	bool software_ready[RC_SCENE_VK_MAX_FRAMES], hardware_ready[RC_SCENE_VK_MAX_FRAMES];
	VkDeviceSize last_upload_bytes[RC_SCENE_VK_MAX_FRAMES];
#ifdef VK_KHR_acceleration_structure
	VkAccelerationStructureKHR accelerations[RC_SCENE_VK_MAX_FRAMES];
#endif
} rc_scene_vk_t;

/* Request a hardware layout only when acceleration-structure support was
 * enabled on the device. Failure of that optional layout keeps software. */
bool RCSceneVk_Create (rc_scene_vk_t *gpu, VkDevice device, const VkPhysicalDeviceMemoryProperties *memory_properties,
	const VkPhysicalDeviceLimits *limits, uint32_t frames, bool hardware_supported);
/* The caller must wait for this frame slot's fence before updating it. */
bool RCSceneVk_Update (rc_scene_vk_t *gpu, uint32_t frame, const rc_scene_t *scene);
/* Append an identity primitive remap for split hardware BLAS instances. */
bool RCSceneVk_UpdateSplit (rc_scene_vk_t *gpu, uint32_t frame, const rc_scene_t *scene);
#ifdef VK_KHR_acceleration_structure
bool RCSceneVk_SetAcceleration (rc_scene_vk_t *gpu, uint32_t frame, VkAccelerationStructureKHR acceleration);
#endif
VkDescriptorSet RCSceneVk_Descriptor (const rc_scene_vk_t *gpu, uint32_t frame, bool hardware);
void RCSceneVk_Destroy (rc_scene_vk_t *gpu);

#endif
