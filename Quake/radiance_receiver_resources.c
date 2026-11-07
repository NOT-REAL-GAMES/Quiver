#include "radiance_receiver_resources.h"
#include <string.h>
void RCReceiverResources_Destroy(rc_receiver_resources_t *r)
{
	if (!r || !r->device) return;
	RCReceiverLight_Destroy(&r->light);
	RCReceiverModelPipelines_Destroy(&r->models);
	for(unsigned i=0;i<4;++i)if(r->forward[i])vkDestroyPipeline(r->device,r->forward[i],NULL);
	if(r->prepass_cutout)vkDestroyPipeline(r->device,r->prepass_cutout,NULL);
	if(r->reconstruct)vkDestroyPipeline(r->device,r->reconstruct,NULL);
	if(r->reconstruct_whole)vkDestroyPipeline(r->device,r->reconstruct_whole,NULL);
	if(r->soft_shadows)vkDestroyPipeline(r->device,r->soft_shadows,NULL);
	RCReceiverPipelines_Destroy(&r->pipelines);
	RCReceiverLayouts_Destroy(&r->layouts);
	RCReceiverDescriptors_Destroy(&r->descriptors);
	RCReceiverTargets_Destroy(&r->targets);
	memset(r,0,sizeof(*r));
}
static bool create_graphics(VkDevice device,VkPipelineCache cache,VkPipeline *out,VkGraphicsPipelineCreateInfo info,VkRenderPass pass,VkPipelineLayout layout,uint32_t subpass)
{
	info.renderPass=pass; info.layout=layout; info.subpass=subpass;
	return vkCreateGraphicsPipelines(device,cache,1,&info,NULL,out)==VK_SUCCESS && *out;
}
static bool dynamic_draw_state(const VkGraphicsPipelineCreateInfo *info)
{
	if (!info->pRasterizationState || !info->pRasterizationState->depthBiasEnable || !info->pDynamicState ||
		!info->pDynamicState->pDynamicStates) return false;
	bool viewport=false,scissor=false,bias=false;
	for(uint32_t i=0;i<info->pDynamicState->dynamicStateCount;++i) {
		VkDynamicState state=info->pDynamicState->pDynamicStates[i];
		viewport |= state==VK_DYNAMIC_STATE_VIEWPORT;
		scissor |= state==VK_DYNAMIC_STATE_SCISSOR;
		bias |= state==VK_DYNAMIC_STATE_DEPTH_BIAS;
	}
	return viewport && scissor && bias;
}
bool RCReceiverResources_Create(rc_receiver_resources_t *out,VkPhysicalDevice physical,VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory,uint32_t width,uint32_t height,VkSampleCountFlagBits samples,
	VkSampler nearest,VkBuffer viewport_buffer,VkDeviceSize viewport_offset,VkDeviceSize viewport_range,
	const VkDescriptorSetLayout world[5],VkDescriptorSetLayout radiance,VkDescriptorSetLayout cascade,
	VkDescriptorSetLayout light,bool geometry_enabled,VkPipelineCache cache,
	const VkGraphicsPipelineCreateInfo prepass[2],const VkGraphicsPipelineCreateInfo forward[4],
	const VkComputePipelineCreateInfo *classify,const VkComputePipelineCreateInfo *direct)
{
	if(!out || !device || !prepass || !forward || !classify || !direct || !RCReceiverTargets_ValidSamples(samples)) return false;
	for(unsigned i=0;i<2;++i) if(!dynamic_draw_state(&prepass[i]) || !prepass[i].pMultisampleState || prepass[i].pMultisampleState->rasterizationSamples!=samples ||
		!prepass[i].pDepthStencilState || !prepass[i].pDepthStencilState->depthTestEnable || !prepass[i].pDepthStencilState->depthWriteEnable ||
		!prepass[i].pColorBlendState || prepass[i].pColorBlendState->attachmentCount!=2 || !prepass[i].pColorBlendState->pAttachments ||
		prepass[i].pColorBlendState->pAttachments[0].blendEnable || prepass[i].pColorBlendState->pAttachments[1].blendEnable) return false;
	for(unsigned i=0;i<4;++i) if(!dynamic_draw_state(&forward[i]) || !forward[i].renderPass || !forward[i].pMultisampleState ||
		!forward[i].pDepthStencilState || !forward[i].pDepthStencilState->depthTestEnable || !forward[i].pDepthStencilState->depthWriteEnable ||
		!forward[i].pColorBlendState || forward[i].pColorBlendState->attachmentCount!=1 || !forward[i].pColorBlendState->pAttachments ||
		forward[i].pColorBlendState->pAttachments[0].blendEnable) return false;
	rc_receiver_resources_t r={.device=device};
	if(!RCReceiverTargets_Create(&r.targets,physical,device,memory,width,height,samples) ||
		!RCReceiverDescriptors_Create(&r.descriptors,&r.targets,nearest,viewport_buffer,viewport_offset,viewport_range) ||
		!RCReceiverLayouts_Create(&r.layouts,physical,device,&r.descriptors,world,radiance,cascade,light,geometry_enabled)) goto fail;
	VkGraphicsPipelineCreateInfo raster=prepass[0]; raster.renderPass=r.targets.pass; raster.layout=r.layouts.prepass; raster.subpass=0;
	VkComputePipelineCreateInfo c=*classify, d=*direct; c.layout=r.layouts.classify; d.layout=r.layouts.direct;
	if(!RCReceiverPipelines_Create(&r.pipelines,device,cache,&raster,&c,&d) ||
		!create_graphics(device,cache,&r.prepass_cutout,prepass[1],r.targets.pass,r.layouts.prepass,0)) goto fail;
	for(unsigned i=0;i<4;++i)
		if(!create_graphics(device,cache,&r.forward[i],forward[i],forward[i].renderPass,r.layouts.forward,forward[i].subpass)) goto fail;
	RCReceiverResources_Destroy(out); *out=r; return true;
fail:
	RCReceiverResources_Destroy(&r); return false;
}
