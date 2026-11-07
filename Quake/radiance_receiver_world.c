#include "radiance_receiver_world.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct {
	float mvp[16];
	uint32_t token;
	float fog_color_yz[2], fog_density, alpha;
	uint32_t instance_base;
} rc_receiver_world_push_t;
_Static_assert(sizeof(rc_receiver_world_push_t)==88 && offsetof(rc_receiver_world_push_t,token)==64 &&
	offsetof(rc_receiver_world_push_t,instance_base)==84, "world push layout mismatch");

bool RCReceiverWorld_CanRecord(VkCommandBuffer command, VkPipelineLayout layout,
	const VkPipeline pipelines[2], const rc_receiver_world_draw_t *draws, uint32_t count)
{
	if (!command || !layout || !pipelines || (count && !draws)) return false;
	for (uint32_t i=0;i<count;++i) {
		const rc_receiver_world_draw_t *d=&draws[i];
		if (!pipelines[d->alpha_test ? 1 : 0] || !d->vertex_buffer || !d->index_buffer ||
			!d->diffuse_set || !d->instance_set || !d->camera_set || !d->token || (i && d->token<=draws[i-1].token) ||
			(d->indirect_buffer ? d->index_count || d->first_index || d->indirect_offset%4 :
				!d->index_count || d->first_index>UINT32_MAX-d->index_count || d->index_count%3) ||
			(d->index_offset % (d->index_type==VK_INDEX_TYPE_UINT16 ? 2u : 4u)) ||
			(d->index_type!=VK_INDEX_TYPE_UINT16 && d->index_type!=VK_INDEX_TYPE_UINT32) ||
			!isfinite(d->depth_bias_constant) || !isfinite(d->depth_bias_slope)) return false;
		for (unsigned j=0;j<16;++j) if (!isfinite(d->mvp[j])) return false;
	}
	return true;
}
void RCReceiverWorld_Draw(VkCommandBuffer command, const rc_receiver_world_draw_t *d)
{
	if (d->indirect_buffer) vkCmdDrawIndexedIndirect(command,d->indirect_buffer,d->indirect_offset,1,0);
	else vkCmdDrawIndexed(command,d->index_count,1,d->first_index,d->vertex_offset_index,0);
}
void RCReceiverWorld_Record(VkCommandBuffer command, VkPipelineLayout layout,
	const VkPipeline pipelines[2], const rc_receiver_world_draw_t *draws, uint32_t count)
{
	for (uint32_t i=0;i<count;++i) {
		const rc_receiver_world_draw_t *d=&draws[i];
		rc_receiver_world_push_t push={0}; memcpy(push.mvp,d->mvp,sizeof(push.mvp));
		push.token=d->token; push.instance_base=d->instance_base;
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[d->alpha_test ? 1 : 0]);
		/* Positions are rebuilt from this depth, so a slope bias would sink them
		 * into the surface at oblique angles. The forward draw keeps the full bias. */
		vkCmdSetDepthBias(command,d->depth_bias_constant,0,0);
		vkCmdBindVertexBuffers(command,0,1,&d->vertex_buffer,&d->vertex_offset);
		vkCmdBindIndexBuffer(command,d->index_buffer,d->index_offset,d->index_type);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&d->diffuse_set,0,NULL);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,4,1,&d->instance_set,0,NULL);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,5,1,&d->camera_set,0,NULL);
		vkCmdPushConstants(command,layout,VK_SHADER_STAGE_ALL_GRAPHICS,0,sizeof(push),&push);
		RCReceiverWorld_Draw(command,d);
	}
}
