#include "radiance_receiver_shaders.h"
#include "../Shaders/shaders.h"
#include <stdlib.h>
#include <string.h>
void RCReceiverShaders_Destroy(rc_receiver_shaders_t *s)
{
	if(!s || !s->device)return;
	if(s->vertex)vkDestroyShaderModule(s->device,s->vertex,NULL);
	if(s->prepass)vkDestroyShaderModule(s->device,s->prepass,NULL);
	if(s->classify)vkDestroyShaderModule(s->device,s->classify,NULL);
	if(s->direct)vkDestroyShaderModule(s->device,s->direct,NULL);
	if(s->forward)vkDestroyShaderModule(s->device,s->forward,NULL);
	if(s->reconstruct)vkDestroyShaderModule(s->device,s->reconstruct,NULL);
	memset(s,0,sizeof(*s));
}
static bool create(VkDevice device,VkShaderModule *out,const unsigned char *bytes,int size)
{
	if(!bytes || size<=0 || size%4)return false;
	uint32_t *aligned=malloc((size_t)size); if(!aligned)return false;
	memcpy(aligned,bytes,(size_t)size);
	VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)size,.pCode=aligned};
	VkResult result=vkCreateShaderModule(device,&info,NULL,out); free(aligned);
	return result==VK_SUCCESS && *out;
}
bool RCReceiverShaders_CreateSoftPipeline(VkPipeline *out,VkDevice device,VkPipelineCache cache,VkPipelineLayout layout,bool hardware)
{
	if(!out || *out || !device || !layout)return false;
	VkShaderModule module=VK_NULL_HANDLE;
	VkPipeline pipeline=VK_NULL_HANDLE;
	VkResult result=VK_ERROR_INITIALIZATION_FAILED;
	if(create(device,&module,hardware?radiance_receiver_soft_rt_comp_spv:radiance_receiver_soft_sw_comp_spv,
		hardware?radiance_receiver_soft_rt_comp_spv_size:radiance_receiver_soft_sw_comp_spv_size)) {
		VkComputePipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,
			.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
				.module=module,.pName="main"}};
		result=vkCreateComputePipelines(device,cache,1,&info,NULL,&pipeline);
	}
	if(module)vkDestroyShaderModule(device,module,NULL);
	if(result!=VK_SUCCESS || !pipeline) {
		if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
		return false;
	}
	*out=pipeline;return true;
}
bool RCReceiverShaders_CreateResolvePipeline(VkPipeline *out,VkDevice device,VkPipelineCache cache,VkPipelineLayout layout,VkRenderPass pass)
{
	if(!out || *out || !device || !layout || !pass)return false;
	VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
	VkPipeline pipeline=VK_NULL_HANDLE;
	VkResult result=VK_ERROR_INITIALIZATION_FAILED;
	if(create(device,&vertex,postprocess_vert_spv,postprocess_vert_spv_size) &&
		create(device,&fragment,radiance_receiver_depth_resolve_frag_spv,radiance_receiver_depth_resolve_frag_spv_size)) {
		VkPipelineShaderStageCreateInfo stages[2]={
			{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
			{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
		VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
		VkPipelineViewportStateCreateInfo viewport={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
		VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1.0f};
		VkPipelineMultisampleStateCreateInfo multisample={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
		VkPipelineDepthStencilStateCreateInfo depth={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_ALWAYS};
		VkPipelineColorBlendAttachmentState attachments[2]={{.colorWriteMask=0xf},{.colorWriteMask=0xf}};
		VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=2,.pAttachments=attachments};
		VkDynamicState states[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
		VkGraphicsPipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
			.pVertexInputState=&input,.pInputAssemblyState=&assembly,.pViewportState=&viewport,.pRasterizationState=&raster,
			.pMultisampleState=&multisample,.pDepthStencilState=&depth,.pColorBlendState=&blend,.pDynamicState=&dynamic,
			.layout=layout,.renderPass=pass};
		result=vkCreateGraphicsPipelines(device,cache,1,&info,NULL,&pipeline);
	}
	if(vertex)vkDestroyShaderModule(device,vertex,NULL);
	if(fragment)vkDestroyShaderModule(device,fragment,NULL);
	if(result!=VK_SUCCESS || !pipeline) {
		if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
		return false;
	}
	*out=pipeline;return true;
}
bool RCReceiverShaders_Create(rc_receiver_shaders_t *out,VkDevice device,bool hardware,bool texture_filter,bool multisampled,bool full_light)
{
	if(!out || !device)return false;
	rc_receiver_shaders_t s={.device=device};
	const unsigned char *direct=texture_filter ? (hardware ? radiance_receiver_direct_fast_rt_comp_spv : radiance_receiver_direct_fast_sw_comp_spv) :
		(hardware ? radiance_receiver_direct_rt_comp_spv : radiance_receiver_direct_sw_comp_spv);
	int direct_size=texture_filter ? (hardware ? radiance_receiver_direct_fast_rt_comp_spv_size : radiance_receiver_direct_fast_sw_comp_spv_size) :
		(hardware ? radiance_receiver_direct_rt_comp_spv_size : radiance_receiver_direct_sw_comp_spv_size);
	const unsigned char *forward=texture_filter ? (hardware ? world_receiver_forward_fast_rt_frag_spv : world_receiver_forward_fast_sw_frag_spv) :
		(hardware ? world_receiver_forward_rt_frag_spv : world_receiver_forward_sw_frag_spv);
	int forward_size=texture_filter ? (hardware ? world_receiver_forward_fast_rt_frag_spv_size : world_receiver_forward_fast_sw_frag_spv_size) :
		(hardware ? world_receiver_forward_rt_frag_spv_size : world_receiver_forward_sw_frag_spv_size);
	if(full_light) { forward=world_receiver_forward_lit_frag_spv; forward_size=world_receiver_forward_lit_frag_spv_size; }
	const unsigned char *classify=multisampled ? (texture_filter ? radiance_receiver_half_fast_msaa_comp_spv : radiance_receiver_half_msaa_comp_spv) :
		(texture_filter ? radiance_receiver_half_fast_comp_spv : radiance_receiver_half_comp_spv);
	int classify_size=multisampled ? (texture_filter ? radiance_receiver_half_fast_msaa_comp_spv_size : radiance_receiver_half_msaa_comp_spv_size) :
		(texture_filter ? radiance_receiver_half_fast_comp_spv_size : radiance_receiver_half_comp_spv_size);
	if(!create(device,&s.vertex,world_radiance_vert_spv,world_radiance_vert_spv_size) ||
		!create(device,&s.prepass,radiance_receiver_world_frag_spv,radiance_receiver_world_frag_spv_size) ||
		!create(device,&s.classify,classify,classify_size) ||
		!create(device,&s.direct,direct,direct_size) ||
		!create(device,&s.reconstruct,texture_filter ? radiance_receiver_filter_comp_spv : radiance_receiver_reconstruct_comp_spv,
			texture_filter ? radiance_receiver_filter_comp_spv_size : radiance_receiver_reconstruct_comp_spv_size) ||
		!create(device,&s.forward,forward,forward_size))goto fail;
	RCReceiverShaders_Destroy(out); *out=s; return true;
fail:
	RCReceiverShaders_Destroy(&s); return false;
}
