#include "radiance_receiver_diagnostics.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H(type,n) ((type)(uintptr_t)(n))
const unsigned char radiance_receiver_diagnostics_comp_spv[4]={3,2,35,7};
const int radiance_receiver_diagnostics_comp_spv_size=4;
static unsigned calls,fail,buffers,memories,maps,set_layouts,pools,layouts,modules,pipelines,commands,updates;
static bool expect_resolved;
static VkResult fence_status=VK_NOT_READY;
static uint32_t counters[16];
static VkResult next(void) { return ++calls==fail?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS; }
static unsigned alive(void) { return buffers+memories+maps+set_layouts+pools+layouts+modules+pipelines; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *o)
{ (void)d;(void)a;assert(i->size==64 && (i->usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT));VkResult r=next();if(!r){++buffers;*o=H(VkBuffer,1);}return r; }
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks *a)
{ (void)d;(void)b;(void)a;assert(buffers);--buffers; }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r)
{ (void)d;(void)b;*r=(VkMemoryRequirements){.size=64,.alignment=4,.memoryTypeBits=1}; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *o)
{ (void)d;(void)a;assert(i->allocationSize==64 && !i->memoryTypeIndex);VkResult r=next();if(!r){++memories;*o=H(VkDeviceMemory,1);}return r; }
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a)
{ (void)d;(void)m;(void)a;assert(memories);--memories; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o)
{ (void)d;(void)b;(void)m;assert(!o);return next(); }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize s,VkMemoryMapFlags f,void **p)
{ (void)d;(void)m;assert(!o && s==64 && !f);VkResult r=next();if(!r){++maps;*p=counters;}return r; }
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d,VkDeviceMemory m)
{ (void)d;(void)m;assert(maps);--maps; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorSetLayout *o)
{ (void)d;(void)a;assert(i->bindingCount==11 && i->pBindings[10].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);VkResult r=next();if(!r){++set_layouts;*o=H(VkDescriptorSetLayout,1);}return r; }
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout s,const VkAllocationCallbacks *a)
{ (void)d;(void)s;(void)a;assert(set_layouts);--set_layouts; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d,const VkDescriptorPoolCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorPool *o)
{ (void)d;(void)a;assert(i->maxSets==1 && i->poolSizeCount==2);VkResult r=next();if(!r){++pools;*o=H(VkDescriptorPool,1);}return r; }
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice d,VkDescriptorPool p,const VkAllocationCallbacks *a)
{ (void)d;(void)p;(void)a;assert(pools);--pools; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *o)
{ (void)d;assert(i->descriptorSetCount==1);VkResult r=next();if(!r)*o=H(VkDescriptorSet,1);return r; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkPipelineLayout *o)
{ (void)d;(void)a;assert(i->setLayoutCount==1 && i->pPushConstantRanges[0].size==112);VkResult r=next();if(!r){++layouts;*o=H(VkPipelineLayout,1);}return r; }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d,VkPipelineLayout l,const VkAllocationCallbacks *a)
{ (void)d;(void)l;(void)a;assert(layouts);--layouts; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *o)
{ (void)d;(void)a;assert(i->codeSize==4 && !((uintptr_t)i->pCode&3u));VkResult r=next();if(!r){++modules;*o=H(VkShaderModule,1);}return r; }
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d,VkShaderModule m,const VkAllocationCallbacks *a)
{ (void)d;(void)m;(void)a;assert(modules);--modules; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *o)
{ (void)d;(void)c;(void)a;assert(n==1 && i->layout && i->stage.module);VkResult r=next();++pipelines;*o=H(VkPipeline,1);return r; /* exercise partial failure handle cleanup */ }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a)
{ (void)d;(void)p;(void)a;assert(pipelines);--pipelines; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice d,VkFence f)
{ (void)d;assert(f);return fence_status; }
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t c,const VkCopyDescriptorSet *p)
{
    (void)d;(void)p;assert(n==11 && !c);
    const unsigned selected[3]={expect_resolved?RC_RECEIVER_RESOLVED_INDIRECT:RC_RECEIVER_HALF_INDIRECT,
        expect_resolved?RC_RECEIVER_RESOLVED_DIRECT:RC_RECEIVER_HALF_DIRECT,expect_resolved?RC_RECEIVER_RESOLVED_MINIMUM:RC_RECEIVER_HALF_MINIMUM};
    assert(w[0].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    assert(w[6].pImageInfo->imageView==H(VkImageView,RC_RECEIVER_HALF_INDIRECT+1));
    for(unsigned j=0;j<3;++j) assert(w[7+j].pImageInfo->imageView==H(VkImageView,selected[j]+1));
    assert(w[10].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && w[10].pBufferInfo->range==64);++updates;
}
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize s,uint32_t v)
{ assert(c && b && !o && s==64 && !v);memset(counters,0,sizeof(counters));++commands; }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,
    uint32_t mn,const VkMemoryBarrier *m,uint32_t bn,const VkBufferMemoryBarrier *b,uint32_t in,const VkImageMemoryBarrier *i)
{
    (void)i;assert(c && !f && !in);
    if(d==VK_PIPELINE_STAGE_HOST_BIT) assert(s==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && !mn && bn==1 && b->dstAccessMask==VK_ACCESS_HOST_READ_BIT);
    else assert((s&VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) && (s&VK_PIPELINE_STAGE_TRANSFER_BIT) && d==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && mn==1 && !bn && (m->srcAccessMask&VK_ACCESS_SHADER_WRITE_BIT));
    ++commands;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline b)
{ assert(c && p==VK_PIPELINE_BIND_POINT_COMPUTE && b);++commands; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t f,uint32_t n,const VkDescriptorSet *s,uint32_t on,const uint32_t *o)
{ (void)o;assert(c && p==VK_PIPELINE_BIND_POINT_COMPUTE && l && !f && n==1 && *s && !on);++commands; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags f,uint32_t o,uint32_t s,const void *p)
{ assert(c && l && f==VK_SHADER_STAGE_COMPUTE_BIT && !o && s==112);const float *v=p;assert(v[24]==.99f && v[25]==.01f && v[26]==1.5f && v[27]==32);++commands; }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z)
{ assert(c && x==3 && y==2 && z==1);for(unsigned i=0;i<16;++i)counters[i]=i+1;++commands; }
int main(void)
{
    rc_receiver_targets_t t={.device=H(VkDevice,1),.width=40,.height=30};
    for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i) t.images[i].view=H(VkImageView,i+1);
    VkPhysicalDeviceMemoryProperties memory={.memoryTypeCount=1};
    memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    rc_receiver_diagnostics_t d={0};
    assert(RCReceiverDiagnostics_Record(NULL,0,0,NULL,0,false,false) && !commands && !calls);
    for(unsigned f=1;f<=10;++f) { calls=0;fail=f;assert(!RCReceiverDiagnostics_Create(&d,t.device,&memory,&t,H(VkSampler,1),0));assert(!d.device && !alive()); }
    calls=fail=0;assert(RCReceiverDiagnostics_Create(&d,t.device,&memory,&t,H(VkSampler,1),0) && calls==10 && !modules);
    unsigned old_alive=alive(); calls=0;fail=5;
    assert(!RCReceiverDiagnostics_Create(&d,t.device,&memory,&t,H(VkSampler,1),0) && d.pipeline && alive()==old_alive);
    fail=0;
    rc_receiver_params_t params={.viewport={3,2,17,9},.samples=1,.divisor=4,.normal_cosine=.99f,.plane_tolerance=.01f};
    params.divisor=.5f;assert(!RCReceiverDiagnostics_Record(&d,H(VkCommandBuffer,1),47,&params,32,true,true) && !commands);
    params.divisor=17;assert(!RCReceiverDiagnostics_Record(&d,H(VkCommandBuffer,1),47,&params,32,true,true) && !commands);
    params.divisor=4;params.inverse_view_projection[0]=NAN;
    assert(!RCReceiverDiagnostics_Record(&d,H(VkCommandBuffer,1),47,&params,32,true,true) && !commands);
    params.inverse_view_projection[0]=1;params.divisor=1.5f;expect_resolved=true;
    assert(RCReceiverDiagnostics_Record(&d,H(VkCommandBuffer,1),47,&params,32,true,true) && d.recorded && commands==7 && updates==1);
    assert(!RCReceiverDiagnostics_Destroy(&d));assert(!RCReceiverDiagnostics_Collect(&d,NULL));
    assert(!RCReceiverDiagnostics_Record(&d,H(VkCommandBuffer,1),47,&params,32,true,true) && commands==7);
    assert(!RCReceiverDiagnostics_Submitted(&d,0));assert(RCReceiverDiagnostics_Submitted(&d,H(VkFence,1)));
    rc_receiver_diagnostic_stats_t stats={0};assert(!RCReceiverDiagnostics_Collect(&d,&stats));assert(!RCReceiverDiagnostics_Destroy(&d));
    fence_status=VK_SUCCESS;assert(RCReceiverDiagnostics_Collect(&d,&stats) && !d.fence && stats.frame_serial==47 && stats.width==17 && stats.height==9 && stats.divisor==1.5f && stats.use_resolved);
    for(unsigned i=0;i<16;++i) assert(stats.count[i]==i+1);
    assert(!RCReceiverDiagnostics_Collect(&d,&stats));expect_resolved=false;
    assert(RCReceiverDiagnostics_Record(&d,H(VkCommandBuffer,1),48,&params,32,false,true));
    RCReceiverDiagnostics_Discard(&d);assert(!d.recorded && RCReceiverDiagnostics_Destroy(&d) && !alive());
    puts("PASS: optional diagnostics emit no disabled work, preserve fences, dispatch full viewport, and clean every creation failure");
    return 0;
}
