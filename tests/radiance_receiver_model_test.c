#include "radiance_receiver_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned creates,fail_create,destroys,commands,draws,pushes,set5,set6,timestamps,allocated,freed;
static unsigned command_calls,fail_command;
static uint32_t last_token;
static VkPipeline recorded_pipeline[64],current_pipeline;
static uint32_t recorded_push_size[64],recorded_token[64];
static unsigned pipeline_records,push_records;
static bool recording_forward;
static bool creation_fail(void) {return ++creates==fail_create;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorSetLayout *out)
{(void)d;(void)a;assert(!i->bindingCount);if(creation_fail())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=H(VkDescriptorSetLayout,creates+100);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout l,const VkAllocationCallbacks *a)
{(void)d;(void)l;(void)a;++destroys;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkPipelineLayout *out)
{(void)d;(void)a;assert(i->setLayoutCount==7 && i->pSetLayouts[5]==H(VkDescriptorSetLayout,50) && i->pSetLayouts[6]==H(VkDescriptorSetLayout,60));assert(i->pushConstantRangeCount==1 && i->pPushConstantRanges[0].size==88 && i->pPushConstantRanges[0].stageFlags==VK_SHADER_STAGE_ALL_GRAPHICS);if(creation_fail())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=H(VkPipelineLayout,creates+200);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d,VkPipelineLayout l,const VkAllocationCallbacks *a)
{(void)d;(void)l;(void)a;++destroys;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkGraphicsPipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *out)
{(void)d;(void)c;(void)a;assert(n==1 && i->layout && ((i->renderPass==H(VkRenderPass,70) && !i->subpass && i->pColorBlendState->attachmentCount==2) || (i->renderPass==H(VkRenderPass,80) && i->subpass==2 && i->pColorBlendState->attachmentCount==1)));if(creation_fail())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=H(VkPipeline,creates+300);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a)
{(void)d;(void)p;(void)a;++destroys;}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *out)
{(void)d;assert(i->commandBufferCount==1 && i->level==VK_COMMAND_BUFFER_LEVEL_SECONDARY);if(++command_calls==fail_command)return VK_ERROR_OUT_OF_HOST_MEMORY;*out=H(VkCommandBuffer,900);++allocated;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer *c)
{(void)d;(void)p;assert(n==1 && c[0]);++freed;}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i)
{(void)c;assert(i->pInheritanceInfo->renderPass==H(VkRenderPass,80) && i->pInheritanceInfo->subpass==2 && i->pInheritanceInfo->framebuffer==H(VkFramebuffer,81));return ++command_calls==fail_command?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c)
{(void)c;return ++command_calls==fail_command?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p)
{(void)c;assert(b==VK_PIPELINE_BIND_POINT_GRAPHICS && p && pipeline_records<64);current_pipeline=p;recorded_pipeline[pipeline_records++]=p;++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t n,const VkBuffer *b,const VkDeviceSize *o)
{(void)c;assert(!first && (n==1 || n==3) && b && o);++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType type)
{(void)c;assert(b && o==2 && type==VK_INDEX_TYPE_UINT16);++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *s,uint32_t dc,const uint32_t *offset)
{(void)c;assert(b==VK_PIPELINE_BIND_POINT_GRAPHICS && l && s);if(!first){assert((n==3 || n==4) && dc==1 && offset && *offset==256);}else{assert(recording_forward && n==1 && !dc && !offset);if(first==5){assert(*s==H(VkDescriptorSet,50));++set5;}else{assert(first==6 && *s==H(VkDescriptorSet,60));++set6;}}++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags stage,uint32_t offset,uint32_t size,const void *data)
{(void)c;assert(l && stage==VK_SHADER_STAGE_ALL_GRAPHICS && !offset && (size==84 || size==80) && push_records<64);if(size==84){const unsigned char *bytes=data;memcpy(&last_token,bytes+80,4);assert(last_token);}else{assert(current_pipeline==H(VkPipeline,4) && l==H(VkPipelineLayout,5));last_token=0;}recorded_push_size[push_records]=size;recorded_token[push_records++]=last_token;++pushes;++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c,uint32_t indices,uint32_t instances,uint32_t first,int32_t vertex,uint32_t instance)
{(void)c;assert(indices==6 && instances==1 && first==3 && vertex==-2 && !instance);++draws;++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t n,const VkViewport *v)
{(void)c;assert(!first && n==1 && v->width==641 && v->height==361);++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t n,const VkRect2D *s)
{(void)c;assert(!first && n==1 && s->extent.width==641 && s->extent.height==361);++commands;}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool p,uint32_t query)
{(void)c;assert(stage==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT && p==H(VkQueryPool,99) && query==5);++timestamps;++commands;}

static rc_receiver_model_draw_t packet(unsigned kind)
{
	rc_receiver_model_draw_t d={.kind=kind,.token=100+kind,.index_buffer=H(VkBuffer,8),.index_offset=2,
		.index_count=6,.first_index=3,.vertex_offset=-2,.vertex_count=kind?1:3,.set_count=kind?4:3,
		.uniform_offset=256,.alpha=1,.reuse_eligible=true,.lighting_set=H(VkDescriptorSet,50),.fallback_pipeline=H(VkPipeline,4),.fallback_layout=H(VkPipelineLayout,5)};
	for(unsigned i=0;i<3;++i){d.vertices[i]=H(VkBuffer,10+i);d.vertex_offsets[i]=i*16;}
	for(unsigned i=0;i<4;++i)d.sets[i]=H(VkDescriptorSet,20+i);
	d.constants[0]=d.constants[5]=d.constants[10]=d.constants[15]=1;
	return d;
}
static rc_receiver_model_pipelines_t synthetic(void);
static void mixed_order(void)
{
	rc_receiver_model_draw_t stream[3]={packet(0),packet(0),packet(0)};
	for(unsigned i=0;i<3;++i)stream[i].token=200+i;
	stream[1].reuse_eligible=false;stream[1].alpha=.5f;
	assert(RCReceiverModel_DrawValid(&stream[1]));
	rc_receiver_model_snapshot_t snapshot={7,stream,3};rc_receiver_model_pipelines_t p=synthetic();
	assert(RCReceiverModel_EligibleCount(&snapshot)==2);
	uint64_t fingerprint=RCReceiverModel_Fingerprint(&snapshot);stream[1].reuse_eligible=true;
	assert(fingerprint!=RCReceiverModel_Fingerprint(&snapshot) && !RCReceiverModel_DrawValid(&stream[1]));stream[1].reuse_eligible=false;
	assert(RCReceiverModel_CanRecord(H(VkCommandBuffer,1),&p,&snapshot,7,false,0));
	unsigned before=draws,first_pipe=pipeline_records,first_push=push_records;
	recording_forward=false;RCReceiverModel_Record(H(VkCommandBuffer,1),&p,&snapshot,false,0);
	assert(draws==before+2 && pipeline_records==first_pipe+2 && recorded_token[first_push]==200 && recorded_token[first_push+1]==202);
	first_pipe=pipeline_records;first_push=push_records;before=draws;unsigned previous5=set5,previous6=set6;
	recording_forward=true;RCReceiverModel_Record(H(VkCommandBuffer,1),&p,&snapshot,true,H(VkDescriptorSet,60));
	assert(draws==before+3 && pipeline_records==first_pipe+3);
	assert(recorded_pipeline[first_pipe]==p.forward[0][0] && recorded_pipeline[first_pipe+1]==stream[1].fallback_pipeline && recorded_pipeline[first_pipe+2]==p.forward[0][0]);
	assert(recorded_push_size[first_push]==84 && recorded_push_size[first_push+1]==80 && recorded_push_size[first_push+2]==84);
	assert(set5==previous5+3 && set6==previous6+2);
	stream[1].shader_flags=2;assert(RCReceiverModel_DrawValid(&stream[1])); /* Debug native shaders keep their original UBO. */
}
static rc_receiver_model_pipelines_t synthetic(void)
{
	rc_receiver_model_pipelines_t p={.device=H(VkDevice,1),.layouts={H(VkPipelineLayout,1),H(VkPipelineLayout,2)},
		.receiver_pass=H(VkRenderPass,70),.forward_pass=H(VkRenderPass,80),.forward_subpass=2};
	for(unsigned k=0;k<3;++k)for(unsigned a=0;a<2;++a){p.prepass[k][a]=H(VkPipeline,10+k*2+a);p.forward[k][a]=H(VkPipeline,30+k*2+a);}
	return p;
}
static void capture_and_replay(void)
{
	rc_receiver_model_draw_t input[6],worker[2][3],combined[6];
	for(unsigned i=0;i<6;++i){input[i]=packet(i/2);input[i].alpha_test=i%2;}
	rc_receiver_model_capture_t a={.frame_serial=7,.draws=worker[0],.capacity=3,.first_token=10000};
	rc_receiver_model_capture_t b={.frame_serial=7,.draws=worker[1],.capacity=3,.first_token=11000};
	RCReceiverModelCapture_Reset();assert(RCReceiverModelCapture_Install(0,&a));assert(RCReceiverModelCapture_Install(1,&b));
	assert(RCReceiverModelCapture_Acquire(0,7)==&a && !RCReceiverModelCapture_Acquire(0,8));
	for(unsigned i=0;i<6;++i)assert(RCReceiverModelCapture_Add(i<3?&a:&b,&input[i]));
	rc_receiver_model_capture_t overlap=b;overlap.first_token=10002;assert(!RCReceiverModelCapture_Install(2,&overlap));
	rc_receiver_model_capture_t *parts[]={&a,&b};rc_receiver_model_snapshot_t snapshot={0};
	assert(RCReceiverModelCapture_Collect(&snapshot,7,combined,6,parts,2) && snapshot.count==6);
	assert(combined[0].token==10000 && combined[5].token==11002);
	uint64_t fingerprint=RCReceiverModel_Fingerprint(&snapshot);combined[4].uniform_offset++;assert(fingerprint!=RCReceiverModel_Fingerprint(&snapshot));combined[4].uniform_offset--;
	rc_receiver_model_pipelines_t p=synthetic();
	assert(RCReceiverModel_CanRecord(H(VkCommandBuffer,1),&p,&snapshot,7,false,0));
	unsigned old=commands;combined[2].constants[0]=NAN;assert(!RCReceiverModel_CanRecord(H(VkCommandBuffer,1),&p,&snapshot,7,false,0) && commands==old);combined[2].constants[0]=1;
	assert(!RCReceiverModel_CanRecord(H(VkCommandBuffer,1),&p,&snapshot,8,false,0));
	assert(!RCReceiverModel_CanRecord(H(VkCommandBuffer,1),&p,&snapshot,7,true,0));
	recording_forward=false;RCReceiverModel_Record(H(VkCommandBuffer,1),&p,&snapshot,false,0);assert(draws==6 && pushes==6 && !set5 && !set6 && last_token==11002);
	recording_forward=true;RCReceiverModel_Record(H(VkCommandBuffer,1),&p,&snapshot,true,H(VkDescriptorSet,60));assert(draws==12 && set5==6 && set6==6);
	const rc_receiver_model_draw_t *saved=snapshot.draws;assert(!RCReceiverModelCapture_Collect(&snapshot,7,combined,5,parts,2) && snapshot.draws==saved && snapshot.count==6);
	assert(!RCReceiverModelCapture_Collect(&snapshot,7,worker[0],6,parts,2));
	assert(!RCReceiverModelCapture_Add(&a,&input[0]) && a.failed && a.count==3);
	for(unsigned mutation=0;mutation<7;++mutation){rc_receiver_model_draw_t d=packet(0);switch(mutation){case 0:d.kind=3;break;case 1:d.vertex_count=1;break;case 2:d.sets[2]=0;break;case 3:d.alpha=.5f;break;case 4:d.shader_flags=8;break;case 5:d.shader_flags=2;break;case 6:d.index_count=5;break;}assert(!RCReceiverModel_DrawValid(&d));}
}
static void pipeline_failures(void)
{
	VkDescriptorSetLayout sets[2][5]={{H(VkDescriptorSetLayout,1),H(VkDescriptorSetLayout,2),H(VkDescriptorSetLayout,3),H(VkDescriptorSetLayout,4),0},
		{H(VkDescriptorSetLayout,1),H(VkDescriptorSetLayout,2),H(VkDescriptorSetLayout,3),H(VkDescriptorSetLayout,8),H(VkDescriptorSetLayout,4)}};
	VkPipelineShaderStageCreateInfo stages[2]={{.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=H(VkShaderModule,1)},{.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=H(VkShaderModule,2)}};
	VkPipelineMultisampleStateCreateInfo ms={.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
	VkPipelineDepthStencilStateCreateInfo depth={.depthTestEnable=1,.depthWriteEnable=1};VkPipelineRasterizationStateCreateInfo raster={0};
	VkDynamicState states[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic={.dynamicStateCount=2,.pDynamicStates=states};
	VkPipelineColorBlendAttachmentState blend[2]={0};VkPipelineColorBlendStateCreateInfo colors[2]={{.attachmentCount=2,.pAttachments=blend},{.attachmentCount=1,.pAttachments=blend}};
	VkGraphicsPipelineCreateInfo pre[3][2],forward[3][2];
	for(unsigned k=0;k<3;++k)for(unsigned a=0;a<2;++a){pre[k][a]=(VkGraphicsPipelineCreateInfo){.stageCount=2,.pStages=stages,.pMultisampleState=&ms,.pDepthStencilState=&depth,.pRasterizationState=&raster,.pDynamicState=&dynamic,.pColorBlendState=&colors[0]};forward[k][a]=pre[k][a];forward[k][a].pColorBlendState=&colors[1];}
	rc_receiver_model_pipelines_t out={0};
	for(unsigned failure=1;failure<=15;++failure){creates=destroys=0;fail_create=failure;assert(!RCReceiverModelPipelines_Create(&out,H(VkDevice,1),0,sets,H(VkDescriptorSetLayout,50),H(VkDescriptorSetLayout,60),H(VkRenderPass,70),H(VkRenderPass,80),2,pre,forward));assert(!out.device && destroys==failure-1);}
	creates=destroys=fail_create=0;assert(RCReceiverModelPipelines_Create(&out,H(VkDevice,1),0,sets,H(VkDescriptorSetLayout,50),H(VkDescriptorSetLayout,60),H(VkRenderPass,70),H(VkRenderPass,80),2,pre,forward));assert(creates==15 && out.forward[2][1]);
	VkPipeline saved=out.forward[2][1];creates=0;fail_create=10;assert(!RCReceiverModelPipelines_Create(&out,H(VkDevice,1),0,sets,H(VkDescriptorSetLayout,50),H(VkDescriptorSetLayout,60),H(VkRenderPass,70),H(VkRenderPass,80),2,pre,forward) && out.forward[2][1]==saved);
	creates=0;raster.depthBiasEnable=1;assert(!RCReceiverModelPipelines_Create(&out,H(VkDevice,1),0,sets,H(VkDescriptorSetLayout,50),H(VkDescriptorSetLayout,60),H(VkRenderPass,70),H(VkRenderPass,80),2,pre,forward) && !creates);
	RCReceiverModelPipelines_Destroy(&out);assert(!out.device);
}
static void alternate_failures(void)
{
	rc_receiver_model_draw_t draw=packet(1);rc_receiver_model_snapshot_t snapshot={7,&draw,1};rc_receiver_model_pipelines_t p=synthetic();
	VkViewport view={0,0,641,361,0,1};VkRect2D scissor={{0,0},{641,361}};rc_receiver_model_alternates_t out={0};
	for(unsigned fail=1;fail<=3;++fail){command_calls=0;fail_command=fail;assert(!RCReceiverModelAlternates_Create(&out,H(VkDevice,1),H(VkCommandPool,1),7,H(VkRenderPass,80),H(VkFramebuffer,81),2,&p,&snapshot,H(VkDescriptorSet,60),H(VkDescriptorSet,50),&view,&scissor,0,0) && !out.device);}
	command_calls=fail_command=timestamps=0;assert(RCReceiverModelAlternates_Create(&out,H(VkDevice,1),H(VkCommandPool,1),7,H(VkRenderPass,80),H(VkFramebuffer,81),2,&p,&snapshot,H(VkDescriptorSet,60),H(VkDescriptorSet,50),&view,&scissor,H(VkQueryPool,99),5));assert(out.serial==7 && out.count==1 && timestamps==1 && out.fingerprint==RCReceiverModel_Fingerprint(&snapshot));
	VkCommandBuffer saved=out.buffers[0];snapshot.frame_serial=8;assert(!RCReceiverModelAlternates_Create(&out,H(VkDevice,1),H(VkCommandPool,1),7,H(VkRenderPass,80),H(VkFramebuffer,81),2,&p,&snapshot,H(VkDescriptorSet,60),H(VkDescriptorSet,50),&view,&scissor,0,0) && out.buffers[0]==saved);
	RCReceiverModelAlternates_Destroy(&out);assert(allocated==freed);
}
int main(void){capture_and_replay();mixed_order();pipeline_failures();alternate_failures();puts("PASS: model receiver mixed ordering, native/receiver ABI parity,15 pipeline failures and alternate rollback");return 0;}
