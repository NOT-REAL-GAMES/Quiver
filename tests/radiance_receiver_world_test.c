#include "radiance_receiver_world.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define HANDLE(type,n) ((type)(uintptr_t)(n))
static unsigned binds,sets,pushes,draws;
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p)
{ (void)c; assert(b==VK_PIPELINE_BIND_POINT_GRAPHICS && p==HANDLE(VkPipeline,1+draws)); ++binds; }
/* Prepass depth rebuilds receiver positions, so it must never carry the slope term. */
static float expected_bias;
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer c,float a,float b,float d)
{ (void)c; assert(a==expected_bias && b==0 && d==0); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t n,const VkBuffer *b,const VkDeviceSize *offset)
{ (void)c;(void)b;(void)offset; assert(first==0 && n==1); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize offset,VkIndexType type)
{ (void)c;(void)b;(void)offset; assert(type==VK_INDEX_TYPE_UINT32); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t first,uint32_t count,const VkDescriptorSet *s,uint32_t nd,const uint32_t *o)
{ (void)c;(void)l;(void)s;(void)nd;(void)o; const uint32_t expected[]={0,4,5}; assert(b==VK_PIPELINE_BIND_POINT_GRAPHICS && count==1 && first==expected[sets%3]); ++sets; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags stage,uint32_t off,uint32_t size,const void *data)
{ (void)c;(void)l; assert(stage==VK_SHADER_STAGE_ALL_GRAPHICS && off==0 && size==88); uint32_t token; memcpy(&token,(const char*)data+64,4); assert(token==11+draws); ++pushes; }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c,uint32_t count,uint32_t inst,uint32_t first,int32_t vertex,uint32_t firstinst)
{ (void)c; assert(count==6 && inst==1 && first==3 && vertex==2 && firstinst==0); ++draws; }
static unsigned indirect_draws;
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize offset,uint32_t count,uint32_t stride)
{ (void)c; assert(b==HANDLE(VkBuffer,9) && offset==40 && count==1 && !stride); ++indirect_draws; ++draws; }
int main(void)
{
	VkCommandBuffer command=HANDLE(VkCommandBuffer,1); VkPipelineLayout layout=HANDLE(VkPipelineLayout,1);
	VkPipeline pipelines[2]={HANDLE(VkPipeline,1),HANDLE(VkPipeline,2)};
	rc_receiver_world_draw_t d[2]={0};
	for(unsigned i=0;i<2;++i) { d[i].vertex_buffer=HANDLE(VkBuffer,1); d[i].index_buffer=HANDLE(VkBuffer,2); d[i].index_type=VK_INDEX_TYPE_UINT32; d[i].index_count=6; d[i].first_index=3; d[i].vertex_offset_index=2; d[i].token=11+i; d[i].alpha_test=i!=0; d[i].diffuse_set=HANDLE(VkDescriptorSet,1); d[i].instance_set=HANDLE(VkDescriptorSet,2); d[i].camera_set=HANDLE(VkDescriptorSet,3); }
	assert(RCReceiverWorld_CanRecord(command,layout,pipelines,d,2));
	d[1].token=11; assert(!RCReceiverWorld_CanRecord(command,layout,pipelines,d,2)); d[1].token=12;
	d[1].index_offset=2; assert(!RCReceiverWorld_CanRecord(command,layout,pipelines,d,2)); d[1].index_offset=0;
	d[1].index_count=5; assert(!RCReceiverWorld_CanRecord(command,layout,pipelines,d,2)); d[1].index_count=6;
	assert(!draws && !binds && !sets && !pushes);
	expected_bias=-4; for(unsigned i=0;i<2;++i) { d[i].depth_bias_constant=-4; d[i].depth_bias_slope=-.125f; }
	RCReceiverWorld_Record(command,layout,pipelines,d,2);
	assert(draws==2 && binds==2 && sets==6 && pushes==2 && !indirect_draws);
	/* GPU-built draws carry a command instead of CPU counts, and never both. */
	draws=binds=sets=pushes=0; d[1].indirect_buffer=HANDLE(VkBuffer,9); d[1].indirect_offset=40;
	assert(!RCReceiverWorld_CanRecord(command,layout,pipelines,d,2));
	d[1].index_count=0; d[1].first_index=0; d[1].indirect_offset=42;
	assert(!RCReceiverWorld_CanRecord(command,layout,pipelines,d,2));
	d[1].indirect_offset=40; assert(RCReceiverWorld_CanRecord(command,layout,pipelines,d,2));
	RCReceiverWorld_Record(command,layout,pipelines,d,2);
	assert(draws==2 && indirect_draws==1);
	puts("PASS: world receiver direct draws preserve per-draw identity and opaque preflight");
	return 0;
}
