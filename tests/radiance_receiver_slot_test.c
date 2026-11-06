#include "radiance_receiver_slot.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H(type,n) ((type)(uintptr_t)(n))
static bool fence_ready=true, fail_shaders,fail_resources,fail_postprocess,created_filter;
static unsigned shader_calls,resource_calls,shader_destroy,resource_destroy,alternate_destroy,soft_calls,soft_destroy;
static bool fail_soft;
void RCReceiverModelAlternates_Destroy(rc_receiver_model_alternates_t *a) { assert(!a->device);memset(a,0,sizeof(*a)); }
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice d,VkFence f) { (void)d;(void)f;return fence_ready?VK_SUCCESS:VK_NOT_READY; }
bool RCReceiverShaders_Create(rc_receiver_shaders_t *s,VkDevice d,bool h,bool texture_filter)
{ (void)h;created_filter=texture_filter;++shader_calls;if(fail_shaders)return false;*s=(rc_receiver_shaders_t){.device=d,.vertex=H(VkShaderModule,1),.prepass=H(VkShaderModule,2),.classify=H(VkShaderModule,3),.direct=H(VkShaderModule,4),.forward=H(VkShaderModule,5),.reconstruct=H(VkShaderModule,6)};return true; }
bool RCReceiverShaders_CreateSoftPipeline(VkPipeline *out,VkDevice d,VkPipelineCache cache,VkPipelineLayout layout,bool hardware)
{ (void)d;(void)cache;(void)hardware;assert(layout==H(VkPipelineLayout,5) && !*out);++soft_calls;if(fail_soft)return false;*out=H(VkPipeline,11);return true; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo *info,const VkAllocationCallbacks *a,VkPipeline *out)
{ (void)d;(void)c;(void)a;assert(n==1 && info->stage.module==H(VkShaderModule,6));if(fail_postprocess)return VK_ERROR_OUT_OF_HOST_MEMORY;*out=H(VkPipeline,10);return VK_SUCCESS; }
void RCReceiverShaders_Destroy(rc_receiver_shaders_t *s){if(s->device)++shader_destroy;memset(s,0,sizeof(*s));}
void RCReceiverAlternates_Destroy(rc_receiver_alternates_t *a){if(a->device)++alternate_destroy;memset(a,0,sizeof(*a));}
bool RCReceiverResources_Create(rc_receiver_resources_t *out,VkPhysicalDevice ph,VkDevice dev,const VkPhysicalDeviceMemoryProperties *memory,uint32_t w,uint32_t h,VkSampleCountFlagBits samples,VkSampler sampler,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize range,const VkDescriptorSetLayout world[5],VkDescriptorSetLayout radiance,VkDescriptorSetLayout scene,VkDescriptorSetLayout light,bool geometry,VkPipelineCache cache,const VkGraphicsPipelineCreateInfo pre[2],const VkGraphicsPipelineCreateInfo fwd[4],const VkComputePipelineCreateInfo *c,const VkComputePipelineCreateInfo *d)
{
	(void)ph;(void)memory;(void)w;(void)h;(void)samples;(void)sampler;(void)buffer;(void)offset;(void)range;(void)world;(void)radiance;(void)scene;(void)light;(void)geometry;(void)cache;
	++resource_calls; assert(pre[0].pStages[0].module==H(VkShaderModule,1) && pre[0].pStages[1].module==H(VkShaderModule,2));
	assert(fwd[3].pStages[0].module==H(VkShaderModule,1) && fwd[3].pStages[1].module==H(VkShaderModule,5));
	assert(c->stage.module==H(VkShaderModule,3) && d->stage.module==H(VkShaderModule,4));
	if(fail_resources)return false;
	out->device=dev;out->layouts.direct=H(VkPipelineLayout,5);return true;
}
void RCReceiverResources_Destroy(rc_receiver_resources_t *r){if(r->device)++resource_destroy;if(r->soft_shadows)++soft_destroy;memset(r,0,sizeof(*r));}
int main(void)
{
	VkPipelineShaderStageCreateInfo stages[2]={{.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=H(VkShaderModule,99)},{.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=H(VkShaderModule,99)}};
	rc_receiver_slot_config_t c={.physical=H(VkPhysicalDevice,1),.device=H(VkDevice,1),.revision=1,.width=640,.height=480,.samples=VK_SAMPLE_COUNT_1_BIT,.geometry_enabled=true};
	for(unsigned i=0;i<2;++i)c.prepass[i]=(VkGraphicsPipelineCreateInfo){.stageCount=2,.pStages=stages};
	for(unsigned i=0;i<4;++i)c.forward[i]=(VkGraphicsPipelineCreateInfo){.stageCount=2,.pStages=stages};
	c.classify.stage.stage=c.direct.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
	rc_receiver_slot_t slot={0}; assert(RCReceiverSlot_Prepare(&slot,&c));assert(slot.ready && shader_calls==1 && resource_calls==1 && stages[0].module==H(VkShaderModule,99));
	slot.alternates.device=c.device; assert(RCReceiverSlot_Prepare(&slot,&c) && shader_calls==1 && alternate_destroy==1);
	assert(RCReceiverSlot_MarkSubmitted(&slot,H(VkFence,1))); fence_ready=false;
	c.revision=2; assert(!RCReceiverSlot_Prepare(&slot,&c) && !RCReceiverSlot_Release(&slot) && slot.revision==1);
	fence_ready=true; fail_resources=true; assert(!RCReceiverSlot_Prepare(&slot,&c) && slot.ready && slot.revision==1); fail_resources=false;
	c.hardware=true; assert(!RCReceiverSlot_Prepare(&slot,&c)); c.ray_query_enabled=true;
	assert(RCReceiverSlot_Prepare(&slot,&c) && slot.revision==2 && slot.hardware);
	unsigned previous_calls=shader_calls;
	c.texture_filter=true; fail_shaders=true;
	assert(!RCReceiverSlot_Prepare(&slot,&c) && slot.ready && !slot.texture_filter); fail_shaders=false;
	fail_resources=true; assert(!RCReceiverSlot_Prepare(&slot,&c) && slot.ready && !slot.texture_filter); fail_resources=false;
	fail_postprocess=true; assert(!RCReceiverSlot_Prepare(&slot,&c) && slot.ready && !slot.texture_filter); fail_postprocess=false;
	assert(RCReceiverSlot_Prepare(&slot,&c) && slot.texture_filter && created_filter && shader_calls==previous_calls+4);
	previous_calls=shader_calls;
	assert(RCReceiverSlot_Prepare(&slot,&c) && shader_calls==previous_calls);
	assert(!soft_calls); /* Default preparation never creates the optional module/pipeline. */
	c.soft_shadows=true;fail_soft=true;
	assert(RCReceiverSlot_Prepare(&slot,&c) && slot.ready && soft_calls==1 && !slot.resources.soft_shadows);
	assert(RCReceiverSlot_Prepare(&slot,&c) && soft_calls==1); /* Cache optional failure. */
	c.soft_shadows=false;assert(RCReceiverSlot_Prepare(&slot,&c) && soft_calls==1);
	c.soft_shadows=true;fail_soft=false;
	assert(RCReceiverSlot_Prepare(&slot,&c) && soft_calls==2 && slot.resources.soft_shadows);
	c.soft_shadows=false;assert(RCReceiverSlot_Prepare(&slot,&c) && soft_calls==2 && slot.resources.soft_shadows);
	c.soft_shadows=true;assert(RCReceiverSlot_Prepare(&slot,&c) && soft_calls==2 && shader_calls==previous_calls);
	c.texture_filter=false;
	assert(RCReceiverSlot_Prepare(&slot,&c) && !slot.texture_filter && !created_filter && shader_calls==previous_calls+1 && soft_calls==2 && soft_destroy==1);
	assert(RCReceiverSlot_Release(&slot) && !slot.ready && shader_destroy>=2 && resource_destroy>=2);
	puts("PASS: frame slot preserves in-flight resources and transactionally replaces validated shader bundles"); return 0;
}
