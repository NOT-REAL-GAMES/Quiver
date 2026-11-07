#include "radiance_receiver_slot.h"
#include "radiance_receiver_frame.h"
#include <stddef.h>
#include <string.h>

typedef struct { VkBool32 fractional; uint32_t group; } rc_filter_constants_t;
static bool idle(const rc_receiver_slot_t *slot)
{
	return !slot->last_fence || (slot->device && vkGetFenceStatus(slot->device,slot->last_fence)==VK_SUCCESS);
}
static void destroy(rc_receiver_slot_t *slot)
{
	RCReceiverAlternates_Destroy(&slot->alternates);
	RCReceiverModelAlternates_Destroy(&slot->model_alternates);
	RCReceiverResources_Destroy(&slot->resources);
	RCReceiverShaders_Destroy(&slot->shaders);
	memset(slot,0,sizeof(*slot));
}
bool RCReceiverSlot_Release(rc_receiver_slot_t *slot)
{
	if(!slot || !idle(slot))return false;
	destroy(slot); return true;
}
bool RCReceiverSlot_MarkSubmitted(rc_receiver_slot_t *slot,VkFence fence)
{
	if(!slot || !slot->ready || !fence || !idle(slot))return false;
	slot->last_fence=fence; return true;
}
static bool patch_graphics(VkGraphicsPipelineCreateInfo *dst,VkPipelineShaderStageCreateInfo stages[2],
	const VkGraphicsPipelineCreateInfo *src,VkShaderModule vertex,VkShaderModule fragment)
{
	if(!src->pStages || src->stageCount!=2)return false;
	bool have_vertex=false,have_fragment=false;
	for(unsigned i=0;i<2;++i) {
		stages[i]=src->pStages[i];
		if(stages[i].stage==VK_SHADER_STAGE_VERTEX_BIT && !have_vertex) { stages[i].module=vertex; have_vertex=true; }
		else if(stages[i].stage==VK_SHADER_STAGE_FRAGMENT_BIT && !have_fragment) { stages[i].module=fragment; have_fragment=true; }
		else return false;
	}
	*dst=*src; dst->pStages=stages; return have_vertex && have_fragment;
}
static void optional_soft_shadows(rc_receiver_slot_t *slot,const rc_receiver_slot_config_t *c)
{
	const bool requested=c->soft_shadows && c->texture_filter;
	if(!requested) {
		slot->soft_shadow_requested=false;
		if(!slot->resources.soft_shadows)slot->soft_shadow_attempted=false;
		return;
	}
	/* Failed optional compilation is retried only after explicit off/on or
 	 * replacement of the mandatory resource bundle, never every frame. */
	if(!slot->soft_shadow_requested)slot->soft_shadow_attempted=false;
	slot->soft_shadow_requested=true;
	if(slot->resources.soft_shadows || slot->soft_shadow_attempted)return;
	slot->soft_shadow_attempted=true;
	(void)RCReceiverShaders_CreateSoftPipeline(&slot->resources.soft_shadows,c->device,c->cache,slot->resources.layouts.direct,c->hardware);
}
bool RCReceiverSlot_Prepare(rc_receiver_slot_t *slot,const rc_receiver_slot_config_t *c)
{
	if(!slot || !c || !c->device || !c->physical || !c->revision || !c->geometry_enabled || (c->full_light && !c->texture_filter) ||
		!RCReceiverTargets_ValidSamples(c->samples) || (c->hardware && !c->ray_query_enabled) || (slot->device && slot->device!=c->device) || !idle(slot)) return false;
	if(slot->ready && slot->revision==c->revision && slot->width==c->width && slot->height==c->height && slot->samples==c->samples && slot->hardware==c->hardware && slot->texture_filter==c->texture_filter && slot->full_light==c->full_light) {
		RCReceiverAlternates_Destroy(&slot->alternates);
		RCReceiverModelAlternates_Destroy(&slot->model_alternates);
		slot->last_fence=VK_NULL_HANDLE; optional_soft_shadows(slot,c); return true;
	}
	rc_receiver_slot_t pending={.device=c->device,.revision=c->revision,.width=c->width,.height=c->height,.samples=c->samples,.hardware=c->hardware,.texture_filter=c->texture_filter,.full_light=c->full_light};
	if(!RCReceiverShaders_Create(&pending.shaders,c->device,c->hardware,c->texture_filter,c->samples!=VK_SAMPLE_COUNT_1_BIT,c->full_light))goto fail;
	VkGraphicsPipelineCreateInfo prepass[2], forward[4]; VkPipelineShaderStageCreateInfo ps[2][2],fs[4][2];
	for(unsigned i=0;i<2;++i)if(!patch_graphics(&prepass[i],ps[i],&c->prepass[i],pending.shaders.vertex,pending.shaders.prepass))goto fail;
	for(unsigned i=0;i<4;++i)if(!patch_graphics(&forward[i],fs[i],&c->forward[i],pending.shaders.vertex,pending.shaders.forward))goto fail;
	VkComputePipelineCreateInfo classify=c->classify,direct=c->direct;
	if(classify.stage.stage!=VK_SHADER_STAGE_COMPUTE_BIT || direct.stage.stage!=VK_SHADER_STAGE_COMPUTE_BIT)goto fail;
	classify.stage.module=pending.shaders.classify; direct.stage.module=pending.shaders.direct;
	if(!RCReceiverResources_Create(&pending.resources,c->physical,c->device,c->memory,c->width,c->height,c->samples,
		c->nearest,c->viewport_buffer,c->viewport_offset,c->viewport_range,c->world,c->radiance,c->cascade,c->light,
		c->geometry_enabled,c->cache,prepass,forward,&classify,&direct))goto fail;
	VkComputePipelineCreateInfo reconstruct=direct;
	reconstruct.layout=pending.resources.layouts.reconstruct;
	reconstruct.stage.module=pending.shaders.reconstruct;
	rc_filter_constants_t filter={VK_TRUE,RC_RECEIVER_FILTER_GROUP};
	const VkSpecializationMapEntry entries[2]={{0,offsetof(rc_filter_constants_t,fractional),sizeof(filter.fractional)},
		{1,offsetof(rc_filter_constants_t,group),sizeof(filter.group)}};
	const VkSpecializationInfo specialization={2,entries,sizeof(filter),&filter};
	if(c->texture_filter) {
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(c->physical,&properties);
		const VkPhysicalDeviceLimits *limits=&properties.limits;
		if(limits->maxComputeSharedMemorySize>=RC_RECEIVER_FILTER_WIDE_SHARED &&
			limits->maxComputeWorkGroupInvocations>=RC_RECEIVER_FILTER_GROUP_WIDE &&
			limits->maxComputeWorkGroupSize[0]>=RC_RECEIVER_FILTER_GROUP_WIDE) filter.group=RC_RECEIVER_FILTER_GROUP_WIDE;
		reconstruct.stage.pSpecializationInfo=&specialization;
		pending.resources.filter_group=filter.group;
	}
	if(vkCreateComputePipelines(c->device,c->cache,1,&reconstruct,NULL,&pending.resources.reconstruct)!=VK_SUCCESS ||
		!pending.resources.reconstruct)goto fail;
	if(c->texture_filter) {
		/* Whole strengths skip the fractional baseline; see the filter shader. */
		filter.fractional=VK_FALSE;
		if(vkCreateComputePipelines(c->device,c->cache,1,&reconstruct,NULL,&pending.resources.reconstruct_whole)!=VK_SUCCESS ||
			!pending.resources.reconstruct_whole)goto fail;
	}
	if(c->samples!=VK_SAMPLE_COUNT_1_BIT && !RCReceiverShaders_CreateResolvePipeline(&pending.resources.pipelines.resolve,c->device,c->cache,
		pending.resources.layouts.classify,pending.resources.targets.resolve_pass))goto fail;
	if(c->full_light && !RCReceiverLight_Create(&pending.resources.light,c->physical,c->device,c->memory,&pending.resources.targets,
		pending.resources.descriptors.layouts[2],c->nearest,c->viewport_buffer,c->viewport_offset,c->viewport_range,c->radiance,c->cache,c->hardware))goto fail;
	pending.ready=true; destroy(slot); *slot=pending; optional_soft_shadows(slot,c); return true;
fail:
	destroy(&pending); return false;
}
