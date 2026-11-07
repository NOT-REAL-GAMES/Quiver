#include "radiance_receiver_light.h"
#include "../Shaders/shaders.h"
#include <stdlib.h>
#include <string.h>
#define RC_LIGHT_BINDINGS 13
#define RC_LIGHT_SAMPLED 9
#define RC_LIGHT_LIST_BINDING 12
/* Dispatch arguments and the miss count, then one packed pixel per miss. */
#define RC_LIGHT_LIST_HEADER 16u
typedef struct { float inverse_view_projection[16]; int32_t viewport[4]; float ndc_transform[4]; float threshold[4]; } rc_light_push_t;
_Static_assert(sizeof(rc_light_push_t)==112,"light push layout mismatch");

void RCReceiverLight_Destroy(rc_receiver_light_t *l)
{
	if(!l || !l->device)return;
	if(l->gather_pipeline)vkDestroyPipeline(l->device,l->gather_pipeline,NULL);
	if(l->miss_pipeline)vkDestroyPipeline(l->device,l->miss_pipeline,NULL);
	if(l->pipeline_layout)vkDestroyPipelineLayout(l->device,l->pipeline_layout,NULL);
	if(l->pool)vkDestroyDescriptorPool(l->device,l->pool,NULL);
	if(l->layout)vkDestroyDescriptorSetLayout(l->device,l->layout,NULL);
	for(unsigned i=0;i<RC_RECEIVER_LIGHT_COUNT;++i) {
		if(l->images[i].view)vkDestroyImageView(l->device,l->images[i].view,NULL);
		if(l->images[i].image)vkDestroyImage(l->device,l->images[i].image,NULL);
		if(l->images[i].memory)vkFreeMemory(l->device,l->images[i].memory,NULL);
	}
	if(l->misses)vkDestroyBuffer(l->device,l->misses,NULL);
	if(l->misses_memory)vkFreeMemory(l->device,l->misses_memory,NULL);
	memset(l,0,sizeof(*l));
}
static bool create_list(rc_receiver_light_t *l,const VkPhysicalDeviceMemoryProperties *memory,VkDeviceSize size)
{
	VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
		.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
	if(vkCreateBuffer(l->device,&info,NULL,&l->misses)!=VK_SUCCESS)return false;
	VkMemoryRequirements req; vkGetBufferMemoryRequirements(l->device,l->misses,&req);
	uint32_t type=0;
	while(type<memory->memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) ||
		!(memory->memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
	if(type==memory->memoryTypeCount)return false;
	VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
	return vkAllocateMemory(l->device,&allocation,NULL,&l->misses_memory)==VK_SUCCESS &&
		vkBindBufferMemory(l->device,l->misses,l->misses_memory,0)==VK_SUCCESS;
}
static bool create_module(VkDevice device,VkShaderModule *out,const unsigned char *bytes,int size)
{
	if(!bytes || size<=0 || size%4)return false;
	uint32_t *aligned=malloc((size_t)size); if(!aligned)return false;
	memcpy(aligned,bytes,(size_t)size);
	VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)size,.pCode=aligned};
	VkResult result=vkCreateShaderModule(device,&info,NULL,out); free(aligned);
	return result==VK_SUCCESS && *out;
}
static bool create_pipeline(const rc_receiver_light_t *l,VkPipelineCache cache,const unsigned char *bytes,int size,VkPipeline *out)
{
	VkShaderModule module=VK_NULL_HANDLE;
	if(!create_module(l->device,&module,bytes,size))return false;
	VkComputePipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=l->pipeline_layout,
		.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
	VkResult result=vkCreateComputePipelines(l->device,cache,1,&info,NULL,out);
	vkDestroyShaderModule(l->device,module,NULL);
	return result==VK_SUCCESS && *out;
}
static bool write_sets(rc_receiver_light_t *l,const rc_receiver_targets_t *t,VkSampler nearest,
	VkBuffer viewport_buffer,VkDeviceSize viewport_offset,VkDeviceSize viewport_range)
{
	VkDescriptorImageInfo images[2*(RC_LIGHT_BINDINGS-1)+9]={0};
	VkWriteDescriptorSet writes[2*RC_LIGHT_BINDINGS+9]={0};
	unsigned image=0,write=0;
	VkDescriptorBufferInfo list={l->misses,0,VK_WHOLE_SIZE};
	for(unsigned set=0;set<2;++set) {
		const unsigned first=set?RC_RECEIVER_RESOLVED_INDIRECT:RC_RECEIVER_HALF_INDIRECT;
		const unsigned sampled[RC_LIGHT_SAMPLED]={RC_RECEIVER_HALF_POSITION,RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_ID,
			first,first+1,first+2,RC_RECEIVER_DEPTH,RC_RECEIVER_NORMAL};
		for(unsigned b=0;b<RC_LIGHT_SAMPLED;++b) {
			images[image]=(VkDescriptorImageInfo){nearest,t->images[sampled[b]].view,
				sampled[b]==RC_RECEIVER_DEPTH?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			writes[write++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=l->gather[set],.dstBinding=b,
				.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[image++]};
		}
		for(unsigned b=0;b<RC_RECEIVER_LIGHT_COUNT;++b) {
			images[image]=(VkDescriptorImageInfo){VK_NULL_HANDLE,l->images[b].view,VK_IMAGE_LAYOUT_GENERAL};
			writes[write++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=l->gather[set],.dstBinding=RC_LIGHT_SAMPLED+b,
				.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&images[image++]};
		}
		writes[write++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=l->gather[set],.dstBinding=RC_LIGHT_LIST_BINDING,
			.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&list};
	}
	/* Same ABI as the ordinary forward sets; only the field bindings differ. */
	const VkImageView forward[8]={l->images[RC_RECEIVER_LIGHT_INDIRECT].view,l->images[RC_RECEIVER_LIGHT_DIRECT].view,
		t->images[RC_RECEIVER_HALF_STATUS].view,t->images[RC_RECEIVER_ID].view,t->images[RC_RECEIVER_HALF_NORMAL].view,
		t->images[RC_RECEIVER_HALF_POSITION].view,l->images[RC_RECEIVER_LIGHT_MINIMUM].view,t->images[RC_RECEIVER_DEPTH].view};
	for(unsigned i=0;i<8;++i) {
		images[image]=(VkDescriptorImageInfo){nearest,forward[i],i==7?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		writes[write++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=l->forward,.dstBinding=i==7?8:i,
			.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[image++]};
	}
	VkDescriptorBufferInfo viewport={viewport_buffer,viewport_offset,viewport_range};
	writes[write++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=l->forward,.dstBinding=7,
		.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&viewport};
	vkUpdateDescriptorSets(l->device,write,writes,0,NULL);
	return true;
}
bool RCReceiverLight_Create(rc_receiver_light_t *out,VkPhysicalDevice physical,VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory,const rc_receiver_targets_t *t,VkDescriptorSetLayout forward_layout,
	VkSampler nearest,VkBuffer viewport_buffer,VkDeviceSize viewport_offset,VkDeviceSize viewport_range,
	VkDescriptorSetLayout radiance,VkPipelineCache cache,bool hardware)
{
	if(!out || !physical || !device || !memory || !t || !forward_layout || !nearest || !viewport_buffer || !radiance)return false;
	rc_receiver_light_t l={.device=device};
	rc_receiver_targets_t scratch={.device=device};
	for(unsigned i=0;i<RC_RECEIVER_LIGHT_COUNT;++i)
		if(!RCReceiverTargets_Image(&scratch,physical,memory,&l.images[i],VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT,VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT,t->width,t->height,VK_SAMPLE_COUNT_1_BIT))goto fail;
	if(!create_list(&l,memory,RC_LIGHT_LIST_HEADER+(VkDeviceSize)t->width*t->height*sizeof(uint32_t)))goto fail;
	VkDescriptorSetLayoutBinding bindings[RC_LIGHT_BINDINGS]={0};
	for(unsigned i=0;i<RC_LIGHT_BINDINGS;++i)
		bindings[i]=(VkDescriptorSetLayoutBinding){i,i<RC_LIGHT_SAMPLED?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			i==RC_LIGHT_LIST_BINDING?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
	VkDescriptorSetLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=RC_LIGHT_BINDINGS,.pBindings=bindings};
	if(vkCreateDescriptorSetLayout(device,&layout,NULL,&l.layout)!=VK_SUCCESS)goto fail;
	VkDescriptorPoolSize sizes[4]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*RC_LIGHT_SAMPLED+8},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*RC_RECEIVER_LIGHT_COUNT},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};
	VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=3,.poolSizeCount=4,.pPoolSizes=sizes};
	if(vkCreateDescriptorPool(device,&pool,NULL,&l.pool)!=VK_SUCCESS)goto fail;
	VkDescriptorSetLayout set_layouts[3]={l.layout,l.layout,forward_layout};
	VkDescriptorSet sets[3]={0};
	VkDescriptorSetAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=l.pool,.descriptorSetCount=3,.pSetLayouts=set_layouts};
	if(vkAllocateDescriptorSets(device,&allocate,sets)!=VK_SUCCESS)goto fail;
	l.gather[0]=sets[0]; l.gather[1]=sets[1]; l.forward=sets[2];
	if(!write_sets(&l,t,nearest,viewport_buffer,viewport_offset,viewport_range))goto fail;
	VkDescriptorSetLayout pipeline_sets[2]={l.layout,radiance};
	VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(rc_light_push_t)};
	VkPipelineLayoutCreateInfo pipeline_layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=2,.pSetLayouts=pipeline_sets,
		.pushConstantRangeCount=1,.pPushConstantRanges=&range};
	if(vkCreatePipelineLayout(device,&pipeline_layout,NULL,&l.pipeline_layout)!=VK_SUCCESS)goto fail;
	if(!create_pipeline(&l,cache,radiance_receiver_light_comp_spv,radiance_receiver_light_comp_spv_size,&l.gather_pipeline) ||
		!create_pipeline(&l,cache,hardware?radiance_receiver_light_miss_rt_comp_spv:radiance_receiver_light_miss_sw_comp_spv,
			hardware?radiance_receiver_light_miss_rt_comp_spv_size:radiance_receiver_light_miss_sw_comp_spv_size,&l.miss_pipeline))goto fail;
	RCReceiverLight_Destroy(out); *out=l; return true;
fail:
	RCReceiverLight_Destroy(&l); return false;
}
static void image_barriers(VkCommandBuffer command,const rc_receiver_light_t *l,VkImageLayout from,VkImageLayout to,
	VkAccessFlags source,VkAccessFlags destination,VkPipelineStageFlags source_stage,VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier images[RC_RECEIVER_LIGHT_COUNT];
	for(unsigned i=0;i<RC_RECEIVER_LIGHT_COUNT;++i)
		images[i]=(VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=source,.dstAccessMask=destination,
			.oldLayout=from,.newLayout=to,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
			.image=l->images[i].image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
	vkCmdPipelineBarrier(command,source_stage,destination_stage,0,0,NULL,0,NULL,RC_RECEIVER_LIGHT_COUNT,images);
}
void RCReceiverLight_Record(VkCommandBuffer command,const rc_receiver_light_t *l,const rc_receiver_params_t *p,bool postprocessed,VkDescriptorSet radiance_set)
{
	static const uint32_t header[4]={0,1,1,0};
	vkCmdUpdateBuffer(command,l->misses,0,sizeof(header),header);
	/* Fields arrive from compute, in a layout whose transition targeted fragment reads. */
	VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);
	image_barriers(command,l,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	rc_light_push_t push;
	memcpy(push.inverse_view_projection,p->inverse_view_projection,sizeof(push.inverse_view_projection));
	memcpy(push.viewport,p->viewport,sizeof(push.viewport));
	memcpy(push.ndc_transform,p->ndc_transform,sizeof(push.ndc_transform));
	push.threshold[0]=p->normal_cosine; push.threshold[1]=p->plane_tolerance; push.threshold[2]=p->divisor; push.threshold[3]=l->upscale_extent;
	VkDescriptorSet sets[2]={l->gather[postprocessed?1:0],radiance_set};
	vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,l->gather_pipeline);
	vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,l->pipeline_layout,0,2,sets,0,NULL);
	vkCmdPushConstants(command,l->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
	vkCmdDispatch(command,((uint32_t)p->viewport[2]+7)/8,((uint32_t)p->viewport[3]+7)/8,1);
	VkMemoryBarrier gathered={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0,1,&gathered,0,NULL,0,NULL);
	vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,l->miss_pipeline);
	vkCmdDispatchIndirect(command,l->misses,0);
	image_barriers(command,l,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
