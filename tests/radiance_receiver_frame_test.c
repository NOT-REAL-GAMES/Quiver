#include "radiance_receiver_light.h"
#include "radiance_receiver_frame.h"
#include "radiance_receiver_forward.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define H(type,n) ((type)(uintptr_t)(n))
uint64_t RCReceiverForward_Fingerprint(uint64_t h,const rc_receiver_world_snapshot_t *s,const rc_receiver_forward_material_t *m)
{ (void)h;(void)s;(void)m;return 0; }
static unsigned frame_calls, can_calls, draw_calls;
static unsigned model_can_calls,model_draw_calls;
static bool allow_model=true;
bool RCReceiverModel_CanRecord(VkCommandBuffer c,const rc_receiver_model_pipelines_t *p,const rc_receiver_model_snapshot_t *s,uint64_t serial,bool f,VkDescriptorSet r)
{ (void)c;(void)r;assert(p && s->count==1 && s->frame_serial==serial && !f);++model_can_calls;return allow_model; }
void RCReceiverModel_Record(VkCommandBuffer c,const rc_receiver_model_pipelines_t *p,const rc_receiver_model_snapshot_t *s,bool f,VkDescriptorSet r)
{ (void)c;(void)r;assert(p && s->count==1 && !f);++model_draw_calls; }
uint64_t RCReceiverModel_Fingerprint(const rc_receiver_model_snapshot_t *s) { assert(s->count==1 && s->draws);return 44; }
static bool allow_world, allow_frame, expect_timing;
static VkPipeline expected_soft_pipeline;
static struct {
    unsigned barriers, sets, pushes, dispatches, timestamps;
    uint32_t queries[4];
    struct { VkPipelineStageFlags source,destination; unsigned memory_count,count; VkMemoryBarrier memory; VkImageMemoryBarrier images[6]; } barrier[4];
    VkDescriptorSet set[3];
    struct { int32_t viewport[4]; float threshold[4]; } constants[3];
    uint32_t groups[3][3];
} captured;
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags a,VkPipelineStageFlags b,VkDependencyFlags d,uint32_t mc,const VkMemoryBarrier *m,uint32_t bc,const VkBufferMemoryBarrier *bs,uint32_t ic,const VkImageMemoryBarrier *is)
{
    (void)c;(void)d;(void)bs; assert(!bc && captured.barriers<4 && ic<=6 && is && mc<=1);
    unsigned n=captured.barriers++; captured.barrier[n].source=a; captured.barrier[n].destination=b;
    captured.barrier[n].memory_count=mc; if(mc) captured.barrier[n].memory=*m;
    captured.barrier[n].count=ic; memcpy(captured.barrier[n].images,is,ic*sizeof(*is));
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p)
{ (void)c;assert(b==VK_PIPELINE_BIND_POINT_COMPUTE && p); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *s,uint32_t dc,const uint32_t *o)
{ (void)c;(void)l;(void)o;assert(b==VK_PIPELINE_BIND_POINT_COMPUTE && !first && !dc && n==2 && captured.sets<3 && s[1]==H(VkDescriptorSet,3));captured.set[captured.sets++]=s[0]; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags f,uint32_t o,uint32_t n,const void *v)
{ (void)c;(void)l;assert(f==VK_SHADER_STAGE_COMPUTE_BIT && !o && n==32 && v && captured.pushes<3);memcpy(&captured.constants[captured.pushes++],v,n); }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z)
{ (void)c;assert(captured.dispatches<3);unsigned n=captured.dispatches++;captured.groups[n][0]=x;captured.groups[n][1]=y;captured.groups[n][2]=z; }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t query)
{ (void)c;assert(expect_timing && stage==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT && pool==H(VkQueryPool,1) && captured.timestamps<4);captured.queries[captured.timestamps++]=query; }
bool RCReceiverWorld_CanRecord(VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline pipelines[2],const rc_receiver_world_draw_t *draws,uint32_t count)
{ (void)command;(void)layout;(void)pipelines;(void)draws;assert(count==1);++can_calls;return allow_world; }
void RCReceiverWorld_Record(VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline pipelines[2],const rc_receiver_world_draw_t *draws,uint32_t count)
{ (void)command;(void)layout;(void)pipelines;(void)draws;assert(count==1);++draw_calls; }
bool RCReceiver_RecordFrameSoftTimed(VkCommandBuffer command,const rc_receiver_targets_t *t,const rc_receiver_pipelines_t *p,
    VkPipelineLayout a,VkPipelineLayout b,VkDescriptorSet c,VkDescriptorSet d,VkDescriptorSet e,VkDescriptorSet f,
    const rc_receiver_params_t *params,float depth,bool (*ready)(void *),void (*draw)(VkCommandBuffer,void *),void *user,VkQueryPool timing_pool,uint32_t first_query,VkPipeline soft_shadow_pipeline)
{
    assert(soft_shadow_pipeline==expected_soft_pipeline);
    (void)t;(void)p;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)params;(void)depth;
    assert((timing_pool!=VK_NULL_HANDLE)==expect_timing);if(timing_pool)assert(first_query==12);++frame_calls;
    if(!allow_frame || !ready(user))return false;
    draw(command,user);return true;
}
static void image_barrier(unsigned group,unsigned index,unsigned target,VkImageLayout from,VkImageLayout to,VkAccessFlags source,VkAccessFlags destination)
{
    const VkImageMemoryBarrier *b=&captured.barrier[group].images[index];
    assert(b->image==H(VkImage,target+1) && b->oldLayout==from && b->newLayout==to);
    assert(b->srcAccessMask==source && b->dstAccessMask==destination);
    assert(b->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && b->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
    assert(b->subresourceRange.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT && b->subresourceRange.levelCount==1 && b->subresourceRange.layerCount==1);
}
static void postprocess(const rc_receiver_frame_plan_t *p)
{
    bool separable=p->gaussian_filter || p->smooth_shadows;
    unsigned passes=p->postprocess?(separable?3:1):0;
    assert(captured.dispatches==passes && captured.pushes==passes && captured.sets==passes);
    /* Reconstruct bounds, with horizontal and vertical ends between them on three-pass frames. */
    if(!expect_timing) assert(!captured.timestamps);
    else if(captured.dispatches==3) assert(captured.timestamps==4 && captured.queries[0]==16 && captured.queries[1]==18 && captured.queries[2]==19 && captured.queries[3]==17);
    else assert(captured.timestamps==2 && captured.queries[0]==16 && captured.queries[1]==17);
    if(!passes){assert(!captured.barriers);return;}
    unsigned last=separable?3:1;
    assert(captured.barriers==last+1 && captured.barrier[0].count==3 && captured.barrier[last].count==3);
    assert(captured.barrier[0].memory_count==1 && captured.barrier[0].memory.srcAccessMask==VK_ACCESS_SHADER_WRITE_BIT && captured.barrier[0].memory.dstAccessMask==VK_ACCESS_SHADER_READ_BIT);
    assert(captured.barrier[last].memory_count==0 && captured.barrier[last].destination==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    for(unsigned n=0;n<captured.barriers;++n)assert(captured.barrier[n].source==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    for(unsigned i=0;i<3;++i){
        image_barrier(0,i,RC_RECEIVER_RESOLVED_INDIRECT+i,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT);
        image_barrier(last,i,RC_RECEIVER_RESOLVED_INDIRECT+i,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    }
    for(unsigned i=0;i<passes;++i){
        assert(captured.set[i]==(i==1?p->reconstruct_reverse_set:p->reconstruct_set));
        assert(!memcmp(captured.constants[i].viewport,p->params.viewport,sizeof(p->params.viewport)));
        assert(captured.constants[i].threshold[0]==p->params.normal_cosine && captured.constants[i].threshold[1]==p->params.plane_tolerance);
        assert(captured.constants[i].threshold[2]==p->params.divisor && captured.constants[i].threshold[3]==i);
        uint32_t w=RCReceiver_ScaledExtent((uint32_t)p->params.viewport[2],p->params.divisor),h=RCReceiver_ScaledExtent((uint32_t)p->params.viewport[3],p->params.divisor);
        uint32_t group=p->filter_group?p->filter_group:RC_RECEIVER_FILTER_GROUP;
        uint32_t x=p->texture_filter?((i==1?h:w)+group-1)/group:(w+7)/8,y=p->texture_filter?(i==1?w:h):(h+7)/8;
        assert(captured.groups[i][0]==x && captured.groups[i][1]==y && captured.groups[i][2]==1);
    }
    if(separable)for(unsigned n=1;n<3;++n){
        assert(captured.barrier[n].count==6 && !captured.barrier[n].memory_count && captured.barrier[n].destination==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        for(unsigned field=0;field<3;++field) {
            image_barrier(n,field*2,(n==1?RC_RECEIVER_RESOLVED_INDIRECT:RC_RECEIVER_HALF_INDIRECT)+field,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            image_barrier(n,field*2+1,(n==1?RC_RECEIVER_HALF_INDIRECT:RC_RECEIVER_RESOLVED_INDIRECT)+field,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT);
        }
    }
}
static void good(rc_receiver_frame_plan_t *p)
{
    memset(&captured,0,sizeof(captured));expected_soft_pipeline=p->texture_filter && p->soft_shadows?p->soft_shadow_pipeline:VK_NULL_HANDLE;RCReceiverFrame_Stage(p);
    assert(RCReceiverFrame_Record(H(VkCommandBuffer,1),p->frame_serial));postprocess(p);
    assert(RCReceiverFrame_Active(p->frame_serial) && !RCReceiverFrame_Active(p->frame_serial+1));
    assert(RCReceiverFrame_ActiveDivisor(p->frame_serial)==p->params.divisor);
    assert(RCReceiverFrame_ActiveCheckerboard(p->frame_serial)==p->checkerboard);
    assert(RCReceiverFrame_ActiveFilterMode(p->frame_serial)==(unsigned)p->texture_filter);
    assert(RCReceiverFrame_ActivePostprocess(p->frame_serial)==p->postprocess);
    assert(RCReceiverFrame_ActiveSoftShadows(p->frame_serial)==(expected_soft_pipeline!=VK_NULL_HANDLE));
    assert(RCReceiverFrame_ActiveShadowFilter(p->frame_serial)==(p->smooth_shadows?p->shadow_filter_strength:0));
    assert(!RCReceiverFrame_ActiveShadowFilter(p->frame_serial+1));
    assert(RCReceiverFrame_ActiveShadowFilterRadius(p->frame_serial)==(p->smooth_shadows?p->shadow_filter_radius:0));
    assert(!RCReceiverFrame_ActiveShadowFilterRadius(p->frame_serial+1));
    assert(!RCReceiverFrame_ActiveSoftShadows(p->frame_serial+1));
    const rc_receiver_params_t *params=RCReceiverFrame_ActiveParams(p->frame_serial);
    assert(params && params!=&p->params && !memcmp(params,&p->params,sizeof(*params)));
}
static void malformed(rc_receiver_frame_plan_t *p)
{
    memset(&captured,0,sizeof(captured));unsigned before=frame_calls;RCReceiverFrame_Stage(p);
    assert(!RCReceiverFrame_Record(H(VkCommandBuffer,1),p->frame_serial) && frame_calls==before);
    assert(!captured.barriers && !captured.timestamps && !RCReceiverFrame_Active(p->frame_serial));
}
static unsigned light_records; static bool light_postprocessed;
void RCReceiverLight_Record(VkCommandBuffer c,const rc_receiver_light_t *l,const rc_receiver_params_t *p,bool post,VkDescriptorSet s)
{ (void)c; assert(l && p && s); light_postprocessed=post; ++light_records; }
int main(void)
{
    assert(RCReceiverFrame_CompletedSerial(43)==42 && RCReceiverFrame_CompletedSerial(0)==UINT32_MAX);
    VkCommandBuffer command=H(VkCommandBuffer,1);rc_receiver_targets_t targets={0};
    for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i)targets.images[i].image=H(VkImage,i+1);
    rc_receiver_frame_plan_t p={.frame_serial=12,.enabled=true,.world_draw_count=1,.targets=&targets,.cascade_set=H(VkDescriptorSet,3),
        .params={.divisor=2,.viewport={3,1,641,361},.normal_cosine=.99f,.plane_tolerance=.01f}};
    RCReceiverFrame_Reset();assert(!RCReceiverFrame_Record(command,12));malformed(&p);
    p.edge_fallback_enabled=true;RCReceiverFrame_Stage(&p);
    assert(RCReceiverFrame_PendingTargets(12)==&targets && !RCReceiverFrame_PendingTargets(13));
    assert(!RCReceiverFrame_Record(command,13) && !frame_calls);
    allow_frame=true;RCReceiverFrame_Stage(&p);assert(!RCReceiverFrame_Record(command,12) && can_calls==1 && !draw_calls);
    allow_world=true;good(&p);assert(draw_calls==1);
    assert(!RCReceiverFrame_Record(command,12) && !RCReceiverFrame_Active(12));
    p.postprocess=true;p.checkerboard=true;malformed(&p);
    p.reconstruct_pipeline=H(VkPipeline,1);p.reconstruct_layout=H(VkPipelineLayout,1);p.reconstruct_set=H(VkDescriptorSet,1);
    const float divisors[]={1.5f,2,3,4};
    for(unsigned d=0;d<4;++d){p.params.divisor=divisors[d];good(&p);} /* Legacy 8x8 checker reconstruction. */
    p.texture_filter=true;
    for(unsigned d=0;d<4;++d){p.params.divisor=divisors[d];good(&p);} /* Zero Gaussian + checker is one pass. */
    p.filter_group=RC_RECEIVER_FILTER_GROUP_WIDE;good(&p);p.filter_group=0;
    p.checkerboard=false;malformed(&p);p.gaussian_filter=true;malformed(&p); /* Missing reverse set. */
    p.reconstruct_reverse_set=H(VkDescriptorSet,2);
    for(unsigned d=0;d<4;++d)for(unsigned checker=0;checker<2;++checker){p.params.divisor=divisors[d];p.checkerboard=checker!=0;good(&p);}
    p.texture_filter=false;malformed(&p);p.texture_filter=true;p.postprocess=false;malformed(&p);
    p.gaussian_filter=false;p.checkerboard=false;good(&p); /* Raw fast path records no filter. */
    p.checkerboard=true;malformed(&p);p.postprocess=true;good(&p);
    p.timing_pool=H(VkQueryPool,1);p.timing_first_query=12;expect_timing=true;good(&p);
    p.gaussian_filter=true;good(&p);p.gaussian_filter=false;p.postprocess=false;p.checkerboard=false;good(&p);
    p.soft_shadows=true;p.soft_shadow_pipeline=H(VkPipeline,5);good(&p);
    p.smooth_shadows=true;p.shadow_filter_strength=1.5f;p.shadow_filter_radius=8;malformed(&p); /* Shadow filtering needs resolved output. */
    p.postprocess=true;
    const float shadow_divisors[]={1,1.5f,2,3,4};
    for(unsigned d=0;d<5;++d) { p.params.divisor=shadow_divisors[d];good(&p);assert(captured.dispatches==3); }
    const float shadow_strengths[]={.5f,1,2};
    for(unsigned strength=0;strength<3;++strength) { p.shadow_filter_strength=shadow_strengths[strength];good(&p);assert(captured.dispatches==3); }
    p.gaussian_filter=true;good(&p);assert(captured.dispatches==3);p.gaussian_filter=false;
    p.soft_shadow_pipeline=VK_NULL_HANDLE;good(&p); /* Gaussian survives optional ray pipeline failure. */
    p.soft_shadow_pipeline=H(VkPipeline,5);p.soft_shadows=false;good(&p);p.soft_shadows=true;
    p.reconstruct_reverse_set=VK_NULL_HANDLE;malformed(&p);p.reconstruct_reverse_set=H(VkDescriptorSet,2);
    p.texture_filter=false;malformed(&p);p.texture_filter=true;
    p.shadow_filter_strength=NAN;malformed(&p);p.shadow_filter_strength=-1;malformed(&p);
    p.shadow_filter_strength=3;malformed(&p);p.shadow_filter_strength=0;malformed(&p);
    p.shadow_filter_strength=1;p.shadow_filter_radius=NAN;malformed(&p);
    p.shadow_filter_radius=-1;malformed(&p);p.shadow_filter_radius=129;malformed(&p);p.shadow_filter_radius=0;malformed(&p);
    p.shadow_filter_strength=0;p.smooth_shadows=false;malformed(&p);p.postprocess=false;
    p.soft_shadow_pipeline=VK_NULL_HANDLE;good(&p); /* Missing optional pipeline leaves hard path. */
    p.soft_shadow_pipeline=H(VkPipeline,5);p.texture_filter=false;good(&p);
    p.texture_filter=true;p.soft_shadows=false;good(&p); /* Cached pipeline is inactive while disabled. */
    p.timing_first_query=UINT32_MAX-4;malformed(&p);
    p.timing_pool=VK_NULL_HANDLE;expect_timing=false;p.timing_first_query=0;
    rc_receiver_model_draw_t model={.reuse_eligible=true};rc_receiver_model_pipelines_t models={0};
    p.world_draw_count=0;p.model_draw_count=1;p.model_draws=&model;p.model_pipelines=&models;p.model_fingerprint=44;
    unsigned before_world=draw_calls;good(&p);assert(model_can_calls==1 && model_draw_calls==1 && draw_calls==before_world);
    p.model_fingerprint=43;malformed(&p);p.model_fingerprint=44;
    allow_model=false;RCReceiverFrame_Stage(&p);assert(!RCReceiverFrame_Record(command,12) && model_draw_calls==1);allow_model=true;
    p.world_draw_count=1;good(&p);assert(model_draw_calls==2 && draw_calls==before_world+1);
    p.world_draw_count=0;model.reuse_eligible=false;RCReceiverFrame_Stage(&p);
    assert(!RCReceiverFrame_Record(command,12) && model_draw_calls==2);model.reuse_eligible=true;
    RCReceiverFrame_Stage(&p);RCReceiverFrame_Stage(NULL);assert(!RCReceiverFrame_Record(command,12));
    /* The light pass follows the filter and takes its output. */
    { rc_receiver_light_t light={0}; p.light=&light; good(&p); assert(light_records==1 && light_postprocessed==p.postprocess); p.light=NULL; }
    puts("PASS: receiver frame preflight, legacy/checker/raw modes and separable Gaussian descriptors, barriers, constants and dispatches");
    return 0;
}
