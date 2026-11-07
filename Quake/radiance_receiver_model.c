#include "radiance_receiver_model.h"
#include "radiance_receiver_targets.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct { float constants[20]; uint32_t token; } model_push_t;
_Static_assert(offsetof(model_push_t,token)==80 && sizeof(model_push_t)==84,"model receiver push ABI");
static rc_receiver_model_capture_t *captures[RC_RECEIVER_MODEL_CAPTURE_SLOTS];
void RCReceiverModelCapture_Reset(void) { memset(captures,0,sizeof(captures)); }
bool RCReceiverModelCapture_Install(uint32_t slot,rc_receiver_model_capture_t *capture)
{
	if(slot>=RC_RECEIVER_MODEL_CAPTURE_SLOTS)return false;
	captures[slot]=NULL;
	if(!capture || !capture->draws || !capture->capacity || !capture->first_token ||
		capture->first_token>UINT32_MAX-(capture->capacity-1))return false;
	for(unsigned i=0;i<RC_RECEIVER_MODEL_CAPTURE_SLOTS;++i) {
		const rc_receiver_model_capture_t *other=captures[i];
		if(!other || other->frame_serial!=capture->frame_serial)continue;
		uint64_t a=capture->first_token,b=other->first_token;
		if(other==capture || (a<b+other->capacity && b<a+capture->capacity))return false;
	}
	capture->count=0;capture->failed=false;captures[slot]=capture;return true;
}
rc_receiver_model_capture_t *RCReceiverModelCapture_Acquire(uint32_t slot,uint64_t serial)
{ return slot<RC_RECEIVER_MODEL_CAPTURE_SLOTS && captures[slot] && captures[slot]->frame_serial==serial?captures[slot]:NULL; }
bool RCReceiverModel_DrawValid(const rc_receiver_model_draw_t *d)
{
	if(!d || d->kind>=RC_RECEIVER_MODEL_KINDS || !d->token || !d->index_buffer || !d->index_count || d->index_count%3 ||
		d->first_index>UINT32_MAX-d->index_count || d->index_offset%2 ||
		d->vertex_count!=(d->kind?1u:3u) || d->set_count!=(d->kind?4u:3u) ||
		!d->fallback_pipeline || !d->fallback_layout || !d->lighting_set ||
		!isfinite(d->alpha) || d->alpha<0 || d->alpha>1 ||
		(d->reuse_eligible && (d->alpha!=1.f || (d->shader_flags&(2u|8u)))))return false;
	for(unsigned i=0;i<d->vertex_count;++i)if(!d->vertices[i] || d->vertex_offsets[i]%4)return false;
	for(unsigned i=0;i<d->set_count;++i)if(!d->sets[i])return false;
	for(unsigned i=0;i<20;++i)if(!isfinite(d->constants[i]))return false;
	return true;
}
bool RCReceiverModelCapture_Add(rc_receiver_model_capture_t *c,const rc_receiver_model_draw_t *input)
{
	if(!c)return false;
	if(c->failed || !input || !c->draws || c->count>=c->capacity || !c->first_token || c->first_token>UINT32_MAX-c->count)
		{c->failed=true;return false;}
	rc_receiver_model_draw_t draw=*input;draw.token=c->first_token+c->count;
	if(!RCReceiverModel_DrawValid(&draw)){c->failed=true;return false;}
	c->draws[c->count++]=draw;return true;
}
static bool disjoint(const void *a,size_t as,const void *b,size_t bs)
{
	uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
	return x<=UINTPTR_MAX-as && y<=UINTPTR_MAX-bs && !(x<y+bs && y<x+as);
}
bool RCReceiverModelCapture_Collect(rc_receiver_model_snapshot_t *snapshot,uint64_t serial,
	rc_receiver_model_draw_t *storage,uint32_t capacity,rc_receiver_model_capture_t *const *parts,uint32_t count)
{
	if(!snapshot || !storage || !capacity || !parts || !count || count>RC_RECEIVER_MODEL_CAPTURE_SLOTS)return false;
	uint64_t total=0;uint32_t last=0;
	for(unsigned i=0;i<count;++i) {
		const rc_receiver_model_capture_t *p=parts[i];
		if(!p || p->failed || p->frame_serial!=serial || !p->draws || p->count>p->capacity)return false;
		for(unsigned j=0;j<p->count;++j) {
			if(!RCReceiverModel_DrawValid(&p->draws[j]) || p->draws[j].token<=last)return false;
			last=p->draws[j].token;
		}
		total+=p->count;if(total>capacity || total>SIZE_MAX/sizeof(*storage))return false;
	}
	size_t bytes=(size_t)total*sizeof(*storage);
	for(unsigned i=0;i<count;++i)if(parts[i]->count && !disjoint(storage,bytes,parts[i]->draws,(size_t)parts[i]->count*sizeof(*storage)))return false;
	uint32_t at=0;
	for(unsigned i=0;i<count;++i){memcpy(storage+at,parts[i]->draws,(size_t)parts[i]->count*sizeof(*storage));at+=parts[i]->count;}
	*snapshot=(rc_receiver_model_snapshot_t){serial,storage,at};return true;
}
static uint64_t hash_bytes(uint64_t h,const void *value,size_t bytes)
{const unsigned char *p=value;while(bytes--){h^=*p++;h*=UINT64_C(1099511628211);}return h;}
uint64_t RCReceiverModel_Fingerprint(const rc_receiver_model_snapshot_t *snapshot)
{
	uint64_t hash=UINT64_C(14695981039346656037);
	if(!snapshot || (snapshot->count && !snapshot->draws))return 0;
	for(unsigned i=0;i<snapshot->count;++i) {
		const rc_receiver_model_draw_t *d=&snapshot->draws[i];
#define HASH(v) hash=hash_bytes(hash,&(v),sizeof(v))
		HASH(d->vertices);HASH(d->vertex_offsets);HASH(d->index_buffer);HASH(d->index_offset);HASH(d->sets);
		HASH(d->lighting_set);HASH(d->fallback_pipeline);HASH(d->fallback_layout);HASH(d->constants);HASH(d->alpha);
		HASH(d->kind);HASH(d->token);HASH(d->vertex_count);HASH(d->set_count);HASH(d->uniform_offset);HASH(d->shader_flags);
		HASH(d->index_count);HASH(d->first_index);HASH(d->vertex_offset);
		unsigned char alpha=d->alpha_test;HASH(alpha);
		unsigned char eligible=d->reuse_eligible;HASH(eligible);
#undef HASH
	}
	return hash;
}
bool RCReceiverModel_CanRecord(VkCommandBuffer command,const rc_receiver_model_pipelines_t *p,
	const rc_receiver_model_snapshot_t *snapshot,uint64_t serial,bool forward,VkDescriptorSet reuse)
{
	if(!command || !p || !p->device || !snapshot || snapshot->frame_serial!=serial ||
		(snapshot->count && !snapshot->draws) || (forward && !reuse))return false;
	uint32_t last=0;
	for(unsigned i=0;i<snapshot->count;++i) {
		const rc_receiver_model_draw_t *d=&snapshot->draws[i];
		if(!RCReceiverModel_DrawValid(d) || d->token<=last)return false;
		if(d->reuse_eligible && (!p->layouts[d->kind?1:0] ||
			!(forward?p->forward[d->kind][d->alpha_test]:p->prepass[d->kind][d->alpha_test])))return false;
		last=d->token;
	}
	return true;
}
void RCReceiverModel_Record(VkCommandBuffer command,const rc_receiver_model_pipelines_t *p,
	const rc_receiver_model_snapshot_t *snapshot,bool forward,VkDescriptorSet reuse)
{
	for(unsigned i=0;i<snapshot->count;++i) {
		const rc_receiver_model_draw_t *d=&snapshot->draws[i];
		if(!forward && !d->reuse_eligible)continue;
		VkPipelineLayout layout=d->reuse_eligible?p->layouts[d->kind?1:0]:d->fallback_layout;
		model_push_t push;memcpy(push.constants,d->constants,sizeof(push.constants));push.token=d->token;
		VkPipeline pipeline=d->reuse_eligible?(forward?p->forward[d->kind][d->alpha_test]:p->prepass[d->kind][d->alpha_test]):d->fallback_pipeline;
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
		vkCmdBindVertexBuffers(command,0,d->vertex_count,d->vertices,d->vertex_offsets);
		vkCmdBindIndexBuffer(command,d->index_buffer,d->index_offset,VK_INDEX_TYPE_UINT16);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,d->set_count,d->sets,1,&d->uniform_offset);
		if(forward) {
			vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,5,1,&d->lighting_set,0,NULL);
			if(d->reuse_eligible)vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,6,1,&reuse,0,NULL);
		}
		vkCmdPushConstants(command,layout,VK_SHADER_STAGE_ALL_GRAPHICS,0,d->reuse_eligible?sizeof(push):sizeof(d->constants),&push);
		vkCmdDrawIndexed(command,d->index_count,1,d->first_index,d->vertex_offset,0);
	}
}

void RCReceiverModelPipelines_Destroy(rc_receiver_model_pipelines_t *p)
{
	if(!p || !p->device)return;
	for(unsigned kind=0;kind<3;++kind)for(unsigned cutout=0;cutout<2;++cutout) {
		if(p->prepass[kind][cutout])vkDestroyPipeline(p->device,p->prepass[kind][cutout],NULL);
		if(p->forward[kind][cutout])vkDestroyPipeline(p->device,p->forward[kind][cutout],NULL);
	}
	for(unsigned i=0;i<2;++i)if(p->layouts[i])vkDestroyPipelineLayout(p->device,p->layouts[i],NULL);
	if(p->empty_layout)vkDestroyDescriptorSetLayout(p->device,p->empty_layout,NULL);
	memset(p,0,sizeof(*p));
}
/* The prepass follows the receiver targets; forward follows the scene. */
static bool graphics_state(const VkGraphicsPipelineCreateInfo *info,uint32_t colors)
{
	if(!info || info->stageCount!=2 || !info->pStages || !info->pMultisampleState ||
		(colors==2 && !RCReceiverTargets_ValidSamples(info->pMultisampleState->rasterizationSamples)) || !info->pDepthStencilState ||
		!info->pDepthStencilState->depthTestEnable || !info->pDepthStencilState->depthWriteEnable ||
		!info->pColorBlendState || info->pColorBlendState->attachmentCount!=colors || !info->pColorBlendState->pAttachments ||
		!info->pDynamicState || !info->pDynamicState->pDynamicStates || !info->pRasterizationState || info->pRasterizationState->depthBiasEnable)return false;
	bool viewport=false,scissor=false;
	for(unsigned i=0;i<info->pDynamicState->dynamicStateCount;++i) {
		viewport|=info->pDynamicState->pDynamicStates[i]==VK_DYNAMIC_STATE_VIEWPORT;
		scissor|=info->pDynamicState->pDynamicStates[i]==VK_DYNAMIC_STATE_SCISSOR;
	}
	for(unsigned i=0;i<colors;++i)if(info->pColorBlendState->pAttachments[i].blendEnable)return false;
	return viewport && scissor && info->pStages[0].module && info->pStages[1].module &&
		info->pStages[0].stage==VK_SHADER_STAGE_VERTEX_BIT && info->pStages[1].stage==VK_SHADER_STAGE_FRAGMENT_BIT;
}
bool RCReceiverModelPipelines_Create(rc_receiver_model_pipelines_t *out,VkDevice device,VkPipelineCache cache,
	const VkDescriptorSetLayout model[2][5],VkDescriptorSetLayout lighting,VkDescriptorSetLayout receiver,
	VkRenderPass prepass,VkRenderPass forward,uint32_t subpass,
	const VkGraphicsPipelineCreateInfo prepass_info[3][2],const VkGraphicsPipelineCreateInfo forward_info[3][2])
{
	if(!out || !device || !model || !lighting || !receiver || !prepass || !forward || !prepass_info || !forward_info)return false;
	for(unsigned kind=0;kind<3;++kind)for(unsigned cutout=0;cutout<2;++cutout)
		if(!graphics_state(&prepass_info[kind][cutout],2) || !graphics_state(&forward_info[kind][cutout],1))return false;
	for(unsigned kind=0;kind<2;++kind)for(unsigned set=0;set<5;++set)
		if(!model[kind][set] && !(kind==0 && set==4))return false;
	rc_receiver_model_pipelines_t p={.device=device,.receiver_pass=prepass,.forward_pass=forward,.forward_subpass=subpass};
	if(!model[0][4]) {
		VkDescriptorSetLayoutCreateInfo empty={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		if(vkCreateDescriptorSetLayout(device,&empty,NULL,&p.empty_layout)!=VK_SUCCESS || !p.empty_layout)goto failed;
	}
	for(unsigned kind=0;kind<2;++kind) {
		VkDescriptorSetLayout sets[7];memcpy(sets,model[kind],5*sizeof(*sets));
		if(kind==0 && !sets[4])sets[4]=p.empty_layout;
		sets[5]=lighting;sets[6]=receiver;
		VkPushConstantRange range={VK_SHADER_STAGE_ALL_GRAPHICS,0,88};
		VkPipelineLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount=7,.pSetLayouts=sets,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
		if(vkCreatePipelineLayout(device,&info,NULL,&p.layouts[kind])!=VK_SUCCESS || !p.layouts[kind])goto failed;
	}
	for(unsigned kind=0;kind<3;++kind)for(unsigned cutout=0;cutout<2;++cutout) {
		VkGraphicsPipelineCreateInfo info=prepass_info[kind][cutout];
		info.layout=p.layouts[kind?1:0];info.renderPass=prepass;info.subpass=0;
		if(vkCreateGraphicsPipelines(device,cache,1,&info,NULL,&p.prepass[kind][cutout])!=VK_SUCCESS || !p.prepass[kind][cutout])goto failed;
		info=forward_info[kind][cutout];info.layout=p.layouts[kind?1:0];info.renderPass=forward;info.subpass=subpass;
		if(vkCreateGraphicsPipelines(device,cache,1,&info,NULL,&p.forward[kind][cutout])!=VK_SUCCESS || !p.forward[kind][cutout])goto failed;
	}
	RCReceiverModelPipelines_Destroy(out);*out=p;return true;
failed:
	RCReceiverModelPipelines_Destroy(&p);return false;
}
void RCReceiverModelAlternates_Destroy(rc_receiver_model_alternates_t *a)
{
	if(!a || !a->device)return;
	if(a->pool && a->count)vkFreeCommandBuffers(a->device,a->pool,a->count,a->buffers);
	memset(a,0,sizeof(*a));
}
bool RCReceiverModelAlternates_Create(rc_receiver_model_alternates_t *out,VkDevice device,VkCommandPool pool,
	uint64_t serial,VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass,
	const rc_receiver_model_pipelines_t *p,const rc_receiver_model_snapshot_t *snapshot,
	VkDescriptorSet reuse,VkDescriptorSet lighting,const VkViewport *viewport,const VkRect2D *scissor,
	VkQueryPool timestamps,uint32_t end_query)
{
	if(!out || !device || !pool || !pass || !framebuffer || !p || p->device!=device || p->forward_pass!=pass ||
		p->forward_subpass!=subpass || !snapshot || !snapshot->count || !snapshot->draws || snapshot->frame_serial!=serial ||
		!reuse || !lighting || !viewport || !scissor || !isfinite(viewport->x) || !isfinite(viewport->y) ||
		!isfinite(viewport->width) || !isfinite(viewport->height) || viewport->width<=0 || viewport->height<=0 ||
		!isfinite(viewport->minDepth) || !isfinite(viewport->maxDepth) || viewport->minDepth!=0 || viewport->maxDepth!=1 ||
		!scissor->extent.width || !scissor->extent.height)return false;
	for(unsigned i=0;i<snapshot->count;++i)if(snapshot->draws[i].lighting_set!=lighting)return false;
	rc_receiver_model_alternates_t a={.device=device,.pool=pool,.serial=serial,.fingerprint=RCReceiverModel_Fingerprint(snapshot),
		.pass=pass,.framebuffer=framebuffer,.subpass=subpass};
	VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY,.commandBufferCount=1};
	if(vkAllocateCommandBuffers(device,&allocate,a.buffers)!=VK_SUCCESS)goto failed;
	a.count=1;
	if(!RCReceiverModel_CanRecord(a.buffers[0],p,snapshot,serial,true,reuse))goto failed;
	VkCommandBufferInheritanceInfo inheritance={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,.renderPass=pass,.framebuffer=framebuffer,.subpass=subpass};
	VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,.pInheritanceInfo=&inheritance};
	if(vkBeginCommandBuffer(a.buffers[0],&begin)!=VK_SUCCESS)goto failed;
	vkCmdSetViewport(a.buffers[0],0,1,viewport);vkCmdSetScissor(a.buffers[0],0,1,scissor);
	RCReceiverModel_Record(a.buffers[0],p,snapshot,true,reuse);
	if(timestamps)vkCmdWriteTimestamp(a.buffers[0],VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,end_query);
	if(vkEndCommandBuffer(a.buffers[0])!=VK_SUCCESS)goto failed;
	RCReceiverModelAlternates_Destroy(out);*out=a;return true;
failed:
	RCReceiverModelAlternates_Destroy(&a);return false;
}
