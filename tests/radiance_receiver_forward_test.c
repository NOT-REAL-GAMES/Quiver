#include "radiance_receiver_forward.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned pipelines_bound, sets, pushes, draws;
bool RCReceiverWorld_CanRecord(VkCommandBuffer c,VkPipelineLayout l,const VkPipeline p[2],const rc_receiver_world_draw_t *d,uint32_t n)
{ return c && l && p[d->alpha_test?1:0] && n==1 && d->token; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p)
{ (void)c; assert(b==VK_PIPELINE_BIND_POINT_GRAPHICS && p==H(VkPipeline,4)); ++pipelines_bound; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer c,float a,float b,float d)
{ (void)c;(void)a;(void)b;(void)d; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,const VkDeviceSize *o)
{ (void)c;(void)f;(void)n;(void)b;(void)o; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t)
{ (void)c;(void)b;(void)o;(void)t; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t first,uint32_t count,const VkDescriptorSet *s,uint32_t n,const uint32_t *o)
{ (void)c;(void)l;(void)s;(void)n;(void)o; const uint32_t expected[]={0,4,5,6}; assert(b==VK_PIPELINE_BIND_POINT_GRAPHICS && first==expected[sets] && count==(sets?1u:3u)); ++sets; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags stage,uint32_t off,uint32_t size,const void *data)
{ (void)c;(void)l; uint32_t token; memcpy(&token,(const char *)data+88,4); assert(stage==VK_SHADER_STAGE_ALL_GRAPHICS && off==0 && size==92 && token==37); ++pushes; }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c,uint32_t count,uint32_t instances,uint32_t first,int32_t vertex,uint32_t start)
{ (void)c; assert(count==6 && instances==1 && first==0 && vertex==0 && start==0); ++draws; }
void RCReceiverWorld_Draw(VkCommandBuffer c,const rc_receiver_world_draw_t *d) { vkCmdDrawIndexed(c,d->index_count,1,d->first_index,d->vertex_offset_index,0); }
int main(void)
{
	VkCommandBuffer command=H(VkCommandBuffer,1); VkPipelineLayout layout=H(VkPipelineLayout,1);
	VkPipeline pipelines[4]={H(VkPipeline,1),H(VkPipeline,2),H(VkPipeline,3),H(VkPipeline,4)};
	rc_receiver_world_draw_t d={.token=37,.alpha_test=true,.index_count=6};
	rc_receiver_world_snapshot_t s={.frame_serial=11,.draws=&d,.count=1};
	rc_receiver_forward_material_t m={.lightmap_set=H(VkDescriptorSet,1),.fullbright_set=H(VkDescriptorSet,2),.reuse_set=H(VkDescriptorSet,3),.fullbright=true};
	assert(RCReceiverForward_CanRecord(command,layout,pipelines,&s,&m,11));
	uint64_t hash=RCReceiverForward_Fingerprint(RC_RECEIVER_FORWARD_HASH_SEED,&s,&m);
	++d.index_count; assert(hash!=RCReceiverForward_Fingerprint(RC_RECEIVER_FORWARD_HASH_SEED,&s,&m)); --d.index_count;
	m.fog_density=0.5f; assert(hash!=RCReceiverForward_Fingerprint(RC_RECEIVER_FORWARD_HASH_SEED,&s,&m)); m.fog_density=0;
	assert(!RCReceiverForward_CanRecord(command,layout,pipelines,&s,&m,12));
	m.reuse_set=0; assert(!RCReceiverForward_CanRecord(command,layout,pipelines,&s,&m,11)); m.reuse_set=H(VkDescriptorSet,3);
	assert(!pipelines_bound && !sets && !pushes && !draws);
	RCReceiverForward_Record(command,layout,pipelines,&s,&m);
	assert(pipelines_bound==1 && sets==4 && pushes==1 && draws==1);
	puts("PASS: forward world binds compatible descriptors and matching draw token");
	return 0;
}
