#include "radiance_receiver_stage.h"
#include "radiance_receiver_select.h"
#include <math.h>
bool RCReceiverResources_StageFrameModels(const rc_receiver_resources_t *r,uint64_t serial,VkCommandBuffer primary,
	VkDescriptorSet cascade_set,VkDescriptorSet lighting_set,const rc_receiver_params_t *params,float depth_clear,
	const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *materials,
	const rc_receiver_alternates_t *alternate,bool complete_world,bool edge_fallback_enabled,bool texture_filter,bool checkerboard,bool smooth_indirect,bool smooth_shadows,bool soft_shadows,VkQueryPool timing_pool,uint32_t timing_first_query,float shadow_filter_strength,
	const rc_receiver_model_snapshot_t *models,const rc_receiver_model_alternates_t *model_alternate)
{
	RCReceiverFrame_Stage(NULL); RCReceiverSelect_Reset();
	if(!isfinite(shadow_filter_strength) || shadow_filter_strength<0 || shadow_filter_strength>2 ||
		(smooth_shadows!=(shadow_filter_strength>0))) return false;
	if(!r || !r->device || !params || !snapshot || (!snapshot->count && !RCReceiverModel_EligibleCount(models)) ||
		(snapshot->count && (!materials || !alternate)) || !complete_world || !edge_fallback_enabled ||
		!r->targets.pass || !r->targets.framebuffer || r->pipelines.raster_pass!=r->targets.pass || !r->pipelines.raster ||
		!r->prepass_cutout || (snapshot->count && alternate->serial!=serial) || !isfinite(depth_clear) || depth_clear<0 || depth_clear>1 ||
		!RCReceiver_CanRecordCompute(primary,&r->targets,&r->pipelines,r->layouts.classify,r->layouts.direct,
			r->descriptors.classify,r->descriptors.direct,cascade_set,lighting_set,params) ||
		(snapshot->count && !RCReceiverForward_CanRecord(primary,r->layouts.forward,r->forward,snapshot,materials,serial))) return false;
	VkPipeline prepass[2]={r->pipelines.raster,r->prepass_cutout};
	if(!RCReceiverWorld_CanRecord(primary,r->layouts.prepass,prepass,snapshot->draws,snapshot->count)) return false;
	bool postprocess=snapshot->count ? materials[0].reuse_set==r->descriptors.forward_resolved : checkerboard || smooth_indirect || smooth_shadows;
	VkDescriptorSet receiver_set=postprocess ? r->descriptors.forward_resolved : r->descriptors.forward;
	if ((checkerboard && !postprocess) || (postprocess && !checkerboard && !smooth_indirect && !smooth_shadows) ||
		(smooth_indirect && (!postprocess || !texture_filter || !r->descriptors.reconstruct_reverse)) ||
		(smooth_shadows && (!postprocess || !texture_filter || !soft_shadows || !r->soft_shadows)) ||
		!receiver_set || (postprocess && (!r->reconstruct || !r->layouts.reconstruct || !r->descriptors.reconstruct))) return false;
	for(uint32_t i=0;i<snapshot->count;++i) if(materials[i].reuse_set!=receiver_set) return false;
	if(snapshot->count && (RCReceiverForward_Fingerprint(RC_RECEIVER_FORWARD_HASH_SEED,snapshot,materials)!=alternate->fingerprint ||
		!RCReceiverSelect_Stage(serial,alternate->buffers,alternate->count,true,alternate->pass,alternate->framebuffer,alternate->subpass))) return false;
	uint64_t model_fingerprint=0;
	if(models && models->count) {
		model_fingerprint=RCReceiverModel_Fingerprint(models);
		if(!texture_filter || !model_alternate || model_alternate->serial!=serial || model_alternate->count!=1 ||
			!model_alternate->buffers[0] || !model_alternate->pass || !model_alternate->framebuffer ||
			model_alternate->fingerprint!=model_fingerprint || r->models.receiver_pass!=r->targets.pass ||
			model_alternate->pass!=r->models.forward_pass || model_alternate->subpass!=r->models.forward_subpass ||
			!RCReceiverModel_CanRecord(primary,&r->models,models,serial,false,VK_NULL_HANDLE) ||
			!RCReceiverModel_CanRecord(primary,&r->models,models,serial,true,receiver_set)) {RCReceiverSelect_Reset();return false;}
		if(snapshot->count && models->draws[0].token<=snapshot->draws[snapshot->count-1].token) {RCReceiverSelect_Reset();return false;}
	}
	rc_receiver_frame_plan_t plan={.frame_serial=serial,.enabled=true,.edge_fallback_enabled=true,
		.targets=&r->targets,.pipelines=&r->pipelines,.classify_layout=r->layouts.classify,.direct_layout=r->layouts.direct,
		.world_layout=r->layouts.prepass,.classify_set=r->descriptors.classify,.direct_set=r->descriptors.direct,
		.cascade_set=cascade_set,.lighting_set=lighting_set,.world_pipelines={prepass[0],prepass[1]},.forward_materials=snapshot->count?materials:NULL,
		.forward_fingerprint=snapshot->count?alternate->fingerprint:0,.params=*params,.depth_clear=depth_clear,
		.model_pipelines=models && models->count?&r->models:NULL,.model_draws=models?models->draws:NULL,
		.model_draw_count=models?models->count:0,.model_fingerprint=model_fingerprint,
		.postprocess=postprocess,.checkerboard=checkerboard,.texture_filter=texture_filter,.gaussian_filter=smooth_indirect,.smooth_shadows=smooth_shadows,
		.shadow_filter_strength=shadow_filter_strength,
		.soft_shadows=soft_shadows && texture_filter && r->soft_shadows,
		.soft_shadow_pipeline=soft_shadows && texture_filter?r->soft_shadows:VK_NULL_HANDLE,
		.timing_pool=timing_pool,.timing_first_query=timing_first_query,.reconstruct_pipeline=r->reconstruct,
		.reconstruct_layout=r->layouts.reconstruct,.reconstruct_set=r->descriptors.reconstruct,.reconstruct_reverse_set=r->descriptors.reconstruct_reverse};
	if(!RCReceiverSnapshot_Stage(snapshot,&plan)) { RCReceiverSelect_Reset(); return false; }
	return true;
}

bool RCReceiverResources_StageFrame(const rc_receiver_resources_t *r,uint64_t serial,VkCommandBuffer primary,
	VkDescriptorSet cascade_set,VkDescriptorSet lighting_set,const rc_receiver_params_t *params,float depth_clear,
	const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *materials,
	const rc_receiver_alternates_t *alternate,bool complete_world,bool edge_fallback_enabled,bool texture_filter,bool checkerboard,bool smooth_indirect,bool smooth_shadows,bool soft_shadows,VkQueryPool timing_pool,uint32_t timing_first_query,float shadow_filter_strength)
{
	return RCReceiverResources_StageFrameModels(r,serial,primary,cascade_set,lighting_set,params,depth_clear,snapshot,materials,alternate,
		complete_world,edge_fallback_enabled,texture_filter,checkerboard,smooth_indirect,smooth_shadows,soft_shadows,timing_pool,timing_first_query,shadow_filter_strength,NULL,NULL);
}
