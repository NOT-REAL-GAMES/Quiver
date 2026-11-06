#include "radiance_receiver_record.h"
#include <math.h>
#include <stddef.h>

_Static_assert(sizeof(rc_receiver_params_t)==112 && offsetof(rc_receiver_params_t, viewport)==64 &&
	offsetof(rc_receiver_params_t, ndc_transform)==80 && offsetof(rc_receiver_params_t, samples)==96 &&
	offsetof(rc_receiver_params_t, divisor)==108,
	"receiver push constants must match GLSL");

static VkImageMemoryBarrier RCReceiver_Barrier(VkImage image, VkImageLayout from, VkImageLayout to,
	VkAccessFlags read, VkAccessFlags write)
{
	return (VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=read,.dstAccessMask=write,
		.oldLayout=from,.newLayout=to,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
		.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
}

bool RCReceiver_CanRecordCompute(VkCommandBuffer command, const rc_receiver_targets_t *t,
	const rc_receiver_pipelines_t *p, VkPipelineLayout classify_layout, VkPipelineLayout direct_layout,
	VkDescriptorSet classify_set, VkDescriptorSet direct_set, VkDescriptorSet cascade_set,
	VkDescriptorSet lighting_set, const rc_receiver_params_t *params)
{
	if (!command || !t || !p || !params || !t->device || t->device!=p->device || !p->classify || !p->direct ||
		!classify_layout || !direct_layout || !classify_set || !direct_set || !cascade_set || !lighting_set ||
		params->samples!=1 || !RCReceiver_ValidDivisor(params->divisor) || params->viewport[0]<0 || params->viewport[1]<0 || params->viewport[2]<=0 || params->viewport[3]<=0 ||
		(uint64_t)params->viewport[0]+(uint32_t)params->viewport[2]>t->width ||
		(uint64_t)params->viewport[1]+(uint32_t)params->viewport[3]>t->height || !isfinite(params->plane_tolerance) ||
		params->plane_tolerance<0 || !isfinite(params->normal_cosine) || params->normal_cosine< -1 || params->normal_cosine>1)
		return false;
	for (unsigned i=0;i<16;++i) if (!isfinite(params->inverse_view_projection[i])) return false;
	for (unsigned i=0;i<4;++i) if (!isfinite(params->ndc_transform[i])) return false;
	for (unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i) if (!t->images[i].image || !t->images[i].view) return false;
	return true;
}

static bool RCReceiver_RecordComputeTimed(VkCommandBuffer command, const rc_receiver_targets_t *t,
	const rc_receiver_pipelines_t *p, VkPipelineLayout classify_layout, VkPipelineLayout direct_layout,
	VkDescriptorSet classify_set, VkDescriptorSet direct_set, VkDescriptorSet cascade_set,
	VkDescriptorSet lighting_set, const rc_receiver_params_t *params,VkQueryPool timing_pool,uint32_t first_query,VkPipeline soft_shadow_pipeline)
{
	if (!RCReceiver_CanRecordCompute(command,t,p,classify_layout,direct_layout,classify_set,direct_set,cascade_set,lighting_set,params)) return false;
	uint32_t width=RCReceiver_ScaledExtent((uint32_t)params->viewport[2],params->divisor);
	uint32_t height=RCReceiver_ScaledExtent((uint32_t)params->viewport[3],params->divisor);
	uint32_t groups_x=width/8+(width%8!=0),groups_y=height/8+(height%8!=0);
	/* Discard last use only after this frame slot's fence has completed. */
	VkImageMemoryBarrier initial[6];
	for (unsigned i=0;i<6;++i) initial[i]=RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_POSITION+i].image,
		VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT);
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0,0,NULL,0,NULL,6,initial);
	vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,p->classify);
	vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,classify_layout,0,1,&classify_set,0,NULL);
	vkCmdPushConstants(command,classify_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(*params),params);
	vkCmdDispatch(command,groups_x,groups_y,1);
	VkImageMemoryBarrier between[3]={
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_POSITION].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_NORMAL].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_STATUS].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT)};
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0,0,NULL,0,NULL,3,between);
	if(timing_pool) {
		vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timing_pool,first_query+1);
		vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timing_pool,first_query+2);
	}
	vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,p->direct);
	VkDescriptorSet sets[2]={direct_set,cascade_set};
	vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,direct_layout,0,2,sets,0,NULL);
	int32_t extent[2]={(int32_t)width,(int32_t)height};
	vkCmdPushConstants(command,direct_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(extent),extent);
	vkCmdDispatch(command,groups_x,groups_y,1);
	if(soft_shadow_pipeline) {
		/* Raw direct/status remain GENERAL until the ordinary final barriers.
		 * Only this opt-in pass needs their producer writes in read/write use. */
		VkMemoryBarrier soft_ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0,1,&soft_ready,0,NULL,0,NULL);
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,soft_shadow_pipeline);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,direct_layout,0,2,sets,0,NULL);
		vkCmdPushConstants(command,direct_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(extent),extent);
		vkCmdDispatch(command,groups_x,groups_y,1);
	}
	VkImageMemoryBarrier final[6]={
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_INDIRECT].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_STATUS].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_DIRECT].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_MINIMUM].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_POSITION].image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),
		RCReceiver_Barrier(t->images[RC_RECEIVER_HALF_NORMAL].image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT)};
	vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0,0,NULL,0,NULL,6,final);
	if(timing_pool)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timing_pool,first_query+3);
	return true;
}

bool RCReceiver_RecordCompute(VkCommandBuffer command, const rc_receiver_targets_t *t,
	const rc_receiver_pipelines_t *p, VkPipelineLayout classify_layout, VkPipelineLayout direct_layout,
	VkDescriptorSet classify_set, VkDescriptorSet direct_set, VkDescriptorSet cascade_set,
	VkDescriptorSet lighting_set, const rc_receiver_params_t *params)
{
	return RCReceiver_RecordComputeTimed(command,t,p,classify_layout,direct_layout,classify_set,direct_set,
		cascade_set,lighting_set,params,VK_NULL_HANDLE,0,VK_NULL_HANDLE);
}

bool RCReceiver_RecordFrameSoftTimed(VkCommandBuffer command, const rc_receiver_targets_t *t,
	const rc_receiver_pipelines_t *p, VkPipelineLayout classify_layout, VkPipelineLayout direct_layout,
	VkDescriptorSet classify_set, VkDescriptorSet direct_set, VkDescriptorSet cascade_set,
	VkDescriptorSet lighting_set, const rc_receiver_params_t *params, float depth_clear,
	bool (*draw_ready)(void *), void (*draw)(VkCommandBuffer, void *), void *user,VkQueryPool timing_pool,uint32_t first_query,VkPipeline soft_shadow_pipeline)
{
	if (!RCReceiver_CanRecordCompute(command,t,p,classify_layout,direct_layout,classify_set,direct_set,cascade_set,lighting_set,params) ||
		!t->pass || !t->framebuffer || !p->raster || p->raster_pass!=t->pass || !draw_ready || !draw || !isfinite(depth_clear) ||
		depth_clear<0 || depth_clear>1 || (timing_pool && first_query>UINT32_MAX-5u) || !draw_ready(user)) return false;
	if(timing_pool)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timing_pool,first_query);
	VkClearValue clear[3]={0}; clear[RC_RECEIVER_DEPTH].depthStencil.depth=depth_clear;
	VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=t->pass,.framebuffer=t->framebuffer,
		.renderArea={{0,0},{t->width,t->height}},.clearValueCount=3,.pClearValues=clear};
	vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);
	/* Dynamic viewport/scissor are command-buffer state, not inherited from
	 * world secondaries. Match the same scene viewport as the forward draws. */
	VkViewport viewport={(float)params->viewport[0],(float)params->viewport[1],
		(float)params->viewport[2],(float)params->viewport[3],0.0f,1.0f};
	VkRect2D scissor={{0,0},{t->width,t->height}};
	vkCmdSetViewport(command,0,1,&viewport);
	vkCmdSetScissor(command,0,1,&scissor);
	vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->raster);
	draw(command,user);
	vkCmdEndRenderPass(command);
	/* All checks succeeded before recording; draw promises not to mutate them. */
	return RCReceiver_RecordComputeTimed(command,t,p,classify_layout,direct_layout,classify_set,direct_set,cascade_set,lighting_set,params,timing_pool,first_query,soft_shadow_pipeline);
}

bool RCReceiver_RecordFrameTimed(VkCommandBuffer command, const rc_receiver_targets_t *t,
	const rc_receiver_pipelines_t *p, VkPipelineLayout classify_layout, VkPipelineLayout direct_layout,
	VkDescriptorSet classify_set, VkDescriptorSet direct_set, VkDescriptorSet cascade_set,
	VkDescriptorSet lighting_set, const rc_receiver_params_t *params, float depth_clear,
	bool (*draw_ready)(void *), void (*draw)(VkCommandBuffer, void *), void *user,VkQueryPool timing_pool,uint32_t first_query)
{
	return RCReceiver_RecordFrameSoftTimed(command,t,p,classify_layout,direct_layout,classify_set,direct_set,cascade_set,
		lighting_set,params,depth_clear,draw_ready,draw,user,timing_pool,first_query,VK_NULL_HANDLE);
}

bool RCReceiver_RecordFrame(VkCommandBuffer command, const rc_receiver_targets_t *t,
	const rc_receiver_pipelines_t *p, VkPipelineLayout classify_layout, VkPipelineLayout direct_layout,
	VkDescriptorSet classify_set, VkDescriptorSet direct_set, VkDescriptorSet cascade_set,
	VkDescriptorSet lighting_set, const rc_receiver_params_t *params, float depth_clear,
	bool (*draw_ready)(void *), void (*draw)(VkCommandBuffer, void *), void *user)
{
	return RCReceiver_RecordFrameTimed(command,t,p,classify_layout,direct_layout,classify_set,direct_set,
		cascade_set,lighting_set,params,depth_clear,draw_ready,draw,user,VK_NULL_HANDLE,0);
}
