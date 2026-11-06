#include "radiance_receiver_descriptors.h"
#include <string.h>
void RCReceiverDescriptors_Destroy(rc_receiver_descriptors_t *d)
{
	if (!d || !d->device) return;
	if (d->pool) vkDestroyDescriptorPool(d->device,d->pool,NULL);
	for (unsigned i=0;i<4;++i) if (d->layouts[i]) vkDestroyDescriptorSetLayout(d->device,d->layouts[i],NULL);
	memset(d,0,sizeof(*d));
}
static bool create_layout(VkDevice device,VkDescriptorSetLayout *out,unsigned count,unsigned sampled,unsigned storage,int uniform_binding,VkShaderStageFlags stage)
{
	VkDescriptorSetLayoutBinding b[10]={0};
	if (count>10 || count!=sampled+storage+(uniform_binding>=0?1u:0u) || uniform_binding>=(int)count) return false;
	for (unsigned i=0;i<count;++i) {
		b[i].binding=i; b[i].descriptorCount=1; b[i].stageFlags=stage;
		unsigned image_binding=i-(uniform_binding>=0 && i>(unsigned)uniform_binding?1u:0u);
		b[i].descriptorType=(int)i==uniform_binding ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
			image_binding<sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	}
	VkDescriptorSetLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=count,.pBindings=b};
	return vkCreateDescriptorSetLayout(device,&info,NULL,out)==VK_SUCCESS && *out;
}
bool RCReceiverDescriptors_Create(rc_receiver_descriptors_t *out,const rc_receiver_targets_t *t,VkSampler nearest,
	VkBuffer viewport_buffer,VkDeviceSize viewport_offset,VkDeviceSize viewport_range)
{
	if (!out || !t || !t->device || !nearest || !viewport_buffer || viewport_range<32 || viewport_offset>UINT64_MAX-viewport_range) return false;
	for (unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i) if (!t->images[i].view) return false;
	rc_receiver_descriptors_t pending={.device=t->device};
	if (!create_layout(t->device,&pending.layouts[0],6,3,3,-1,VK_SHADER_STAGE_COMPUTE_BIT) ||
		!create_layout(t->device,&pending.layouts[1],6,2,4,-1,VK_SHADER_STAGE_COMPUTE_BIT) ||
		!create_layout(t->device,&pending.layouts[2],9,8,0,7,VK_SHADER_STAGE_FRAGMENT_BIT) ||
		!create_layout(t->device,&pending.layouts[3],10,7,3,-1,VK_SHADER_STAGE_COMPUTE_BIT)) goto fail;
	VkDescriptorPoolSize sizes[3]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,35},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,13},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2}};
	VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=6,.poolSizeCount=3,.pPoolSizes=sizes};
	if (vkCreateDescriptorPool(t->device,&pool,NULL,&pending.pool)!=VK_SUCCESS || !pending.pool) goto fail;
	VkDescriptorSetLayout set_layouts[6]={pending.layouts[0],pending.layouts[1],pending.layouts[2],pending.layouts[2],pending.layouts[3],pending.layouts[3]};
	VkDescriptorSet sets[6]={0}; VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool=pending.pool,.descriptorSetCount=6,.pSetLayouts=set_layouts};
	if (vkAllocateDescriptorSets(t->device,&alloc,sets)!=VK_SUCCESS) goto fail;
	for(unsigned i=0;i<6;++i) if(!sets[i]) goto fail;
	pending.classify=sets[0]; pending.direct=sets[1]; pending.forward=sets[2]; pending.forward_resolved=sets[3]; pending.reconstruct=sets[4]; pending.reconstruct_reverse=sets[5];
	const unsigned indices[46]={RC_RECEIVER_DEPTH,RC_RECEIVER_NORMAL,RC_RECEIVER_ID,RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,
		RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_MINIMUM,
		RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_MINIMUM,
		RC_RECEIVER_RESOLVED_INDIRECT,RC_RECEIVER_RESOLVED_DIRECT,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_POSITION,RC_RECEIVER_RESOLVED_MINIMUM,
		RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_MINIMUM,
		RC_RECEIVER_RESOLVED_INDIRECT,RC_RECEIVER_RESOLVED_DIRECT,RC_RECEIVER_RESOLVED_MINIMUM,
		RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,RC_RECEIVER_RESOLVED_INDIRECT,RC_RECEIVER_HALF_DIRECT,RC_RECEIVER_HALF_MINIMUM,
		RC_RECEIVER_HALF_INDIRECT,RC_RECEIVER_RESOLVED_DIRECT,RC_RECEIVER_RESOLVED_MINIMUM};
	VkDescriptorImageInfo images[48]={0}; VkWriteDescriptorSet writes[50]={0};
	for (unsigned i=0;i<46;++i) {
		unsigned set=i<6?0:i<12?1:i<19?2:i<26?3:i<36?4:5;
		unsigned binding=set==0?i:set==1?i-6:set==2?i-12:set==3?i-19:set==4?i-26:i-36;
		bool storage=(i>=3 && i<6) || (i>=8 && i<12) || (i>=33 && i<36) || i>=43;
		VkImageLayout layout=storage?VK_IMAGE_LAYOUT_GENERAL:indices[i]==RC_RECEIVER_DEPTH?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		images[i]=(VkDescriptorImageInfo){.sampler=storage?VK_NULL_HANDLE:nearest,.imageView=t->images[indices[i]].view,.imageLayout=layout};
		writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[set],.dstBinding=binding,.descriptorCount=1,
			.descriptorType=storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
	}
	VkDescriptorBufferInfo buffer={viewport_buffer,viewport_offset,viewport_range};
	for(unsigned i=0;i<2;++i) writes[46+i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[2+i],.dstBinding=7,
		.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
	for(unsigned i=0;i<2;++i) {
		images[46+i]=(VkDescriptorImageInfo){.sampler=nearest,.imageView=t->images[RC_RECEIVER_DEPTH].view,
			.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
		writes[48+i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[2+i],.dstBinding=8,
			.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[46+i]};
	}
	vkUpdateDescriptorSets(t->device,50,writes,0,NULL);
	RCReceiverDescriptors_Destroy(out); *out=pending; return true;
fail:
	RCReceiverDescriptors_Destroy(&pending); return false;
}
