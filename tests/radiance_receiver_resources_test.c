#include "radiance_receiver_resources.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned call, fail, destroyed;
void RCReceiverModelPipelines_Destroy(rc_receiver_model_pipelines_t *p) { assert(!p->device);memset(p,0,sizeof(*p)); }
bool RCReceiverTargets_Create(rc_receiver_targets_t *o,VkPhysicalDevice p,VkDevice d,const VkPhysicalDeviceMemoryProperties *m,uint32_t w,uint32_t h,VkSampleCountFlagBits s)
{ (void)p;(void)m;(void)w;(void)h;(void)s; if(++call==fail)return false; o->device=d;o->pass=H(VkRenderPass,7);return true; }
void RCReceiverTargets_Destroy(rc_receiver_targets_t *o) { if(o->device)++destroyed; memset(o,0,sizeof(*o)); }
bool RCReceiverDescriptors_Create(rc_receiver_descriptors_t *o,const rc_receiver_targets_t *t,VkSampler s,VkBuffer b,VkDeviceSize a,VkDeviceSize z)
{ (void)s;(void)b;(void)a;(void)z; if(++call==fail)return false; o->device=t->device;return true; }
void RCReceiverDescriptors_Destroy(rc_receiver_descriptors_t *o) { if(o->device)++destroyed; memset(o,0,sizeof(*o)); }
bool RCReceiverLayouts_Create(rc_receiver_layouts_t *o,VkPhysicalDevice p,VkDevice d,const rc_receiver_descriptors_t *desc,const VkDescriptorSetLayout w[5],VkDescriptorSetLayout r,VkDescriptorSetLayout s,VkDescriptorSetLayout l,bool g)
{ (void)p;(void)desc;(void)w;(void)r;(void)s;(void)l;(void)g; if(++call==fail)return false; o->device=d;o->prepass=H(VkPipelineLayout,1);o->forward=H(VkPipelineLayout,2);o->classify=H(VkPipelineLayout,3);o->direct=H(VkPipelineLayout,4);return true; }
void RCReceiverLayouts_Destroy(rc_receiver_layouts_t *o) { if(o->device)++destroyed; memset(o,0,sizeof(*o)); }
bool RCReceiverPipelines_Create(rc_receiver_pipelines_t *o,VkDevice d,VkPipelineCache cache,const VkGraphicsPipelineCreateInfo *g,const VkComputePipelineCreateInfo *c,const VkComputePipelineCreateInfo *i)
{ (void)cache; if(++call==fail)return false; assert(g->renderPass==H(VkRenderPass,7) && g->layout==H(VkPipelineLayout,1) && c->layout==H(VkPipelineLayout,3) && i->layout==H(VkPipelineLayout,4));o->device=d;o->raster=H(VkPipeline,1);return true; }
void RCReceiverPipelines_Destroy(rc_receiver_pipelines_t *o) { if(o->device)++destroyed; memset(o,0,sizeof(*o)); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkGraphicsPipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *out)
{ (void)d;(void)c;(void)a;assert(n==1);if(++call==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; if(call==5)assert(i->renderPass==H(VkRenderPass,7) && i->layout==H(VkPipelineLayout,1));else assert(i->renderPass==H(VkRenderPass,8) && i->layout==H(VkPipelineLayout,2) && i->subpass==2); *out=H(VkPipeline,call);return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a)
{ (void)d;(void)p;(void)a;++destroyed; }
void RCReceiverLight_Destroy(rc_receiver_light_t *l) { assert(!l->device); }
int main(void)
{
	VkPipelineMultisampleStateCreateInfo ms={.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
	VkPipelineDepthStencilStateCreateInfo depth={.depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE};
	VkPipelineColorBlendAttachmentState blend[2]={0}; VkPipelineColorBlendStateCreateInfo color={.attachmentCount=2,.pAttachments=blend}, fcolor={.attachmentCount=1,.pAttachments=blend};
	VkGraphicsPipelineCreateInfo pre[2]={0},forward[4]={0};
	VkPipelineRasterizationStateCreateInfo raster={.depthBiasEnable=VK_TRUE};
	VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS};
	VkPipelineDynamicStateCreateInfo dynamic={.dynamicStateCount=3,.pDynamicStates=states};
	for(unsigned i=0;i<2;++i)pre[i]=(VkGraphicsPipelineCreateInfo){.pMultisampleState=&ms,.pDepthStencilState=&depth,.pColorBlendState=&color,.pRasterizationState=&raster,.pDynamicState=&dynamic};
	for(unsigned i=0;i<4;++i)forward[i]=(VkGraphicsPipelineCreateInfo){.renderPass=H(VkRenderPass,8),.subpass=2,.pMultisampleState=&ms,.pDepthStencilState=&depth,.pColorBlendState=&fcolor,.pRasterizationState=&raster,.pDynamicState=&dynamic};
	VkComputePipelineCreateInfo c={0}; rc_receiver_resources_t out={0};
	dynamic.dynamicStateCount=2;
	assert(!RCReceiverResources_Create(&out,0,H(VkDevice,1),0,10,10,VK_SAMPLE_COUNT_1_BIT,0,0,0,32,0,0,0,0,true,0,pre,forward,&c,&c) && !call);
	dynamic.dynamicStateCount=3;
	for(unsigned i=1;i<=9;++i) { call=destroyed=0;fail=i; assert(!RCReceiverResources_Create(&out,0,H(VkDevice,1),0,10,10,VK_SAMPLE_COUNT_1_BIT,0,0,0,32,0,0,0,0,true,0,pre,forward,&c,&c)); assert(!out.device && destroyed==i-1); }
	call=fail=0; assert(RCReceiverResources_Create(&out,0,H(VkDevice,1),0,10,10,VK_SAMPLE_COUNT_1_BIT,0,0,0,32,0,0,0,0,true,0,pre,forward,&c,&c)); assert(out.forward[3]);
	call=0;fail=7; assert(!RCReceiverResources_Create(&out,0,H(VkDevice,1),0,10,10,VK_SAMPLE_COUNT_1_BIT,0,0,0,32,0,0,0,0,true,0,pre,forward,&c,&c)); assert(out.forward[3]);
	/* The prepass may be multisampled, but its pipelines must match the targets. */
	assert(!RCReceiverResources_Create(&out,0,H(VkDevice,1),0,10,10,VK_SAMPLE_COUNT_4_BIT,0,0,0,32,0,0,0,0,true,0,pre,forward,&c,&c));
	assert(out.forward[3]);
	ms.rasterizationSamples=VK_SAMPLE_COUNT_4_BIT; call=fail=0;
	assert(RCReceiverResources_Create(&out,0,H(VkDevice,1),0,10,10,VK_SAMPLE_COUNT_4_BIT,0,0,0,32,0,0,0,0,true,0,pre,forward,&c,&c));
	assert(!RCReceiverResources_Create(&out,0,H(VkDevice,1),0,10,10,(VkSampleCountFlagBits)3,0,0,0,32,0,0,0,0,true,0,pre,forward,&c,&c));
	RCReceiverResources_Destroy(&out); assert(!out.device); puts("PASS: receiver resources preserve old frame state on every creation failure"); return 0;
}
