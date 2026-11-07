#include "radiance_receiver_diagnostics.h"
#include "../Shaders/shaders.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float inverse_view_projection[16];
    int32_t viewport[4];
    float ndc_transform[4],threshold[4];
} diagnostic_params_t;
_Static_assert(sizeof(diagnostic_params_t)==112 && offsetof(diagnostic_params_t,threshold)==96,
    "receiver diagnostic push constants must match GLSL");

bool RCReceiverDiagnostics_Destroy(rc_receiver_diagnostics_t *d)
{
    if(!d) return false;
    if(!d->device) return true;
    if(d->recorded || (d->fence && vkGetFenceStatus(d->device,d->fence)!=VK_SUCCESS)) return false;
    if(d->pipeline) vkDestroyPipeline(d->device,d->pipeline,NULL);
    if(d->layout) vkDestroyPipelineLayout(d->device,d->layout,NULL);
    if(d->pool) vkDestroyDescriptorPool(d->device,d->pool,NULL);
    if(d->set_layout) vkDestroyDescriptorSetLayout(d->device,d->set_layout,NULL);
    if(d->mapped) vkUnmapMemory(d->device,d->memory);
    if(d->buffer) vkDestroyBuffer(d->device,d->buffer,NULL);
    if(d->memory) vkFreeMemory(d->device,d->memory,NULL);
    memset(d,0,sizeof(*d)); return true;
}

bool RCReceiverDiagnostics_Create(rc_receiver_diagnostics_t *out,VkDevice device,
    const VkPhysicalDeviceMemoryProperties *memory,const rc_receiver_targets_t *targets,VkSampler nearest,VkPipelineCache cache)
{
    if(!out || !device || !memory || memory->memoryTypeCount>VK_MAX_MEMORY_TYPES || !targets || targets->device!=device || !nearest)
        return false;
    for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i) if(!targets->images[i].view) return false;
    rc_receiver_diagnostics_t d={.device=device,.targets=targets,.nearest=nearest};
    VkShaderModule module=VK_NULL_HANDLE;
    const VkDeviceSize size=sizeof(d.metadata.count);
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(vkCreateBuffer(device,&buffer,NULL,&d.buffer)!=VK_SUCCESS) goto fail;
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device,d.buffer,&requirements);
    uint32_t type=0;
    while(type<memory->memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) ||
        (memory->memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) ++type;
    if(type==memory->memoryTypeCount) goto fail;
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=type};
    if(vkAllocateMemory(device,&allocation,NULL,&d.memory)!=VK_SUCCESS || vkBindBufferMemory(device,d.buffer,d.memory,0)!=VK_SUCCESS ||
        vkMapMemory(device,d.memory,0,size,0,(void **)&d.mapped)!=VK_SUCCESS) goto fail;
    VkDescriptorSetLayoutBinding bindings[11]={0};
    for(unsigned i=0;i<11;++i) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorCount=1,
        .descriptorType=i==10?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_layout={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=11,.pBindings=bindings};
    if(vkCreateDescriptorSetLayout(device,&set_layout,NULL,&d.set_layout)!=VK_SUCCESS) goto fail;
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,10},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    if(vkCreateDescriptorPool(device,&pool,NULL,&d.pool)!=VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo sets={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=d.pool,
        .descriptorSetCount=1,.pSetLayouts=&d.set_layout};
    if(vkAllocateDescriptorSets(device,&sets,&d.set)!=VK_SUCCESS) goto fail;
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(diagnostic_params_t)};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,
        .pSetLayouts=&d.set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    if(vkCreatePipelineLayout(device,&layout,NULL,&d.layout)!=VK_SUCCESS) goto fail;
    int code_size=radiance_receiver_diagnostics_comp_spv_size;
    if(code_size<=0 || code_size%4) goto fail;
    uint32_t *code=malloc((size_t)code_size);
    if(!code) goto fail;
    memcpy(code,radiance_receiver_diagnostics_comp_spv,(size_t)code_size);
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)code_size,.pCode=code};
    VkResult shader_result=vkCreateShaderModule(device,&shader,NULL,&module); free(code);
    if(shader_result!=VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},
        .layout=d.layout};
    if(vkCreateComputePipelines(device,cache,1,&pipeline,NULL,&d.pipeline)!=VK_SUCCESS) goto fail;
    vkDestroyShaderModule(device,module,NULL); module=VK_NULL_HANDLE;
    if(!RCReceiverDiagnostics_Destroy(out)) goto fail;
    *out=d; return true;
fail:
    if(module) vkDestroyShaderModule(device,module,NULL);
    RCReceiverDiagnostics_Destroy(&d); return false;
}

bool RCReceiverDiagnostics_Record(rc_receiver_diagnostics_t *d,VkCommandBuffer command,uint64_t serial,
    const rc_receiver_params_t *params,float spacing,bool resolved,bool enabled)
{
    if(!enabled) return true;
    if(!d || !d->device || !d->pipeline || !d->targets || !command || !params || d->recorded || d->fence ||
        !RCReceiverTargets_ValidSamples(params->samples&255u) || !RCReceiver_ValidDivisor(params->divisor) || !isfinite(spacing) || spacing<=0 ||
        params->viewport[0]<0 || params->viewport[1]<0 || params->viewport[2]<=0 || params->viewport[3]<=0 ||
        (uint64_t)params->viewport[0]+(uint32_t)params->viewport[2]>d->targets->width ||
        (uint64_t)params->viewport[1]+(uint32_t)params->viewport[3]>d->targets->height ||
        !isfinite(params->plane_tolerance) || params->plane_tolerance<0 || !isfinite(params->normal_cosine) ||
        params->normal_cosine< -1 || params->normal_cosine>1) return false;
    for(unsigned i=0;i<16;++i) if(!isfinite(params->inverse_view_projection[i])) return false;
    for(unsigned i=0;i<4;++i) if(!isfinite(params->ndc_transform[i])) return false;
    const unsigned indices[10]={RC_RECEIVER_DEPTH,RC_RECEIVER_NORMAL,RC_RECEIVER_ID,RC_RECEIVER_HALF_POSITION,
        RC_RECEIVER_HALF_NORMAL,RC_RECEIVER_HALF_STATUS,RC_RECEIVER_HALF_INDIRECT,
        resolved?RC_RECEIVER_RESOLVED_INDIRECT:RC_RECEIVER_HALF_INDIRECT,
        resolved?RC_RECEIVER_RESOLVED_DIRECT:RC_RECEIVER_HALF_DIRECT,
        resolved?RC_RECEIVER_RESOLVED_MINIMUM:RC_RECEIVER_HALF_MINIMUM};
    VkDescriptorImageInfo images[10]; VkWriteDescriptorSet writes[11]={0};
    for(unsigned i=0;i<10;++i) {
        if(!d->targets->images[indices[i]].view) return false;
        images[i]=(VkDescriptorImageInfo){.sampler=d->nearest,.imageView=d->targets->images[indices[i]].view,
            .imageLayout=i==0?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=d->set,.dstBinding=i,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
    }
    VkDescriptorBufferInfo counter_buffer={d->buffer,0,sizeof(d->metadata.count)};
    writes[10]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=d->set,.dstBinding=10,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&counter_buffer};
    vkUpdateDescriptorSets(d->device,11,writes,0,NULL);
    vkCmdFillBuffer(command,d->buffer,0,sizeof(d->metadata.count),0);
    /* The ordinary postpass publishes resolved textures to fragment shaders.
     * Diagnostics additionally consume those writes in compute. */
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);
    diagnostic_params_t push;
    memcpy(push.inverse_view_projection,params->inverse_view_projection,sizeof(push.inverse_view_projection));
    memcpy(push.viewport,params->viewport,sizeof(push.viewport)); memcpy(push.ndc_transform,params->ndc_transform,sizeof(push.ndc_transform));
    push.threshold[0]=params->normal_cosine; push.threshold[1]=params->plane_tolerance;
    push.threshold[2]=params->divisor; push.threshold[3]=spacing;
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,d->pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,d->layout,0,1,&d->set,0,NULL);
    vkCmdPushConstants(command,d->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(command,((uint32_t)params->viewport[2]+7)/8,((uint32_t)params->viewport[3]+7)/8,1);
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=d->buffer,.offset=0,.size=sizeof(d->metadata.count)};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    d->metadata=(rc_receiver_diagnostic_stats_t){.frame_serial=serial,.width=(uint32_t)params->viewport[2],
        .height=(uint32_t)params->viewport[3],.divisor=params->divisor,.use_resolved=resolved};
    d->recorded=true; return true;
}

bool RCReceiverDiagnostics_Submitted(rc_receiver_diagnostics_t *d,VkFence fence)
{
    if(!d || !d->device || !d->recorded || d->fence || !fence) return false;
    d->recorded=false; d->fence=fence; return true;
}
bool RCReceiverDiagnostics_Collect(rc_receiver_diagnostics_t *d,rc_receiver_diagnostic_stats_t *stats)
{
    if(!d || !d->device || !d->fence || vkGetFenceStatus(d->device,d->fence)!=VK_SUCCESS) return false;
    if(stats) { *stats=d->metadata; memcpy(stats->count,d->mapped,sizeof(stats->count)); }
    d->fence=VK_NULL_HANDLE; return true;
}
void RCReceiverDiagnostics_Discard(rc_receiver_diagnostics_t *d) { if(d && !d->fence) d->recorded=false; }
