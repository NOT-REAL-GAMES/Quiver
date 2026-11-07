#include "radiance_receiver_descriptors.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned call, fail, destroyed_layout, destroyed_pool, updates;
static bool omit_reverse;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorSetLayout *out)
{ (void)d;(void)a; ++call; if(call==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; static const uint32_t counts[4]={6,6,9,10}; assert(call<=4 && i->bindingCount==counts[call-1]);
	for(unsigned b=0;b<i->bindingCount;++b) {
		const VkDescriptorSetLayoutBinding *binding=&i->pBindings[b];
		assert(binding->binding==b && binding->descriptorCount==1);
		VkDescriptorType type=call==3?(b==7?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER):
			b<(call==1?3:call==2?2:7)?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		assert(binding->descriptorType==type && binding->stageFlags==(call==3?VK_SHADER_STAGE_FRAGMENT_BIT:call==1?VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_COMPUTE_BIT));
	}
	*out=H(VkDescriptorSetLayout,call); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout l,const VkAllocationCallbacks *a)
{ (void)d;(void)l;(void)a; ++destroyed_layout; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d,const VkDescriptorPoolCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorPool *out)
{ (void)d;(void)a; ++call; if(call==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; assert(i->maxSets==6 && i->poolSizeCount==3);
	assert(i->pPoolSizes[0].type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && i->pPoolSizes[0].descriptorCount==35);
	assert(i->pPoolSizes[1].type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && i->pPoolSizes[1].descriptorCount==13);
	assert(i->pPoolSizes[2].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && i->pPoolSizes[2].descriptorCount==2);
	*out=H(VkDescriptorPool,call); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice d,VkDescriptorPool p,const VkAllocationCallbacks *a)
{ (void)d;(void)p;(void)a; ++destroyed_pool; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *out)
{ (void)d; ++call; if(call==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; assert(i->descriptorSetCount==6);
	assert(i->pSetLayouts[4]==H(VkDescriptorSetLayout,4) && i->pSetLayouts[5]==i->pSetLayouts[4]);
	for(unsigned n=0;n<6;++n)out[n]=H(VkDescriptorSet,n+1);
	if(omit_reverse)out[5]=VK_NULL_HANDLE;
	return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t copies,const VkCopyDescriptorSet *c)
{
	(void)d;(void)c; assert(n==50 && copies==0);
	const unsigned targets[46]={RC_RECEIVER_DEPTH,RC_RECEIVER_NORMAL,RC_RECEIVER_ID,RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,
		RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_MINIMUM,
		RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_MINIMUM,
		RC_RECEIVER_RESOLVED_INDIRECT,RC_RECEIVER_RESOLVED_DIRECT,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_POSITION,RC_RECEIVER_RESOLVED_MINIMUM,
		RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_MINIMUM,
		RC_RECEIVER_RESOLVED_INDIRECT,RC_RECEIVER_RESOLVED_DIRECT,RC_RECEIVER_RESOLVED_MINIMUM,
		RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_RESOLVED_INDIRECT,RC_RECEIVER_RESOLVED_DIRECT,RC_RECEIVER_RESOLVED_MINIMUM,
		RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_MINIMUM};
	for(unsigned k=0;k<46;++k) {
		bool storage=(k>=3 && k<6) || (k>=8 && k<12) || (k>=33 && k<36) || k>=43;
		unsigned set=k<6?0:k<12?1:k<19?2:k<26?3:k<36?4:5;
		unsigned binding=set==0?k:set==1?k-6:set==2?k-12:set==3?k-19:set==4?k-26:k-36;
		assert(w[k].dstSet==H(VkDescriptorSet,set+1) && w[k].dstBinding==binding);
		assert(w[k].pImageInfo->imageView==H(VkImageView,targets[k]+1));
		assert(w[k].descriptorType==(storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		assert(w[k].pImageInfo->imageLayout==(storage?VK_IMAGE_LAYOUT_GENERAL:targets[k]==RC_RECEIVER_DEPTH?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	}
	assert(w[46].dstSet==H(VkDescriptorSet,3) && w[47].dstSet==H(VkDescriptorSet,4));
	assert(w[46].dstBinding==7 && w[47].dstBinding==7 && w[46].pBufferInfo->range==32 && w[47].pBufferInfo->range==32);
	for(unsigned k=48;k<50;++k) {
		assert(w[k].dstSet==H(VkDescriptorSet,k-45) && w[k].dstBinding==8 && w[k].descriptorCount==1);
		assert(w[k].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		assert(w[k].pImageInfo->imageView==H(VkImageView,RC_RECEIVER_DEPTH+1));
		assert(w[k].pImageInfo->sampler==H(VkSampler,1) && w[k].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
	}
	++updates;
}
int main(void)
{
	rc_receiver_targets_t t={.device=H(VkDevice,1)}; for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i)t.images[i].view=H(VkImageView,i+1);
	rc_receiver_descriptors_t d={0};
	VkImageView depth=t.images[RC_RECEIVER_DEPTH].view;t.images[RC_RECEIVER_DEPTH].view=VK_NULL_HANDLE;
	assert(!RCReceiverDescriptors_Create(&d,&t,H(VkSampler,1),H(VkBuffer,1),0,32) && !call);
	t.images[RC_RECEIVER_DEPTH].view=depth;
	for(unsigned f=1;f<=6;++f) { call=0;fail=f; destroyed_layout=destroyed_pool=0; assert(!RCReceiverDescriptors_Create(&d,&t,H(VkSampler,1),H(VkBuffer,1),0,32)); assert(!d.device && destroyed_layout==(f<=5?f-1:4) && destroyed_pool==(f==6)); }
	call=fail=0; assert(RCReceiverDescriptors_Create(&d,&t,H(VkSampler,1),H(VkBuffer,1),0,32));
	assert(updates==1 && d.classify && d.direct && d.forward && d.forward_resolved && d.reconstruct && d.reconstruct_reverse);
	call=0; fail=3; assert(!RCReceiverDescriptors_Create(&d,&t,H(VkSampler,1),H(VkBuffer,1),0,32)); assert(d.classify && updates==1);
	call=fail=0;omit_reverse=true;VkDescriptorSet previous=d.reconstruct_reverse;
	assert(!RCReceiverDescriptors_Create(&d,&t,H(VkSampler,1),H(VkBuffer,1),0,32) && d.reconstruct_reverse==previous && updates==1);
	RCReceiverDescriptors_Destroy(&d); assert(!d.device);
	puts("PASS: receiver descriptor layouts, image transitions and transactional failures"); return 0;
}
