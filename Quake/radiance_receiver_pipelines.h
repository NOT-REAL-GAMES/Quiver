#ifndef RADIANCE_RECEIVER_PIPELINES_H
#define RADIANCE_RECEIVER_PIPELINES_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
typedef struct {
	VkDevice device;
	VkRenderPass raster_pass;
	VkPipeline raster, classify, direct;
	VkPipeline resolve; /* collapses multisampled targets; null at one sample */
} rc_receiver_pipelines_t;
/* Pipeline layouts, shader modules and render pass are borrowed from the
 * caller. Replacement/destruction is only safe after the old frame fences. */
bool RCReceiverPipelines_Create(rc_receiver_pipelines_t *out, VkDevice device, VkPipelineCache cache,
	const VkGraphicsPipelineCreateInfo *raster, const VkComputePipelineCreateInfo *classify,
	const VkComputePipelineCreateInfo *direct);
void RCReceiverPipelines_Destroy(rc_receiver_pipelines_t *pipelines);
#endif
