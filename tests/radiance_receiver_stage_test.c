#include "radiance_receiver_stage.h"
#include "radiance_receiver_select.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned resets, clears, select_calls, stages;
static rc_receiver_frame_plan_t last_plan;
bool RCReceiverModel_CanRecord(VkCommandBuffer c,const rc_receiver_model_pipelines_t *p,const rc_receiver_model_snapshot_t *s,uint64_t serial,bool f,VkDescriptorSet r)
{ (void)c;assert(p && s->count==1 && s->frame_serial==serial && (!f || r));return true; }
uint64_t RCReceiverModel_Fingerprint(const rc_receiver_model_snapshot_t *s) { assert(s->count==1 && s->draws);return 44; }
void RCReceiverFrame_Stage(const rc_receiver_frame_plan_t *p) { assert(!p); ++clears; }
void RCReceiverSelect_Reset(void) { ++resets; }
bool RCReceiverSelect_Stage(uint64_t serial,const VkCommandBuffer *a,uint32_t n,bool complete,VkRenderPass pass,VkFramebuffer fb,uint32_t subpass)
{ (void)serial;(void)a;(void)pass;(void)fb;(void)subpass; ++select_calls; return n==1 && complete; }
bool RCReceiver_CanRecordCompute(VkCommandBuffer command,const rc_receiver_targets_t *t,const rc_receiver_pipelines_t *p,VkPipelineLayout a,VkPipelineLayout b,VkDescriptorSet c,VkDescriptorSet d,VkDescriptorSet e,VkDescriptorSet f,const rc_receiver_params_t *params)
{ (void)command;(void)t;(void)p;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)params; return true; }
bool RCReceiverWorld_CanRecord(VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline p[2],const rc_receiver_world_draw_t *draws,uint32_t count)
{ (void)command;(void)layout;(void)p;(void)draws; return count<=1; }
bool RCReceiverForward_CanRecord(VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline p[4],const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *m,uint64_t serial)
{ (void)command;(void)layout;(void)p;(void)m; return snapshot->frame_serial==serial; }
uint64_t RCReceiverForward_Fingerprint(uint64_t h,const rc_receiver_world_snapshot_t *s,const rc_receiver_forward_material_t *m)
{ (void)h;(void)s;(void)m;return 42; }
bool RCReceiverSnapshot_Stage(const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_frame_plan_t *plan)
{ ++stages; assert(snapshot->frame_serial==plan->frame_serial && plan->edge_fallback_enabled); last_plan=*plan; return true; }
int main(void)
{
	rc_receiver_resources_t r={.device=H(VkDevice,1),.targets={.pass=H(VkRenderPass,1),.framebuffer=H(VkFramebuffer,1)},
		.pipelines={.raster_pass=H(VkRenderPass,1),.raster=H(VkPipeline,1)},.prepass_cutout=H(VkPipeline,2),.descriptors={.forward=H(VkDescriptorSet,3)}};
	rc_receiver_world_draw_t draw={0}; rc_receiver_world_snapshot_t snapshot={.frame_serial=2,.draws=&draw,.count=1};
	rc_receiver_forward_material_t material={.reuse_set=H(VkDescriptorSet,3)}; rc_receiver_params_t params={.divisor=1.5f}; rc_receiver_alternates_t alt={.serial=2,.fingerprint=42,.count=1,.pass=H(VkRenderPass,1),.framebuffer=H(VkFramebuffer,1),.buffers={H(VkCommandBuffer,1)}};
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,false,false,false,false,false,false,VK_NULL_HANDLE,0,0));
	assert(!select_calls && !stages);
	material.reuse_set=0; assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,false,false,false,false,VK_NULL_HANDLE,0,0));
	material.reuse_set=H(VkDescriptorSet,3); alt.count=0; assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,false,false,false,false,VK_NULL_HANDLE,0,0)); alt.count=1;
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,false,false,false,false,VK_NULL_HANDLE,0,0));
	assert(stages==1 && select_calls==2 && clears==4 && resets==4 && !last_plan.postprocess && last_plan.params.divisor==1.5f);
	r.descriptors.forward_resolved=H(VkDescriptorSet,4);
	material.reuse_set=r.descriptors.forward_resolved;
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,true,false,false,false,VK_NULL_HANDLE,0,0));
	assert(stages==1 && select_calls==2);
	r.reconstruct=H(VkPipeline,5); r.layouts.reconstruct=H(VkPipelineLayout,6);
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,true,false,false,false,VK_NULL_HANDLE,0,0));
	r.descriptors.reconstruct=H(VkDescriptorSet,7);
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,true,false,false,false,VK_NULL_HANDLE,0,0));
	assert(stages==2 && select_calls==3 && last_plan.postprocess && last_plan.reconstruct_pipeline==r.reconstruct &&
		last_plan.reconstruct_layout==r.layouts.reconstruct && last_plan.reconstruct_set==r.descriptors.reconstruct);
	assert(last_plan.checkerboard && !last_plan.texture_filter && !last_plan.gaussian_filter);
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,false,false,VK_NULL_HANDLE,0,0));
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,true,false,false,VK_NULL_HANDLE,0,0));
	r.descriptors.reconstruct_reverse=H(VkDescriptorSet,8);
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,true,false,false,VK_NULL_HANDLE,0,0));
	assert(last_plan.postprocess && last_plan.texture_filter && !last_plan.checkerboard && last_plan.gaussian_filter &&
		last_plan.reconstruct_reverse_set==r.descriptors.reconstruct_reverse);
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,true,true,false,false,VK_NULL_HANDLE,0,0));
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,true,false,false,false,VK_NULL_HANDLE,0,0));
	assert(last_plan.postprocess && last_plan.checkerboard && !last_plan.gaussian_filter);
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,true,false,false,H(VkQueryPool,9),12,0));
	assert(last_plan.timing_pool==H(VkQueryPool,9) && last_plan.timing_first_query==12);
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,false,false,false,false,VK_NULL_HANDLE,0,0));
	material.reuse_set=r.descriptors.forward;
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,false,false,VK_NULL_HANDLE,0,0));
	assert(!last_plan.postprocess && last_plan.texture_filter && !last_plan.checkerboard && !last_plan.gaussian_filter);
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,true,false,false,VK_NULL_HANDLE,0,0));
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,true,false,false,false,VK_NULL_HANDLE,0,0));
	r.soft_shadows=VK_NULL_HANDLE;
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,false,true,VK_NULL_HANDLE,0,0));
	assert(!last_plan.soft_shadows && !last_plan.soft_shadow_pipeline); /* Optional failure keeps hard receiver. */
	r.soft_shadows=H(VkPipeline,9);
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,false,true,VK_NULL_HANDLE,0,0));
	assert(last_plan.soft_shadows && last_plan.soft_shadow_pipeline==r.soft_shadows);
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,false,false,false,true,VK_NULL_HANDLE,0,0));
	assert(!last_plan.soft_shadows && !last_plan.soft_shadow_pipeline); /* Reference path never opts in. */
	/* Shadows alone select the resolved H pass, without GI/checker/reverse. */
	material.reuse_set=r.descriptors.forward_resolved;
	VkDescriptorSet reverse=r.descriptors.reconstruct_reverse;r.descriptors.reconstruct_reverse=VK_NULL_HANDLE;
	assert(RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,true,true,0,0,1.5f));
	assert(last_plan.smooth_shadows && last_plan.postprocess && last_plan.soft_shadows && !last_plan.gaussian_filter && !last_plan.checkerboard);
	assert(last_plan.shadow_filter_strength==1.5f);
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,true,true,0,0,0));
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,true,true,0,0,3));
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,true,true,0,0,NAN));
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,true,false,0,0,1.5f));
	r.soft_shadows=VK_NULL_HANDLE;
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,true,true,0,0,1.5f));
	r.soft_shadows=H(VkPipeline,9);
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,false,false,false,true,true,0,0,1.5f));
	material.reuse_set=r.descriptors.forward;
	assert(!RCReceiverResources_StageFrame(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,true,true,0,0,1.5f));
	r.descriptors.reconstruct_reverse=reverse;
	rc_receiver_model_draw_t model={.token=100,.reuse_eligible=true};rc_receiver_model_snapshot_t models={2,&model,1};
	r.models=(rc_receiver_model_pipelines_t){.device=H(VkDevice,1),.receiver_pass=r.targets.pass,.forward_pass=alt.pass};
	rc_receiver_model_alternates_t model_alt={.serial=2,.fingerprint=44,.pass=alt.pass,.framebuffer=alt.framebuffer,.count=1,.buffers={H(VkCommandBuffer,8)}};
	rc_receiver_world_snapshot_t empty={.frame_serial=2};
	assert(RCReceiverResources_StageFrameModels(&r,2,alt.buffers[0],0,0,&params,1,&empty,NULL,NULL,true,true,true,false,false,false,false,0,0,0,&models,&model_alt));
	assert(last_plan.model_draw_count==1 && last_plan.model_pipelines==&r.models && !last_plan.forward_materials);
	assert(RCReceiverResources_StageFrameModels(&r,2,alt.buffers[0],0,0,&params,1,&empty,NULL,NULL,true,true,true,false,false,true,true,0,0,1.5f,&models,&model_alt));
	assert(last_plan.postprocess && last_plan.smooth_shadows && !last_plan.gaussian_filter);
	model_alt.fingerprint=43;assert(!RCReceiverResources_StageFrameModels(&r,2,alt.buffers[0],0,0,&params,1,&empty,NULL,NULL,true,true,true,false,false,false,false,0,0,0,&models,&model_alt));
	model_alt.fingerprint=44;
	model.reuse_eligible=false;
	assert(!RCReceiverResources_StageFrameModels(&r,2,alt.buffers[0],0,0,&params,1,&empty,NULL,NULL,true,true,true,false,false,false,false,0,0,0,&models,&model_alt));
	model.reuse_eligible=true;
	assert(RCReceiverResources_StageFrameModels(&r,2,alt.buffers[0],0,0,&params,1,&snapshot,&material,&alt,true,true,true,false,false,false,false,0,0,0,&models,&model_alt));
	puts("PASS: receiver frame staging requires edge proof, matching resources and complete world/model alternates"); return 0;
}
