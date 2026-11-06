#include "radiance_scene_rt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void FreeBuffer(rc_scene_rt_t *rt, rc_scene_rt_buffer_t *b)
{
    if (b->buffer) vkDestroyBuffer(rt->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(rt->device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}
static void FreeFrame(rc_scene_rt_t *rt, rc_scene_rt_frame_t *f)
{
    if (f->batch_blas) rt->destroy_as(rt->device, f->batch_blas, NULL);
    FreeBuffer(rt, &f->batch_vertices); FreeBuffer(rt, &f->batch_memory);
    if (f->static_blas) rt->destroy_as(rt->device, f->static_blas, NULL);
    FreeBuffer(rt, &f->static_vertices); FreeBuffer(rt, &f->static_memory);
    if (f->tlas) rt->destroy_as(rt->device, f->tlas, NULL);
    if (f->blas) rt->destroy_as(rt->device, f->blas, NULL);
    FreeBuffer(rt, &f->scratch); FreeBuffer(rt, &f->instance); FreeBuffer(rt, &f->vertices);
    FreeBuffer(rt, &f->tlas_memory); FreeBuffer(rt, &f->blas_memory);
    memset(f, 0, sizeof(*f));
}

static bool Buffer(rc_scene_rt_t *rt, rc_scene_rt_buffer_t *b, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags required, const void *initial)
{
    if (!size) return false;
    VkBufferCreateInfo bi = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=size,
        .usage=usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(rt->device, &bi, NULL, &b->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(rt->device, b->buffer, &req);
    uint32_t i;
    for (i=0; i<rt->memory.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (rt->memory.memoryTypes[i].propertyFlags & required) == required) break;
    if (i == rt->memory.memoryTypeCount) return false;
    VkMemoryAllocateFlagsInfo flags = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext=&flags,
        .allocationSize=req.size, .memoryTypeIndex=i};
    if (vkAllocateMemory(rt->device, &ai, NULL, &b->memory) != VK_SUCCESS ||
        vkBindBufferMemory(rt->device, b->buffer, b->memory, 0) != VK_SUCCESS) return false;
    b->allocation_size = req.size;
    if (initial) {
        void *mapped = NULL;
        if (vkMapMemory(rt->device, b->memory, 0, size, 0, &mapped) != VK_SUCCESS) return false;
        memcpy(mapped, initial, (size_t)size); vkUnmapMemory(rt->device, b->memory);
    }
    VkBufferDeviceAddressInfo address = {.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer=b->buffer};
    b->address = rt->get_buffer_address(rt->device, &address);
    return b->address != 0;
}

bool RCSceneRT_Create(rc_scene_rt_t *rt, VkPhysicalDevice physical, VkDevice device, uint32_t frames)
{
    if (!rt) return false;
    memset(rt, 0, sizeof(*rt));
    if (!physical || !device || !frames || frames > RC_SCENE_RT_MAX_FRAMES) return false;
    VkPhysicalDeviceRayQueryFeaturesKHR query = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext=&query};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .pNext=&as};
    VkPhysicalDeviceFeatures2 features = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext=&bda};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!bda.bufferDeviceAddress || !as.accelerationStructure || !query.rayQuery) return false;
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32_SFLOAT, &format);
    if (!(format.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)) return false;
    rt->device=device; rt->frames=frames;
    vkGetPhysicalDeviceMemoryProperties(physical, &rt->memory);
    rt->properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext=&rt->properties};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    rt->get_buffer_address=(PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress");
    if (!rt->get_buffer_address) rt->get_buffer_address=(PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR");
    rt->create_as=(PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
    rt->destroy_as=(PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR");
    rt->get_sizes=(PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
    rt->get_as_address=(PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR");
    rt->cmd_build=(PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR");
    if (!rt->get_buffer_address || !rt->create_as || !rt->destroy_as || !rt->get_sizes || !rt->get_as_address || !rt->cmd_build) {
        memset(rt, 0, sizeof(*rt)); return false;
    }
    return true;
}

static bool CreateAS(rc_scene_rt_t *rt, rc_scene_rt_buffer_t *buffer, VkDeviceSize size,
                     VkAccelerationStructureTypeKHR type, VkAccelerationStructureKHR *as)
{
    if (!Buffer(rt, buffer, size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL)) return false;
    VkAccelerationStructureCreateInfoKHR ci = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer=buffer->buffer, .size=size, .type=type};
    return rt->create_as(rt->device, &ci, NULL, as) == VK_SUCCESS;
}

bool RCSceneRT_Build(rc_scene_rt_t *rt, uint32_t frame, const rc_scene_t *scene, VkCommandBuffer command)
{
    if (!rt || !rt->device || frame >= rt->frames || !scene || !command) return false;
    rc_scene_rt_frame_t *f=&rt->frame[frame];
    FreeFrame(rt, f);
    uint32_t n=scene->index_count;
    if (!n || !scene->triangles || !scene->indices || n > UINT32_MAX/3 ||
        n > rt->properties.maxPrimitiveCount) return false;
#if SIZE_MAX <= UINT32_MAX
    if (n > SIZE_MAX / (9u * sizeof(float))) return false;
#endif
    float *vertices=malloc((size_t)n * 9 * sizeof(float));
    if (!vertices) return false;
    for (uint32_t i=0; i<n; ++i) {
        if (scene->indices[i] >= scene->triangle_count) { free(vertices); return false; }
        const rc_scene_triangle_t *tri=&scene->triangles[scene->indices[i]];
        for (int p=0; p<3; ++p) for (int c=0; c<3; ++c) {
            float v=tri->position[p][c];
            if (!isfinite(v)) { free(vertices); return false; }
            vertices[(size_t)i*9 + p*3 + c]=v;
        }
    }
    bool success=Buffer(rt, &f->vertices, (VkDeviceSize)n*9*sizeof(float),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vertices);
    free(vertices);
    if (!success) goto fail;
    VkAccelerationStructureGeometryTrianglesDataKHR tri = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .vertexFormat=VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress=f->vertices.address,
        .vertexStride=3*sizeof(float), .maxVertex=n*3-1, .indexType=VK_INDEX_TYPE_NONE_KHR};
    VkAccelerationStructureGeometryKHR blas_geometry = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR, .geometry.triangles=tri};
    VkAccelerationStructureBuildGeometryInfoKHR blas_info = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, .flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount=1, .pGeometries=&blas_geometry};
    VkAccelerationStructureBuildSizesInfoKHR blas_size = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt->get_sizes(rt->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blas_info, &n, &blas_size);
    if (!CreateAS(rt, &f->blas_memory, blas_size.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &f->blas)) goto fail;
    VkAccelerationStructureDeviceAddressInfoKHR address_info = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure=f->blas};
    VkDeviceAddress blas_address=rt->get_as_address(rt->device, &address_info);
    if (!blas_address) goto fail;
    VkAccelerationStructureInstanceKHR instance = {0};
    instance.transform.matrix[0][0]=instance.transform.matrix[1][1]=instance.transform.matrix[2][2]=1;
    instance.mask=0xff; instance.accelerationStructureReference=blas_address;
    if (!Buffer(rt, &f->instance, sizeof(instance), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instance) || (f->instance.address & 15u)) goto fail;
    VkAccelerationStructureGeometryInstancesDataKHR instances = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .data.deviceAddress=f->instance.address};
    VkAccelerationStructureGeometryKHR tlas_geometry = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR, .geometry.instances=instances};
    VkAccelerationStructureBuildGeometryInfoKHR tlas_info = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, .flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount=1, .pGeometries=&tlas_geometry};
    uint32_t one=1;
    VkAccelerationStructureBuildSizesInfoKHR tlas_size = {.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt->get_sizes(rt->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_info, &one, &tlas_size);
    if (!CreateAS(rt, &f->tlas_memory, tlas_size.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &f->tlas)) goto fail;
    VkDeviceSize alignment=rt->properties.minAccelerationStructureScratchOffsetAlignment;
    if (!alignment || !rt->properties.maxInstanceCount || !rt->properties.maxGeometryCount) goto fail;
    VkDeviceSize scratch_size=blas_size.buildScratchSize > tlas_size.buildScratchSize ? blas_size.buildScratchSize : tlas_size.buildScratchSize;
    if (!scratch_size || scratch_size > UINT64_MAX - (alignment-1)) goto fail;
    if (!Buffer(rt, &f->scratch, scratch_size+alignment-1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL)) goto fail;
    VkDeviceAddress rem=f->scratch.address % alignment;
    VkDeviceAddress scratch=f->scratch.address + (rem ? alignment-rem : 0);
    blas_info.dstAccelerationStructure=f->blas; blas_info.scratchData.deviceAddress=scratch;
    tlas_info.dstAccelerationStructure=f->tlas; tlas_info.scratchData.deviceAddress=scratch;
    VkAccelerationStructureBuildRangeInfoKHR blas_range = {.primitiveCount=n}, tlas_range = {.primitiveCount=1};
    const VkAccelerationStructureBuildRangeInfoKHR *range=&blas_range;
    rt->cmd_build(command, 1, &blas_info, &range);
    /* Also makes reuse of the scratch buffer safe for the TLAS build. */
    VkMemoryBarrier between = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &between, 0, NULL, 0, NULL);
    range=&tlas_range; rt->cmd_build(command, 1, &tlas_info, &range);
    VkMemoryBarrier ready = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, .dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &ready, 0, NULL, 0, NULL);
    return true;
fail:
    FreeFrame(rt, f); return false;
}
VkAccelerationStructureKHR RCSceneRT_Acceleration(const rc_scene_rt_t *rt, uint32_t frame)
{
    return rt && frame < rt->frames ? rt->frame[frame].tlas : VK_NULL_HANDLE;
}
void RCSceneRT_Destroy(rc_scene_rt_t *rt)
{
    if (!rt || !rt->device) return;
    for (uint32_t i=0; i<rt->frames; ++i) FreeFrame(rt, &rt->frame[i]);
    memset(rt, 0, sizeof(*rt));
}

/* Build static map geometry once per fenced frame slot; the dynamic BLAS and
 * small TLAS are independent. Shader primitive IDs address an identity remap
 * appended by RCSceneVk_UpdateSplit after the software BVH's indices. */
typedef struct {
    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureBuildSizesInfoKHR size;
    VkAccelerationStructureBuildRangeInfoKHR range;
} rc_split_build_t;

static bool SplitBLAS(rc_scene_rt_t *rt, rc_split_build_t *build,
    rc_scene_rt_buffer_t *vertices, rc_scene_rt_buffer_t *memory,
    VkAccelerationStructureKHR *as, const rc_scene_triangle_t *triangles, uint32_t count, bool allow_update)
{
    if (!count || count > UINT32_MAX / 3 || count > rt->properties.maxPrimitiveCount) return false;
#if SIZE_MAX <= UINT32_MAX
    if (count > SIZE_MAX / (9u * sizeof(float))) return false;
#endif
    float *data=malloc((size_t)count * 9 * sizeof(float));
    if (!data) return false;
    for (uint32_t i=0; i<count; ++i)
        for (int p=0; p<3; ++p) for (int c=0; c<3; ++c) {
            float v=triangles[i].position[p][c];
            if (!isfinite(v)) { free(data); return false; }
            data[(size_t)i*9 + p*3+c]=v;
        }
    bool ok=Buffer(rt, vertices, (VkDeviceSize)count*9*sizeof(float),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, data);
    free(data);
    if (!ok) return false;
    memset(build,0,sizeof(*build));
    build->geometry=(VkAccelerationStructureGeometryKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat=VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress=vertices->address,
            .vertexStride=3*sizeof(float), .maxVertex=count*3-1, .indexType=VK_INDEX_TYPE_NONE_KHR}};
    build->info=(VkAccelerationStructureBuildGeometryInfoKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, .flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            (allow_update ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0),
        .mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount=1, .pGeometries=&build->geometry};
    build->size.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    rt->get_sizes(rt->device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&build->info,&count,&build->size);
    if (!CreateAS(rt,memory,build->size.accelerationStructureSize,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,as)) return false;
    build->info.dstAccelerationStructure=*as;
    build->range.primitiveCount=count;
    return true;
}

static void SplitFreeTransient(rc_scene_rt_t *rt, rc_scene_rt_frame_t *f)
{
    if (f->tlas) rt->destroy_as(rt->device,f->tlas,NULL);
    if (f->blas) rt->destroy_as(rt->device,f->blas,NULL);
    f->tlas=f->blas=VK_NULL_HANDLE;
    FreeBuffer(rt,&f->scratch); FreeBuffer(rt,&f->instance); FreeBuffer(rt,&f->vertices);
    FreeBuffer(rt,&f->tlas_memory); FreeBuffer(rt,&f->blas_memory);
    f->geometry_ready=f->geometry_recorded=f->recorded_refit=false;
    f->geometry_epoch=0; f->dynamic_triangles=f->instance_count=f->refits=0; f->aligned_scratch=0;
    f->batch_count=f->scene_instance_count=0;
}
static void SplitFreeBatch(rc_scene_rt_t *rt, rc_scene_rt_frame_t *f)
{
    if (f->batch_blas) rt->destroy_as(rt->device,f->batch_blas,NULL);
    f->batch_blas=VK_NULL_HANDLE;
    FreeBuffer(rt,&f->batch_vertices); FreeBuffer(rt,&f->batch_memory);
    f->batch_ready=f->batch_recorded=false;
    f->batch_mesh_revision=f->batch_world_epoch=0; f->batch_mesh_triangles=0;
}
static void SplitFreeStatic(rc_scene_rt_t *rt, rc_scene_rt_frame_t *f)
{
    if (f->static_blas) rt->destroy_as(rt->device,f->static_blas,NULL);
    f->static_blas=VK_NULL_HANDLE;
    FreeBuffer(rt,&f->static_vertices); FreeBuffer(rt,&f->static_memory);
    f->static_epoch=0; f->static_ready=false; f->static_recorded=false; f->static_triangles=0;
}
static void SplitBarrier(VkCommandBuffer command)
{
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,0,1,&barrier,0,NULL,0,NULL);
}

/* Refit only a previously submitted, topology-compatible pair owned by this
 * fenced frame slot. Non-indexed geometry has fixed primitive topology. */
static bool SplitRefit(rc_scene_rt_t *rt, rc_scene_rt_frame_t *f,
    const rc_scene_triangle_t *triangles, uint32_t count, const rc_scene_rt_batch_t *batch, VkCommandBuffer command)
{
    size_t bytes=(size_t)count*9*sizeof(float);
    float *data=bytes ? malloc(bytes) : NULL;
    if (bytes && !data) return false;
    for (uint32_t i=0; i<count; ++i)
        for (int p=0; p<3; ++p) for (int c=0; c<3; ++c) {
            float v=triangles[i].position[p][c];
            if (!isfinite(v)) { free(data); return false; }
            data[(size_t)i*9+p*3+c]=v;
        }
    void *mapped=NULL;
    if (bytes) {
        if (vkMapMemory(rt->device,f->vertices.memory,0,bytes,0,&mapped)!=VK_SUCCESS) {
            free(data); return false;
        }
        memcpy(mapped,data,bytes); vkUnmapMemory(rt->device,f->vertices.memory); free(data);
    }
    if (batch && batch->count) {
        VkDeviceSize size=(VkDeviceSize)f->instance_count*sizeof(VkAccelerationStructureInstanceKHR);
        if (vkMapMemory(rt->device,f->instance.memory,0,size,0,&mapped)!=VK_SUCCESS) return false;
        VkAccelerationStructureInstanceKHR *instances=mapped;
        for (uint32_t i=0;i<batch->count;++i)
            for (unsigned a=0;a<3;++a) instances[f->scene_instance_count+i].transform.matrix[a][3]=batch->origins[i][a];
        vkUnmapMemory(rt->device,f->instance.memory);
    }
    VkAccelerationStructureGeometryKHR bg={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat=VK_FORMAT_R32G32B32_SFLOAT,.vertexData.deviceAddress=f->vertices.address,
            .vertexStride=3*sizeof(float),.maxVertex=count ? count*3-1 : 0,.indexType=VK_INDEX_TYPE_NONE_KHR}};
    VkAccelerationStructureBuildGeometryInfoKHR bi={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,.srcAccelerationStructure=f->blas,.dstAccelerationStructure=f->blas,
        .geometryCount=1,.pGeometries=&bg,.scratchData.deviceAddress=f->aligned_scratch};
    VkAccelerationStructureGeometryKHR tg={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .data.deviceAddress=f->instance.address}};
    VkAccelerationStructureBuildGeometryInfoKHR ti={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,.srcAccelerationStructure=f->tlas,.dstAccelerationStructure=f->tlas,
        .geometryCount=1,.pGeometries=&tg,.scratchData.deviceAddress=f->aligned_scratch};
    VkMemoryBarrier previous={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        0,1,&previous,0,NULL,0,NULL);
    VkAccelerationStructureBuildRangeInfoKHR br={.primitiveCount=count},tr={.primitiveCount=f->instance_count};
    const VkAccelerationStructureBuildRangeInfoKHR *range=&br;
    if (count) { rt->cmd_build(command,1,&bi,&range); SplitBarrier(command); }
    range=&tr; rt->cmd_build(command,1,&ti,&range);
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);
    f->dynamic_upload_bytes=bytes; f->dynamic_updates=count ? 1 : 0; f->tlas_updates=1;
    f->geometry_recorded=true; f->recorded_refit=true;
    return true;
}

bool RCSceneRT_BuildSplitBatch(rc_scene_rt_t *rt, uint32_t frame, const rc_scene_t *scene,
    uint32_t static_count, uint64_t world_epoch, const rc_scene_rt_batch_t *batch, VkCommandBuffer command)
{
    if (!rt || !rt->device || frame>=rt->frames || !scene || !command || !world_epoch ||
        !scene->triangles || scene->triangle_count!=scene->index_count ||
        static_count>scene->triangle_count || !scene->triangle_count ||
        (uint64_t)scene->index_count+static_count>=0xffffffu) return false;
    uint32_t batch_count=batch ? batch->count : 0;
    if (batch_count) {
        if (!batch->triangles || !batch->triangle_count || !batch->origins || !batch->mesh_revision ||
            (uint64_t)batch_count+2u>UINT32_MAX || (uint64_t)batch_count+2u>rt->properties.maxInstanceCount) return false;
        for (uint32_t i=0;i<batch->triangle_count;++i)
            if (batch->triangles[i].metadata[1] & (RC_SCENE_CUTOUT|RC_SCENE_LIQUID|RC_SCENE_SKY)) return false;
        for (uint32_t i=0;i<batch_count;++i)
            for (unsigned a=0;a<3;++a) if (!isfinite(batch->origins[i][a])) return false;
    }
    rc_scene_rt_frame_t *f=&rt->frame[frame];
    f->static_upload_bytes=f->dynamic_upload_bytes=0;
    f->static_builds=f->dynamic_builds=f->tlas_builds=0;
    f->dynamic_updates=f->tlas_updates=0;
    uint32_t dynamic_count=scene->triangle_count-static_count;
    bool batch_match=!batch_count || (f->batch_ready && f->batch_blas &&
        f->batch_world_epoch==world_epoch && f->batch_mesh_revision==batch->mesh_revision &&
        f->batch_mesh_triangles==batch->triangle_count);
    if ((dynamic_count || batch_count) && f->geometry_ready && !f->geometry_recorded && batch_match &&
        f->geometry_epoch==world_epoch && f->static_triangles==static_count &&
        f->dynamic_triangles==dynamic_count && f->batch_count==batch_count && f->refits<32 && f->tlas &&
        (!dynamic_count || (f->blas && f->vertices.memory)) && f->instance.address && f->aligned_scratch &&
        (!static_count || (f->static_ready && f->static_epoch==world_epoch)))
        return SplitRefit(rt,f,scene->triangles+static_count,dynamic_count,batch,command);
    SplitFreeTransient(rt,f);
    bool rebuild_static=static_count && (!f->static_ready || f->static_epoch!=world_epoch || f->static_triangles!=static_count);
    if (!static_count || f->static_epoch!=world_epoch || f->static_triangles!=static_count || (!f->static_ready && f->static_blas)) SplitFreeStatic(rt,f);
    bool rebuild_batch=batch_count && !batch_match;
    if (!batch_count || rebuild_batch) SplitFreeBatch(rt,f);
    rc_split_build_t sb={0},db={0},bb={0};
    VkAccelerationStructureInstanceKHR *instances=NULL;
    if (rebuild_static && !SplitBLAS(rt,&sb,&f->static_vertices,&f->static_memory,&f->static_blas,scene->triangles,static_count,false)) goto fail;
    if (dynamic_count && !SplitBLAS(rt,&db,&f->vertices,&f->blas_memory,&f->blas,scene->triangles+static_count,dynamic_count,true)) goto fail;
    if (rebuild_batch && !SplitBLAS(rt,&bb,&f->batch_vertices,&f->batch_memory,&f->batch_blas,batch->triangles,batch->triangle_count,false)) goto fail;
    instances=calloc((size_t)batch_count+2u,sizeof(*instances));
    if (!instances) goto fail;
    uint32_t instance_count=0;
    VkAccelerationStructureKHR sources[2]={f->static_blas,f->blas};
    for (unsigned i=0;i<2;++i) if (sources[i]) {
        VkAccelerationStructureDeviceAddressInfoKHR ai={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,.accelerationStructure=sources[i]};
        VkDeviceAddress address=rt->get_as_address(rt->device,&ai);
        if (!address) goto fail;
        VkAccelerationStructureInstanceKHR *inst=&instances[instance_count++];
        inst->transform.matrix[0][0]=inst->transform.matrix[1][1]=inst->transform.matrix[2][2]=1;
        inst->mask=1; inst->instanceCustomIndex=scene->index_count+(i ? static_count : 0);
        inst->accelerationStructureReference=address;
    }
    uint32_t scene_instances=instance_count;
    if (batch_count) {
        VkAccelerationStructureDeviceAddressInfoKHR ai={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,.accelerationStructure=f->batch_blas};
        VkDeviceAddress address=rt->get_as_address(rt->device,&ai);
        if (!address) goto fail;
        for (uint32_t i=0;i<batch_count;++i) {
            VkAccelerationStructureInstanceKHR *inst=&instances[instance_count++];
            inst->transform.matrix[0][0]=inst->transform.matrix[1][1]=inst->transform.matrix[2][2]=1;
            for (unsigned a=0;a<3;++a) inst->transform.matrix[a][3]=batch->origins[i][a];
            inst->mask=2; inst->instanceCustomIndex=0xffffffu;
            inst->accelerationStructureReference=address;
        }
    }
    if (!instance_count || instance_count>rt->properties.maxInstanceCount || !rt->properties.maxGeometryCount ||
        !Buffer(rt,&f->instance,(VkDeviceSize)instance_count*sizeof(instances[0]),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,instances) || (f->instance.address & 15u)) goto fail;
    free(instances); instances=NULL;
    VkAccelerationStructureGeometryKHR geom={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR,.geometry.instances={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .data.deviceAddress=f->instance.address}};
    VkAccelerationStructureBuildGeometryInfoKHR info={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,.geometryCount=1,.pGeometries=&geom};
    VkAccelerationStructureBuildSizesInfoKHR size={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt->get_sizes(rt->device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&info,&instance_count,&size);
    if (!CreateAS(rt,&f->tlas_memory,size.accelerationStructureSize,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,&f->tlas)) goto fail;
    VkDeviceSize alignment=rt->properties.minAccelerationStructureScratchOffsetAlignment;
    VkDeviceSize scratch_size=size.buildScratchSize;
    if (sb.size.buildScratchSize>scratch_size) scratch_size=sb.size.buildScratchSize;
    if (db.size.buildScratchSize>scratch_size) scratch_size=db.size.buildScratchSize;
    if (bb.size.buildScratchSize>scratch_size) scratch_size=bb.size.buildScratchSize;
    if (db.size.updateScratchSize>scratch_size) scratch_size=db.size.updateScratchSize;
    if (size.updateScratchSize>scratch_size) scratch_size=size.updateScratchSize;
    if (!alignment || !scratch_size || scratch_size>UINT64_MAX-(alignment-1) ||
        !Buffer(rt,&f->scratch,scratch_size+alignment-1,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,NULL)) goto fail;
    VkDeviceAddress rem=f->scratch.address%alignment;
    VkDeviceAddress scratch=f->scratch.address+(rem ? alignment-rem : 0);
    /* No failure paths after the first recorded command. */
    if (rebuild_static) {
        sb.info.scratchData.deviceAddress=scratch;
        const VkAccelerationStructureBuildRangeInfoKHR *range=&sb.range;
        rt->cmd_build(command,1,&sb.info,&range); SplitBarrier(command);
        f->static_epoch=world_epoch; f->static_recorded=true;
        f->static_upload_bytes=(uint64_t)static_count*9*sizeof(float); f->static_builds=1;
    }
    if (dynamic_count) {
        db.info.scratchData.deviceAddress=scratch;
        const VkAccelerationStructureBuildRangeInfoKHR *range=&db.range;
        rt->cmd_build(command,1,&db.info,&range); SplitBarrier(command);
        f->dynamic_upload_bytes=(uint64_t)dynamic_count*9*sizeof(float); f->dynamic_builds=1;
    }
    if (rebuild_batch) {
        bb.info.scratchData.deviceAddress=scratch;
        const VkAccelerationStructureBuildRangeInfoKHR *batch_range=&bb.range;
        rt->cmd_build(command,1,&bb.info,&batch_range); SplitBarrier(command);
        f->batch_mesh_revision=batch->mesh_revision; f->batch_world_epoch=world_epoch;
        f->batch_mesh_triangles=batch->triangle_count; f->batch_recorded=true;
    }
    info.dstAccelerationStructure=f->tlas; info.scratchData.deviceAddress=scratch;
    VkAccelerationStructureBuildRangeInfoKHR range={.primitiveCount=instance_count};
    const VkAccelerationStructureBuildRangeInfoKHR *p=&range;
    rt->cmd_build(command,1,&info,&p);
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);
    f->tlas_builds=1;
    f->geometry_epoch=world_epoch; f->static_triangles=static_count; f->dynamic_triangles=dynamic_count;
    f->instance_count=instance_count; f->scene_instance_count=scene_instances; f->batch_count=batch_count;
    f->aligned_scratch=scratch; f->geometry_recorded=true; f->recorded_refit=false;
    return true;
fail:
    free(instances);
    SplitFreeTransient(rt,f);
    if (rebuild_static) SplitFreeStatic(rt,f);
    if (rebuild_batch) SplitFreeBatch(rt,f);
    return false;
}
bool RCSceneRT_BuildSplit(rc_scene_rt_t *rt, uint32_t frame, const rc_scene_t *scene,
    uint32_t static_count, uint64_t world_epoch, VkCommandBuffer command)
{
    return RCSceneRT_BuildSplitBatch(rt,frame,scene,static_count,world_epoch,NULL,command);
}
void RCSceneRT_Submitted(rc_scene_rt_t *rt, uint32_t frame)
{
    if (!rt || frame>=rt->frames) return;
    rc_scene_rt_frame_t *f=&rt->frame[frame];
    if (f->static_recorded) { f->static_recorded=false; f->static_ready=true; }
    if (f->batch_recorded) { f->batch_recorded=false; f->batch_ready=true; }
    if (f->geometry_recorded) {
        f->geometry_recorded=false; f->geometry_ready=true;
        f->refits=f->recorded_refit ? f->refits+1 : 0; f->recorded_refit=false;
    }
}
void RCSceneRT_Discard(rc_scene_rt_t *rt, uint32_t frame)
{
    if (!rt || frame>=rt->frames) return;
    rc_scene_rt_frame_t *f=&rt->frame[frame];
    if (f->static_recorded) { f->static_recorded=false; f->static_ready=false; }
    if (f->batch_recorded) { f->batch_recorded=false; f->batch_ready=false; }
    if (f->geometry_recorded) { f->geometry_recorded=false; f->geometry_ready=false; f->recorded_refit=false; }
}
