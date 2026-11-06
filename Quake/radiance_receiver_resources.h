#ifndef RADIANCE_RECEIVER_RESOURCES_H
#define RADIANCE_RECEIVER_RESOURCES_H
#include "radiance_receiver_pipelines.h"
#include "radiance_receiver_layouts.h"
#include "radiance_receiver_model.h"
typedef struct {
	VkDevice device;
	rc_receiver_targets_t targets;
	rc_receiver_descriptors_t descriptors;
	rc_receiver_layouts_t layouts;
	rc_receiver_pipelines_t pipelines;
	VkPipeline prepass_cutout, forward[4], reconstruct, soft_shadows;
	rc_receiver_model_pipelines_t models;
} rc_receiver_resources_t;
/* Every Vk*CreateInfo and shader module is borrowed only during creation.
 * The prepass infos target the private pass; forward infos target a compatible
 * scene pass. The viewport UBO, sampler and external set layouts remain caller
 * owned. Destruction/replacement requires all referencing frames fence-idle. */
bool RCReceiverResources_Create(rc_receiver_resources_t *out,VkPhysicalDevice physical,VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory,uint32_t width,uint32_t height,VkSampleCountFlagBits samples,
	VkSampler nearest,VkBuffer viewport_buffer,VkDeviceSize viewport_offset,VkDeviceSize viewport_range,
	const VkDescriptorSetLayout world[5],VkDescriptorSetLayout radiance,VkDescriptorSetLayout cascade,
	VkDescriptorSetLayout light,bool geometry_enabled,VkPipelineCache cache,
	const VkGraphicsPipelineCreateInfo prepass[2],const VkGraphicsPipelineCreateInfo forward[4],
	const VkComputePipelineCreateInfo *classify,const VkComputePipelineCreateInfo *direct);
void RCReceiverResources_Destroy(rc_receiver_resources_t *resources);
#endif
