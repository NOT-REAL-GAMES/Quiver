#include "radiance_receiver_layouts.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned call,fail,destroys,limited; static bool supported=true;
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p,VkPhysicalDeviceProperties *o)
{ (void)p; memset(o,0,sizeof(*o)); o->limits.maxBoundDescriptorSets=supported?7:6; o->limits.maxPushConstantsSize=128;
	o->limits.maxPerStageDescriptorSampledImages=limited==1?7:8;o->limits.maxDescriptorSetSampledImages=limited==2?7:8;
	o->limits.maxPerStageDescriptorSamplers=limited==3?7:8;o->limits.maxDescriptorSetSamplers=limited==4?7:8;
	o->limits.maxPerStageDescriptorUniformBuffers=limited==5?0:1;o->limits.maxDescriptorSetUniformBuffers=limited==6?0:1; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice p,VkPhysicalDeviceFeatures *o)
{ (void)p; memset(o,0,sizeof(*o)); o->geometryShader=VK_TRUE; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkPipelineLayout *out)
{ (void)d;(void)a; static const uint32_t counts[]={1,2,2,6,7}, sizes[]={112,8,32,88,92}; ++call; if(call==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; assert(i->setLayoutCount==counts[(call-1)%5] && i->pPushConstantRanges->size==sizes[(call-1)%5]); if ((call-1)%5==1 || (call-1)%5==2) assert(i->pSetLayouts[1]==H(VkDescriptorSetLayout,2)); if ((call-1)%5>=3) assert(i->pSetLayouts[5]==H(VkDescriptorSetLayout,2)); *out=H(VkPipelineLayout,call); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d,VkPipelineLayout l,const VkAllocationCallbacks *a)
{ (void)d;(void)l;(void)a; ++destroys; }
int main(void)
{
	VkDevice dev=H(VkDevice,1); VkPhysicalDevice phys=H(VkPhysicalDevice,1); VkDescriptorSetLayout set=H(VkDescriptorSetLayout,1), world[5]={set,set,set,set,set}; VkDescriptorSetLayout cascade=H(VkDescriptorSetLayout,2);
	rc_receiver_descriptors_t desc={.device=dev,.layouts={set,set,set,set}}; rc_receiver_layouts_t out={0};
	assert(!RCReceiverLayouts_Create(&out,phys,dev,&desc,world,cascade,set,set,false)); assert(!call);
	supported=false; assert(!RCReceiverLayouts_Create(&out,phys,dev,&desc,world,cascade,set,set,true)); assert(!call); supported=true;
	for(limited=1;limited<=6;++limited) { assert(!RCReceiverLayouts_Create(&out,phys,dev,&desc,world,cascade,set,set,true)); assert(!call); } limited=0;
	for(unsigned f=1;f<=5;++f) { call=destroys=0;fail=f; assert(!RCReceiverLayouts_Create(&out,phys,dev,&desc,world,cascade,set,set,true)); assert(!out.device && destroys==f-1); }
	call=fail=0; assert(RCReceiverLayouts_Create(&out,phys,dev,&desc,world,cascade,set,set,true)); assert(out.forward && out.reconstruct);
	call=0;fail=3; assert(!RCReceiverLayouts_Create(&out,phys,dev,&desc,world,cascade,set,set,true)); assert(out.forward && out.reconstruct);
	RCReceiverLayouts_Destroy(&out); assert(!out.device);
	puts("PASS: receiver pipeline layouts reject unsupported devices and roll back failures"); return 0;
}
