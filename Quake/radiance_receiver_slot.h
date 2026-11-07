#ifndef RADIANCE_RECEIVER_SLOT_H
#define RADIANCE_RECEIVER_SLOT_H
#include "radiance_receiver_resources.h"
#include "radiance_receiver_shaders.h"
#include "radiance_receiver_alternates.h"
/* A slot owns every receiver resource referenced by one set of submitted frames.
 * The caller owns external layouts, sampler, UBO, command pool and fence. */
typedef struct {
	VkDevice device;
	VkFence last_fence;
	uint64_t revision;
	uint32_t width,height;
	VkSampleCountFlagBits samples;
	bool hardware,texture_filter,full_light,ready;
	bool soft_shadow_requested,soft_shadow_attempted;
	rc_receiver_shaders_t shaders;
	rc_receiver_resources_t resources;
	rc_receiver_alternates_t alternates;
	rc_receiver_model_alternates_t model_alternates;
} rc_receiver_slot_t;
typedef struct {
	VkPhysicalDevice physical; VkDevice device; const VkPhysicalDeviceMemoryProperties *memory;
	uint64_t revision; uint32_t width,height; VkSampleCountFlagBits samples;
	bool hardware,texture_filter,full_light,geometry_enabled,ray_query_enabled,soft_shadows;
	VkSampler nearest; VkBuffer viewport_buffer; VkDeviceSize viewport_offset,viewport_range;
	VkDescriptorSetLayout world[5],radiance,cascade,light;
	VkPipelineCache cache;
	/* Exactly a vertex+fragment pair per graphics variant. Stage info, state
	 * pointers and external handles remain valid for this call only. */
	VkGraphicsPipelineCreateInfo prepass[2],forward[4];
	VkComputePipelineCreateInfo classify,direct;
} rc_receiver_slot_config_t;
/* revision must change for every dependency change, including sampler, UBO,
 * layouts, scene render pass, shaders, pipeline state and video restart.
 * Busy or failed slots preserve their old resources and return false. */
bool RCReceiverSlot_Prepare(rc_receiver_slot_t *slot,const rc_receiver_slot_config_t *config);
/* Associate the last submitted use with its fence before that fence is reset or
 * reused. Null fences are rejected. Never call on a slot still in flight. */
bool RCReceiverSlot_MarkSubmitted(rc_receiver_slot_t *slot,VkFence fence);
/* Nonblocking; returns false without destruction when the fence is not ready. */
bool RCReceiverSlot_Release(rc_receiver_slot_t *slot);
#endif
