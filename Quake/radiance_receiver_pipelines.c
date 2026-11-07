#include "radiance_receiver_pipelines.h"
#include "radiance_receiver_targets.h"
#include <string.h>

void RCReceiverPipelines_Destroy(rc_receiver_pipelines_t *p)
{
	if (!p || !p->device) return;
	if (p->raster) vkDestroyPipeline(p->device,p->raster,NULL);
	if (p->classify) vkDestroyPipeline(p->device,p->classify,NULL);
	if (p->direct) vkDestroyPipeline(p->device,p->direct,NULL);
	if (p->resolve) vkDestroyPipeline(p->device,p->resolve,NULL);
	memset(p,0,sizeof(*p));
}

bool RCReceiverPipelines_Create(rc_receiver_pipelines_t *out, VkDevice device, VkPipelineCache cache,
	const VkGraphicsPipelineCreateInfo *raster, const VkComputePipelineCreateInfo *classify,
	const VkComputePipelineCreateInfo *direct)
{
	if (!out || !device || !raster || !classify || !direct || !raster->renderPass || raster->subpass ||
		!raster->layout || !classify->layout || !direct->layout || !raster->pMultisampleState ||
		!RCReceiverTargets_ValidSamples(raster->pMultisampleState->rasterizationSamples) ||
		!raster->pDepthStencilState || !raster->pDepthStencilState->depthTestEnable ||
		!raster->pDepthStencilState->depthWriteEnable || !raster->pColorBlendState ||
		raster->pColorBlendState->attachmentCount != 2 || !raster->pColorBlendState->pAttachments ||
		raster->pColorBlendState->pAttachments[0].blendEnable || raster->pColorBlendState->pAttachments[1].blendEnable)
		return false;
	rc_receiver_pipelines_t pending={.device=device,.raster_pass=raster->renderPass};
	if (vkCreateGraphicsPipelines(device,cache,1,raster,NULL,&pending.raster)!=VK_SUCCESS || !pending.raster) goto fail;
	if (vkCreateComputePipelines(device,cache,1,classify,NULL,&pending.classify)!=VK_SUCCESS || !pending.classify) goto fail;
	if (vkCreateComputePipelines(device,cache,1,direct,NULL,&pending.direct)!=VK_SUCCESS || !pending.direct) goto fail;
	RCReceiverPipelines_Destroy(out); *out=pending; return true;
fail:
	RCReceiverPipelines_Destroy(&pending); return false;
}
