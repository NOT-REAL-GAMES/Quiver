#include "radiance_receiver_record.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static unsigned barriers, binds, sets, pushes, dispatches, begins, ends, draws, viewports, scissors;
static bool ready,expect_soft;
static unsigned timestamp_writes;
static unsigned expected_x=80, expected_y=45;
static float expected_divisor=2;
static bool draw_ready(void *user) { (void)user; return ready; }
static void draw(VkCommandBuffer command, void *user) { (void)command; (void)user; ++draws; }
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *i,VkSubpassContents contents)
{ (void)c; assert(i->renderArea.extent.width==1290 && i->renderArea.extent.height==726 && i->clearValueCount==3 && i->pClearValues[0].depthStencil.depth==1.0f && contents==VK_SUBPASS_CONTENTS_INLINE); ++begins; }
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c) { (void)c; ++ends; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t count,const VkViewport *v)
{ (void)c; assert(begins==ends+1 && first==0 && count==1 && v->x==11 && v->y==7 && v->width==1279 && v->height==719 && v->minDepth==0 && v->maxDepth==1); ++viewports; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t count,const VkRect2D *s)
{ (void)c; assert(begins==ends+1 && first==0 && count==1 && s->offset.x==0 && s->offset.y==0 && s->extent.width==1290 && s->extent.height==726); ++scissors; }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,
	VkDependencyFlags dep,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,
	uint32_t ni,const VkImageMemoryBarrier *images)
{
	(void)c;(void)dep;(void)nb;(void)b;
	if(barriers==0) { assert(src==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && dst==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && ni==6);
		for(unsigned i=0;i<ni;++i) assert(images[i].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && images[i].newLayout==VK_IMAGE_LAYOUT_GENERAL); }
	if(barriers==1) { assert(src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && dst==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && ni==3);
		assert(images[0].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && images[2].newLayout==VK_IMAGE_LAYOUT_GENERAL); }
	if(barriers==2 && expect_soft) { assert(src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && dst==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && nm==1 && !ni);
        assert(m->srcAccessMask==VK_ACCESS_SHADER_WRITE_BIT && m->dstAccessMask==(VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT)); }
    if(barriers==(expect_soft?3u:2u)) { assert(src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && dst==(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) && ni==6);
		for(unsigned i=0;i<ni;++i) assert(images[i].oldLayout==(i<4?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) && images[i].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
	++barriers;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint point,VkPipeline p)
{ (void)c;(void)p; assert(point==VK_PIPELINE_BIND_POINT_COMPUTE || (point==VK_PIPELINE_BIND_POINT_GRAPHICS && begins==ends+1)); ++binds; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint point,VkPipelineLayout l,
	uint32_t first,uint32_t count,const VkDescriptorSet *s,uint32_t nd,const uint32_t *offsets)
{ (void)c;(void)l;(void)s;(void)nd;(void)offsets; assert(point==VK_PIPELINE_BIND_POINT_COMPUTE && first==0 && count==(sets ? 2u : 1u)); ++sets; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags stage,uint32_t off,uint32_t size,const void *data)
{ (void)c;(void)l; assert(stage==VK_SHADER_STAGE_COMPUTE_BIT && off==0 && size==(pushes ? 8u : 112u));
 if(!pushes) assert(((const rc_receiver_params_t *)data)->divisor==expected_divisor);
 else { const int32_t *e=data; assert(e[0]==(int32_t)RCReceiver_ScaledExtent(1279,expected_divisor) && e[1]==(int32_t)RCReceiver_ScaledExtent(719,expected_divisor)); } ++pushes; }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z)
{ (void)c; assert(x==expected_x && y==expected_y && z==1); ++dispatches; }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t query)
{ (void)c;assert(pool==(VkQueryPool)(uintptr_t)1 && query==12+(timestamp_writes%4));assert(stage==(timestamp_writes%4==0?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT));++timestamp_writes; }
int main(void)
{
	VkCommandBuffer command=(VkCommandBuffer)(uintptr_t)1;
	VkDevice device=(VkDevice)(uintptr_t)1;
	rc_receiver_targets_t targets={.device=device,.width=1290,.height=726};
	for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i) { targets.images[i].image=(VkImage)(uintptr_t)(i+1); targets.images[i].view=(VkImageView)(uintptr_t)(i+1); }
	rc_receiver_pipelines_t p={.device=device,.classify=(VkPipeline)(uintptr_t)1,.direct=(VkPipeline)(uintptr_t)2};
	rc_receiver_params_t params={.viewport={11,7,1279,719},.samples=1,.divisor=2,.plane_tolerance=.01f,.normal_cosine=.99f};
	VkPipelineLayout layout=(VkPipelineLayout)(uintptr_t)1;
	VkDescriptorSet set=(VkDescriptorSet)(uintptr_t)1;
	assert(RCReceiver_RecordCompute(command,&targets,&p,layout,layout,set,set,set,set,&params));
	assert(barriers==3 && binds==2 && sets==2 && pushes==2 && dispatches==2);
	barriers=binds=sets=pushes=dispatches=0;
	for(unsigned d=3;d<=4;d++) {
        params.divisor=expected_divisor=d; expected_x=d==3?54:40; expected_y=d==3?30:23;
        assert(RCReceiver_RecordCompute(command,&targets,&p,layout,layout,set,set,set,set,&params));
        assert(dispatches==2); barriers=binds=sets=pushes=dispatches=0;
    }
    params.divisor=expected_divisor=1.5f;expected_x=107;expected_y=60;
    assert(RCReceiver_RecordCompute(command,&targets,&p,layout,layout,set,set,set,set,&params));
    assert(dispatches==2);barriers=binds=sets=pushes=dispatches=0;
    const float invalid[]={1,1.499f,1.501f,2.5f,5,NAN,INFINITY,-INFINITY};
    for(unsigned d=0;d<sizeof(invalid)/sizeof(invalid[0]);++d) {
        params.divisor=invalid[d];assert(!RCReceiver_RecordCompute(command,&targets,&p,layout,layout,set,set,set,set,&params));
    }
    params.divisor=expected_divisor=2; expected_x=80; expected_y=45;
    params.samples=4; assert(!RCReceiver_RecordCompute(command,&targets,&p,layout,layout,set,set,set,set,&params));
	params.samples=1; params.viewport[0]=12; assert(!RCReceiver_RecordCompute(command,&targets,&p,layout,layout,set,set,set,set,&params));
	params.viewport[0]=11; params.normal_cosine=NAN; assert(!RCReceiver_RecordCompute(command,&targets,&p,layout,layout,set,set,set,set,&params));
	assert(!barriers && !binds && !sets && !pushes && !dispatches);
	params.normal_cosine=.99f; targets.pass=(VkRenderPass)(uintptr_t)3; targets.framebuffer=(VkFramebuffer)(uintptr_t)4;
	p.raster=(VkPipeline)(uintptr_t)5; p.raster_pass=targets.pass;
	assert(!RCReceiver_RecordFrame(command,&targets,&p,layout,layout,set,set,set,set,&params,1.0f,draw_ready,draw,NULL));
	assert(!begins && !ends && !draws && !binds);
	ready=true; p.raster_pass=(VkRenderPass)(uintptr_t)6;
	assert(!RCReceiver_RecordFrame(command,&targets,&p,layout,layout,set,set,set,set,&params,1.0f,draw_ready,draw,NULL));
	assert(!begins && !ends && !draws && !binds);
	p.raster_pass=targets.pass;
	assert(RCReceiver_RecordFrame(command,&targets,&p,layout,layout,set,set,set,set,&params,1.0f,draw_ready,draw,NULL));
	assert(begins==1 && ends==1 && draws==1 && viewports==1 && scissors==1 && barriers==3 && binds==3 && sets==2 && pushes==2 && dispatches==2);
	assert(!timestamp_writes);
	barriers=binds=sets=pushes=dispatches=0;
	assert(RCReceiver_RecordFrameTimed(command,&targets,&p,layout,layout,set,set,set,set,&params,1.0f,draw_ready,draw,NULL,(VkQueryPool)(uintptr_t)1,12));
	assert(timestamp_writes==4);
	ready=false; assert(!RCReceiver_RecordFrameTimed(command,&targets,&p,layout,layout,set,set,set,set,&params,1.0f,draw_ready,draw,NULL,(VkQueryPool)(uintptr_t)1,12));
	assert(timestamp_writes==4);
	ready=true;expect_soft=true;barriers=binds=sets=pushes=dispatches=0;
	assert(RCReceiver_RecordFrameSoftTimed(command,&targets,&p,layout,layout,set,set,set,set,&params,1.0f,draw_ready,draw,NULL,VK_NULL_HANDLE,0,(VkPipeline)(uintptr_t)33));
	assert(barriers==4 && binds==4 && sets==3 && pushes==3 && dispatches==3 && timestamp_writes==4);
	puts("PASS: receiver prepass and compute ordering with pre-record fallback checks");
	return 0;
}
