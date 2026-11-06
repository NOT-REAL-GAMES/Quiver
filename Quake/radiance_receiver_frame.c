#include "radiance_receiver_frame.h"
#include "radiance_receiver_forward.h"
#include <math.h>
#include <string.h>
static rc_receiver_frame_plan_t staged;
static bool pending, active, active_soft;
static uint64_t active_frame;
typedef struct { VkCommandBuffer command; const rc_receiver_frame_plan_t *p; } world_context_t;
void RCReceiverFrame_Reset(void) { memset(&staged,0,sizeof(staged)); pending=active=active_soft=false; active_frame=0; }
void RCReceiverFrame_Stage(const rc_receiver_frame_plan_t *plan)
{
	/* Replacing a plan invalidates any previous result, including same-frame. */
	active=active_soft=false; pending=plan!=NULL;
	if (plan) staged=*plan; else memset(&staged,0,sizeof(staged));
}
static bool world_ready(void *user)
{
	const world_context_t *ctx=user;
	const rc_receiver_frame_plan_t *p=ctx->p;
	rc_receiver_model_snapshot_t models={p->frame_serial,p->model_draws,p->model_draw_count};
	return (p->world_draw_count || RCReceiverModel_EligibleCount(&models)) &&
		(!p->world_draw_count || RCReceiverWorld_CanRecord(ctx->command,p->world_layout,p->world_pipelines,p->world_draws,p->world_draw_count)) &&
		(!p->model_draw_count || RCReceiverModel_CanRecord(ctx->command,p->model_pipelines,&models,p->frame_serial,false,VK_NULL_HANDLE));
}
static void world_draw(VkCommandBuffer command, void *user)
{
	const world_context_t *ctx=user;
	const rc_receiver_frame_plan_t *p=ctx->p;
	if(p->world_draw_count)RCReceiverWorld_Record(command,p->world_layout,p->world_pipelines,p->world_draws,p->world_draw_count);
	if(p->model_draw_count) {
		rc_receiver_model_snapshot_t models={p->frame_serial,p->model_draws,p->model_draw_count};
		RCReceiverModel_Record(command,p->model_pipelines,&models,false,VK_NULL_HANDLE);
	}
}
static VkImageMemoryBarrier filter_barrier(VkImage image,VkImageLayout from,VkImageLayout to,VkAccessFlags source,VkAccessFlags destination)
{
	return (VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=source,.dstAccessMask=destination,
		.oldLayout=from,.newLayout=to,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
		.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
}
/* The indirect images alternate sampled and storage roles between the two
 * axes. Direct and minimum stay in their first-pass output images. */
static void filter_swap(VkCommandBuffer command,const rc_receiver_frame_plan_t *p,bool reverse)
{
	const unsigned source=reverse?RC_RECEIVER_HALF_INDIRECT:RC_RECEIVER_RESOLVED_INDIRECT;
	const unsigned destination=reverse?RC_RECEIVER_RESOLVED_INDIRECT:RC_RECEIVER_HALF_INDIRECT;
	VkImageMemoryBarrier images[2]={
		filter_barrier(p->targets->images[source].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		filter_barrier(p->targets->images[destination].image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT)};
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,2,images);
}
static void reconstruct(VkCommandBuffer command,const rc_receiver_frame_plan_t *p)
{
	VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
	VkImageMemoryBarrier outputs[3];
	for(unsigned i=0;i<3;++i) outputs[i]=filter_barrier(p->targets->images[RC_RECEIVER_RESOLVED_INDIRECT+i].image,
		VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT);
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,3,outputs);
	VkDescriptorSet sets[2]={p->reconstruct_set,p->cascade_set};
	vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,p->reconstruct_pipeline);
	struct { int32_t viewport[4]; float thresholds[4]; } constants;
	memcpy(constants.viewport,p->params.viewport,sizeof(constants.viewport));
	constants.thresholds[0]=p->params.normal_cosine; constants.thresholds[1]=p->params.plane_tolerance;
	constants.thresholds[2]=p->params.divisor; constants.thresholds[3]=0;
	uint32_t w=RCReceiver_ScaledExtent((uint32_t)p->params.viewport[2],p->params.divisor);
	uint32_t h=RCReceiver_ScaledExtent((uint32_t)p->params.viewport[3],p->params.divisor);
	unsigned passes=p->gaussian_filter?3u:1u;
	for(unsigned pass=0;pass<passes;++pass) {
		if(pass) filter_swap(command,p,pass==2);
		sets[0]=pass==1?p->reconstruct_reverse_set:p->reconstruct_set;
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,p->reconstruct_layout,0,2,sets,0,NULL);
		constants.thresholds[3]=(float)pass;
		vkCmdPushConstants(command,p->reconstruct_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),&constants);
		if(p->texture_filter) {
			if(pass==1) vkCmdDispatch(command,(h+127)/128,w,1);
			else vkCmdDispatch(command,(w+127)/128,h,1);
		} else vkCmdDispatch(command,(w+7)/8,(h+7)/8,1);
	}
	for(unsigned i=0;i<3;++i) { outputs[i].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; outputs[i].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
		outputs[i].oldLayout=VK_IMAGE_LAYOUT_GENERAL; outputs[i].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,3,outputs);
}
bool RCReceiverFrame_Record(VkCommandBuffer command, uint64_t frame_serial)
{
	active=active_soft=false;
	if (!pending) return false;
	pending=false;
	const rc_receiver_frame_plan_t *p=&staged;
	if (!p->enabled || !p->edge_fallback_enabled || p->frame_serial!=frame_serial ||
		!isfinite(p->shadow_filter_strength) || p->shadow_filter_strength<0 || p->shadow_filter_strength>2 ||
		(p->smooth_shadows!=(p->shadow_filter_strength>0)) ||
		(p->timing_pool && p->timing_first_query>UINT32_MAX-5u) ||
		(p->checkerboard && !p->postprocess) || (p->postprocess && !p->checkerboard && !p->gaussian_filter && !p->smooth_shadows) ||
		(p->gaussian_filter && (!p->postprocess || !p->texture_filter || !p->reconstruct_reverse_set)) ||
		(p->smooth_shadows && (!p->postprocess || !p->texture_filter || !p->soft_shadows || !p->soft_shadow_pipeline)) ||
		(p->postprocess && (!p->reconstruct_pipeline || !p->reconstruct_layout || !p->reconstruct_set))) return false;
	if (p->forward_materials) {
		rc_receiver_world_snapshot_t snapshot={.frame_serial=frame_serial,.draws=(rc_receiver_world_draw_t *)p->world_draws,.count=p->world_draw_count};
		if (!snapshot.draws || !snapshot.count || RCReceiverForward_Fingerprint(RC_RECEIVER_FORWARD_HASH_SEED,&snapshot,p->forward_materials)!=p->forward_fingerprint) return false;
	}
	if(p->model_draw_count) {
		rc_receiver_model_snapshot_t models={p->frame_serial,p->model_draws,p->model_draw_count};
		if(!p->model_draws || RCReceiverModel_Fingerprint(&models)!=p->model_fingerprint)return false;
	}
	world_context_t ctx={command,p};
	VkPipeline soft=p->texture_filter && p->soft_shadows?p->soft_shadow_pipeline:VK_NULL_HANDLE;
	if (!RCReceiver_RecordFrameSoftTimed(command,p->targets,p->pipelines,p->classify_layout,p->direct_layout,
		p->classify_set,p->direct_set,p->cascade_set,p->lighting_set,&p->params,p->depth_clear,world_ready,world_draw,&ctx,p->timing_pool,p->timing_first_query,soft)) return false;
	if(p->timing_pool)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timing_pool,p->timing_first_query+4);
	if(p->postprocess) reconstruct(command,p);
	if(p->timing_pool)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timing_pool,p->timing_first_query+5);
	active=true; active_soft=soft!=VK_NULL_HANDLE; active_frame=frame_serial; return true;
}
const rc_receiver_targets_t *RCReceiverFrame_PendingTargets(uint64_t serial)
{ return pending && staged.frame_serial==serial && staged.enabled && staged.edge_fallback_enabled ? staged.targets : NULL; }
bool RCReceiverFrame_Active(uint64_t frame_serial) { return active && active_frame==frame_serial; }

float RCReceiverFrame_ActiveDivisor(uint64_t serial)
{ return RCReceiverFrame_Active(serial) ? staged.params.divisor : 1; }
bool RCReceiverFrame_ActiveCheckerboard(uint64_t serial)
{ return RCReceiverFrame_Active(serial) && staged.checkerboard; }
uint32_t RCReceiverFrame_ActiveFilterMode(uint64_t serial)
{ return RCReceiverFrame_Active(serial) && staged.texture_filter ? 1u : 0u; }
const rc_receiver_params_t *RCReceiverFrame_ActiveParams(uint64_t serial)
{ return RCReceiverFrame_Active(serial) ? &staged.params : NULL; }
bool RCReceiverFrame_ActivePostprocess(uint64_t serial)
{ return RCReceiverFrame_Active(serial) && staged.postprocess; }
bool RCReceiverFrame_ActiveSoftShadows(uint64_t serial)
{ return RCReceiverFrame_Active(serial) && active_soft; }
float RCReceiverFrame_ActiveShadowFilter(uint64_t serial)
{ return RCReceiverFrame_ActiveSoftShadows(serial) && staged.postprocess && staged.smooth_shadows ? staged.shadow_filter_strength : 0; }

const rc_receiver_targets_t *RCReceiverFrame_ActiveTargets(uint64_t serial)
{ return RCReceiverFrame_Active(serial) ? staged.targets : NULL; }
