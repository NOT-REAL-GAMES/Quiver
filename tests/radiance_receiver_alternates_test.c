#include "radiance_receiver_alternates.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned calls,fail,freed,draws,viewports,scissors,timestamps; static bool allow=true;
bool RCReceiverForward_CanRecord(VkCommandBuffer c,VkPipelineLayout l,const VkPipeline p[4],const rc_receiver_world_snapshot_t *s,const rc_receiver_forward_material_t *m,uint64_t serial)
{ (void)c;(void)l;(void)p;(void)m; return allow && s->frame_serial==serial; }
uint64_t RCReceiverForward_Fingerprint(uint64_t h,const rc_receiver_world_snapshot_t *s,const rc_receiver_forward_material_t *m)
{ (void)m; return h+s->draws[0].token; }
void RCReceiverForward_Record(VkCommandBuffer c,VkPipelineLayout l,const VkPipeline p[4],const rc_receiver_world_snapshot_t *s,const rc_receiver_forward_material_t *m)
{ (void)c;(void)l;(void)p;(void)s;(void)m;++draws; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo *a,VkCommandBuffer *out)
{ (void)d; if(++calls==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; assert(a->level==VK_COMMAND_BUFFER_LEVEL_SECONDARY && a->commandBufferCount==2); for(unsigned i=0;i<2;++i)out[i]=H(VkCommandBuffer,i+1);return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer *b)
{ (void)d;(void)p;(void)b; assert(n==2);++freed; }
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i)
{ (void)c; if(++calls==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; assert(i->flags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT); assert(i->pInheritanceInfo->renderPass==H(VkRenderPass,1) && i->pInheritanceInfo->subpass==2);return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c)
{ (void)c;if(++calls==fail)return VK_ERROR_OUT_OF_HOST_MEMORY;return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t count,const VkViewport *v)
{ (void)c;assert(first==0 && count==1 && v->width==640);++viewports; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t count,const VkRect2D *r)
{ (void)c;assert(first==0 && count==1 && r->extent.width==640);++scissors; }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t query)
{
	assert(stage==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT && pool==H(VkQueryPool,9));
	if (!timestamps) assert(c==H(VkCommandBuffer,1) && query==8 && draws==0);
	else assert(timestamps==1 && c==H(VkCommandBuffer,2) && query==9 && draws==2);
	++timestamps;
}
int main(void)
{
	rc_receiver_world_draw_t draws0={.token=9},draws1={.token=10};
	rc_receiver_world_snapshot_t snap[2]={{.frame_serial=7,.draws=&draws0,.count=1},{.frame_serial=7,.draws=&draws1,.count=1}};
	rc_receiver_forward_material_t mat[2]={0}; rc_receiver_alternate_part_t parts[2]={{&snap[0],&mat[0]},{&snap[1],&mat[1]}};
	VkViewport view={.width=640,.height=480,.maxDepth=1}; VkRect2D scissor={.extent={640,480}};
	VkPipeline p[4]={0}; rc_receiver_alternates_t out={0};
	for(unsigned i=1;i<=5;++i) {calls=freed=draws=0;fail=i; assert(!RCReceiverAlternates_Create(&out,H(VkDevice,1),H(VkCommandPool,1),7,H(VkRenderPass,1),H(VkFramebuffer,1),2,H(VkPipelineLayout,1),p,&view,&scissor,parts,2,VK_NULL_HANDLE,0)); assert(!out.device && freed==(i>1));}
	calls=freed=draws=fail=timestamps=0;assert(RCReceiverAlternates_Create(&out,H(VkDevice,1),H(VkCommandPool,1),7,H(VkRenderPass,1),H(VkFramebuffer,1),2,H(VkPipelineLayout,1),p,&view,&scissor,parts,2,H(VkQueryPool,9),8));assert(out.count==2 && draws==2 && timestamps==2 && !freed);
	calls=0;fail=3;assert(!RCReceiverAlternates_Create(&out,H(VkDevice,1),H(VkCommandPool,1),7,H(VkRenderPass,1),H(VkFramebuffer,1),2,H(VkPipelineLayout,1),p,&view,&scissor,parts,2,VK_NULL_HANDLE,0));assert(out.count==2 && freed==1);
	draws1.token=9;calls=0;fail=0;assert(!RCReceiverAlternates_Create(&out,H(VkDevice,1),H(VkCommandPool,1),7,H(VkRenderPass,1),H(VkFramebuffer,1),2,H(VkPipelineLayout,1),p,&view,&scissor,parts,2,VK_NULL_HANDLE,0));assert(!calls && out.count==2);
	RCReceiverAlternates_Destroy(&out);assert(!out.count && freed==2);puts("PASS: alternate world secondaries record with inherited pass and transactional fallback");return 0;
}
