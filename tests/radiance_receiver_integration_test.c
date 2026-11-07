#include "radiance_receiver_light.h"
#include "radiance_receiver_forward.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
bool RCReceiverModel_CanRecord(VkCommandBuffer c,const rc_receiver_model_pipelines_t *p,const rc_receiver_model_snapshot_t *s,uint64_t serial,bool f,VkDescriptorSet r)
{ (void)c;(void)p;(void)s;(void)serial;(void)f;(void)r;assert(!"unexpected model in world-only fixture");return false; }
void RCReceiverModel_Record(VkCommandBuffer c,const rc_receiver_model_pipelines_t *p,const rc_receiver_model_snapshot_t *s,bool f,VkDescriptorSet r)
{ (void)c;(void)p;(void)s;(void)f;(void)r;assert(!"unexpected model in world-only fixture"); }
uint64_t RCReceiverModel_Fingerprint(const rc_receiver_model_snapshot_t *s) { (void)s;assert(!"unexpected model in world-only fixture");return 0; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t count,const VkViewport *v)
{ (void)c;(void)first;(void)count;(void)v; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t count,const VkRect2D *s)
{ (void)c;(void)first;(void)count;(void)s; }
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned order, prepass, compute, forward, pre_token, forward_token;
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *i,VkSubpassContents contents)
{ (void)c;(void)i;(void)contents; assert(order++==0); }
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c) { (void)c; assert(prepass==1); }
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer c,uint32_t vertices,uint32_t instances,uint32_t first,uint32_t base) { (void)c;(void)vertices;(void)instances;(void)first;(void)base; assert(0); }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags a,VkPipelineStageFlags b,VkDependencyFlags d,uint32_t x,const VkMemoryBarrier *m,uint32_t y,const VkBufferMemoryBarrier *bb,uint32_t n,const VkImageMemoryBarrier *im)
{ (void)c;(void)a;(void)b;(void)d;(void)x;(void)m;(void)y;(void)bb;(void)n;(void)im; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p)
{ (void)c;(void)b;(void)p; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t f,uint32_t n,const VkDescriptorSet *s,uint32_t x,const uint32_t *o)
{ (void)c;(void)b;(void)l;(void)f;(void)n;(void)s;(void)x;(void)o; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags stage,uint32_t off,uint32_t size,const void *data)
{ (void)c;(void)l;(void)off; if (stage==VK_SHADER_STAGE_ALL_GRAPHICS && size==88) memcpy(&pre_token,(const char*)data+64,4); if (stage==VK_SHADER_STAGE_ALL_GRAPHICS && size==92) memcpy(&forward_token,(const char*)data+88,4); }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z)
{ (void)c;(void)x;(void)y;(void)z; ++compute; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer c,float a,float b,float d)
{ (void)c;(void)a;(void)b;(void)d; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,const VkDeviceSize *o)
{ (void)c;(void)f;(void)n;(void)b;(void)o; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t)
{ (void)c;(void)b;(void)o;(void)t; }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s) { (void)c;(void)b;(void)o;(void)n;(void)s; assert(0); }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c,uint32_t n,uint32_t instances,uint32_t first,int32_t vertex,uint32_t start)
{ (void)c;(void)n;(void)instances;(void)first;(void)vertex;(void)start; if (!compute) ++prepass; else ++forward; }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t query)
{ (void)c;(void)stage;(void)pool;(void)query;assert(!"profiling must be disabled in this fixture"); }
void RCReceiverLight_Record(VkCommandBuffer c,const rc_receiver_light_t *l,const rc_receiver_params_t *p,bool post,VkDescriptorSet s) { (void)c;(void)l;(void)p;(void)post;(void)s; assert(0); }
int main(void)
{
	VkCommandBuffer cb=H(VkCommandBuffer,1); VkDevice dev=H(VkDevice,1); VkPipelineLayout layout=H(VkPipelineLayout,1); VkDescriptorSet set=H(VkDescriptorSet,1);
	VkPipeline world_pipes[2]={H(VkPipeline,1),H(VkPipeline,2)}, forward_pipes[4]={H(VkPipeline,1),H(VkPipeline,2),H(VkPipeline,3),H(VkPipeline,4)};
	rc_receiver_world_draw_t input={.vertex_buffer=H(VkBuffer,1),.index_buffer=H(VkBuffer,2),.index_type=VK_INDEX_TYPE_UINT32,.index_count=6,.diffuse_set=set,.instance_set=set,.camera_set=set}, storage;
	rc_receiver_world_snapshot_t snapshot={0}; assert(RCReceiverSnapshot_Build(&snapshot,3,&storage,1,&input,1,9,cb,layout,world_pipes));
	rc_receiver_targets_t t={.device=dev,.width=640,.height=480,.pass=H(VkRenderPass,1),.framebuffer=H(VkFramebuffer,1)};
	for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i) { t.images[i].image=H(VkImage,i+1); t.images[i].view=H(VkImageView,i+1); }
	rc_receiver_pipelines_t p={.device=dev,.raster_pass=t.pass,.raster=world_pipes[0],.classify=H(VkPipeline,1),.direct=H(VkPipeline,2)};
	rc_receiver_frame_plan_t plan={.frame_serial=3,.enabled=true,.edge_fallback_enabled=true,.targets=&t,.pipelines=&p,.classify_layout=layout,.direct_layout=layout,.world_layout=layout,.classify_set=set,.direct_set=set,.cascade_set=set,.lighting_set=set,.world_pipelines={world_pipes[0],world_pipes[1]},.params={.viewport={0,0,640,480},.samples=1,.divisor=2,.normal_cosine=1},.depth_clear=1};
	rc_receiver_forward_material_t m={.lightmap_set=set,.fullbright_set=set,.reuse_set=set,.alpha=1};
	assert(RCReceiverForward_CanRecord(cb,layout,forward_pipes,&snapshot,&m,3));
	plan.forward_materials=&m; plan.forward_fingerprint=RCReceiverForward_Fingerprint(RC_RECEIVER_FORWARD_HASH_SEED,&snapshot,&m);
	assert(RCReceiverSnapshot_Stage(&snapshot,&plan)); ++storage.index_count;
	assert(!RCReceiverFrame_Record(cb,3) && !prepass && !compute); --storage.index_count;
	assert(RCReceiverSnapshot_Stage(&snapshot,&plan)); assert(RCReceiverFrame_Record(cb,3));
	assert(RCReceiverFrame_Active(3) && prepass==1 && compute==2 && pre_token==9);
	RCReceiverForward_Record(cb,layout,forward_pipes,&snapshot,&m); assert(forward==1 && forward_token==pre_token);
	RCReceiverFrame_Reset(); assert(!RCReceiverFrame_Active(3));
	puts("PASS: staged world snapshot produces matching prepass and forward identity"); return 0;
}
