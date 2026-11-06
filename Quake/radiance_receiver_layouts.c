#include "radiance_receiver_layouts.h"
#include <string.h>
void RCReceiverLayouts_Destroy(rc_receiver_layouts_t *p)
{
	if (!p || !p->device) return;
	if(p->classify)vkDestroyPipelineLayout(p->device,p->classify,NULL);
	if(p->direct)vkDestroyPipelineLayout(p->device,p->direct,NULL);
	if(p->prepass)vkDestroyPipelineLayout(p->device,p->prepass,NULL);
	if(p->forward)vkDestroyPipelineLayout(p->device,p->forward,NULL);
	if(p->reconstruct)vkDestroyPipelineLayout(p->device,p->reconstruct,NULL);
	memset(p,0,sizeof(*p));
}
static bool create(VkDevice dev,VkPipelineLayout *out,const VkDescriptorSetLayout *sets,uint32_t count,uint32_t size,VkShaderStageFlags stage)
{
	VkPushConstantRange range={stage,0,size};
	VkPipelineLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=count,.pSetLayouts=sets,
		.pushConstantRangeCount=1,.pPushConstantRanges=&range};
	return vkCreatePipelineLayout(dev,&info,NULL,out)==VK_SUCCESS && *out;
}
bool RCReceiverLayouts_Create(rc_receiver_layouts_t *out,VkPhysicalDevice physical,VkDevice device,
	const rc_receiver_descriptors_t *d,const VkDescriptorSetLayout world[5],VkDescriptorSetLayout radiance,
	VkDescriptorSetLayout cascade,VkDescriptorSetLayout light,bool geometry_enabled)
{
	if(!out || !physical || !device || !d || d->device!=device || !world || !radiance || !cascade || !light) return false;
	for(unsigned i=0;i<4;++i)if(!d->layouts[i])return false;
	for(unsigned i=0;i<5;++i)if(!world[i])return false;
	VkPhysicalDeviceProperties props; VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceProperties(physical,&props); vkGetPhysicalDeviceFeatures(physical,&features);
	if(props.limits.maxBoundDescriptorSets<7 || props.limits.maxPushConstantsSize<112 || !features.geometryShader || !geometry_enabled) return false;
	/* Minimum requirements of the receiver forward set itself. */
	if(props.limits.maxPerStageDescriptorSampledImages<8 || props.limits.maxDescriptorSetSampledImages<8 ||
		props.limits.maxPerStageDescriptorSamplers<8 || props.limits.maxDescriptorSetSamplers<8 ||
		props.limits.maxPerStageDescriptorUniformBuffers<1 || props.limits.maxDescriptorSetUniformBuffers<1) return false;
	rc_receiver_layouts_t pending={.device=device};
	/* Direct and indirect receiver compute share the graphics radiance snapshot. */
	VkDescriptorSetLayout direct[2]={d->layouts[1],radiance};
	VkDescriptorSetLayout reconstruct[2]={d->layouts[3],radiance};
	VkDescriptorSetLayout graphics[7]={world[0],world[1],world[2],world[3],world[4],radiance,d->layouts[2]};
	if(!create(device,&pending.classify,&d->layouts[0],1,112,VK_SHADER_STAGE_COMPUTE_BIT) ||
		!create(device,&pending.direct,direct,2,8,VK_SHADER_STAGE_COMPUTE_BIT) ||
		!create(device,&pending.reconstruct,reconstruct,2,32,VK_SHADER_STAGE_COMPUTE_BIT) ||
		!create(device,&pending.prepass,graphics,6,88,VK_SHADER_STAGE_ALL_GRAPHICS) ||
		!create(device,&pending.forward,graphics,7,92,VK_SHADER_STAGE_ALL_GRAPHICS)) goto fail;
	RCReceiverLayouts_Destroy(out); *out=pending; return true;
fail:
	RCReceiverLayouts_Destroy(&pending); return false;
}
