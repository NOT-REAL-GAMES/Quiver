#include "radiance_receiver_forward.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
typedef struct { float mvp[16], fog_color[3], fog_density, alpha; uint32_t instance_base, token; } forward_push_t;
_Static_assert(sizeof(forward_push_t)==92 && offsetof(forward_push_t,token)==88, "receiver forward push layout mismatch");
bool RCReceiverForward_CanRecord(VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline pipelines[4],
	const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *materials,uint64_t serial)
{
	if (!command || !layout || !pipelines || !snapshot || !snapshot->draws || !snapshot->count ||
		!materials || snapshot->frame_serial!=serial) return false;
	for (uint32_t i=0;i<snapshot->count;++i) {
		const rc_receiver_world_draw_t *d=&snapshot->draws[i]; const rc_receiver_forward_material_t *m=&materials[i];
		VkPipeline check[2]={pipelines[m->fullbright?1:0],pipelines[m->fullbright?3:2]};
		if (!RCReceiverWorld_CanRecord(command,layout,check,d,1) || !m->lightmap_set || !m->fullbright_set || !m->reuse_set ||
			!isfinite(m->fog_color[0]) || !isfinite(m->fog_color[1]) || !isfinite(m->fog_color[2]) ||
			!isfinite(m->fog_density) || !isfinite(m->alpha)) return false;
	}
	return true;
}
static uint64_t hash_bytes(uint64_t h,const void *data,size_t size)
{
	const unsigned char *p=data; for(size_t i=0;i<size;++i){h^=p[i];h*=UINT64_C(1099511628211);} return h;
}
uint64_t RCReceiverForward_Fingerprint(uint64_t h,const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *materials)
{
	for(uint32_t i=0;i<snapshot->count;++i) {
		const rc_receiver_world_draw_t *d=&snapshot->draws[i]; const rc_receiver_forward_material_t *m=&materials[i];
#define HASH(v) h=hash_bytes(h,&(v),sizeof(v))
		HASH(d->vertex_buffer); HASH(d->index_buffer); HASH(d->vertex_offset); HASH(d->index_offset); HASH(d->index_type);
		HASH(d->index_count); HASH(d->first_index); HASH(d->token); HASH(d->vertex_offset_index);
		HASH(d->diffuse_set); HASH(d->instance_set); HASH(d->camera_set); HASH(d->mvp); HASH(d->instance_base);
		unsigned char alpha=d->alpha_test ? 1 : 0; HASH(alpha); HASH(d->depth_bias_constant); HASH(d->depth_bias_slope);
		HASH(m->lightmap_set); HASH(m->fullbright_set); HASH(m->reuse_set); HASH(m->fog_color); HASH(m->fog_density); HASH(m->alpha);
		unsigned char fullbright=m->fullbright ? 1 : 0; HASH(fullbright);
#undef HASH
	}
	return h;
}
void RCReceiverForward_Record(VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline pipelines[4],
	const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *materials)
{
	for (uint32_t i=0;i<snapshot->count;++i) {
		const rc_receiver_world_draw_t *d=&snapshot->draws[i]; const rc_receiver_forward_material_t *m=&materials[i];
		forward_push_t push={0}; memcpy(push.mvp,d->mvp,sizeof(push.mvp)); memcpy(push.fog_color,m->fog_color,sizeof(push.fog_color));
		push.fog_density=m->fog_density; push.alpha=m->alpha; push.instance_base=d->instance_base; push.token=d->token;
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[(m->fullbright?1:0)+(d->alpha_test?2:0)]);
		vkCmdSetDepthBias(command,d->depth_bias_constant,0,d->depth_bias_slope);
		vkCmdBindVertexBuffers(command,0,1,&d->vertex_buffer,&d->vertex_offset);
		vkCmdBindIndexBuffer(command,d->index_buffer,d->index_offset,d->index_type);
		VkDescriptorSet initial[3]={d->diffuse_set,m->lightmap_set,m->fullbright_set};
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,3,initial,0,NULL);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,4,1,&d->instance_set,0,NULL);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,5,1,&d->camera_set,0,NULL);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,6,1,&m->reuse_set,0,NULL);
		vkCmdPushConstants(command,layout,VK_SHADER_STAGE_ALL_GRAPHICS,0,sizeof(push),&push);
		vkCmdDrawIndexed(command,d->index_count,1,d->first_index,d->vertex_offset_index,0);
	}
}
