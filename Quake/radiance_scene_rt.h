/* Optional ray-query acceleration structures for world-space radiance scenes.
 * A caller must enable buffer device addresses, acceleration structures and
 * ray queries on the device before calling Create. Each frame slot is owned
 * until its submitted work completes: wait for its fence before rebuilding or
 * destroying it, and submit a successfully recorded build before tracing.
 * Scene GPU descriptors and this build must use the same scene snapshot.
 * Build only records commands; its returned handle becomes usable on the
 * GPU only after successful submission. If recording is discarded or queue
 * submission fails, never use that slot's handle: rebuild it before use, or
 * destroy it after resolving any pending device work. */
#ifndef QUIVER_RADIANCE_SCENE_RT_H
#define QUIVER_RADIANCE_SCENE_RT_H
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "radiance_scene.h"

#define RC_SCENE_RT_MAX_FRAMES 2u

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    VkDeviceSize allocation_size;
} rc_scene_rt_buffer_t;

typedef struct {
    rc_scene_rt_buffer_t vertices, instance, blas_memory, tlas_memory, scratch;
    VkAccelerationStructureKHR blas, tlas;
    rc_scene_rt_buffer_t static_vertices, static_memory;
    VkAccelerationStructureKHR static_blas;
    rc_scene_rt_buffer_t batch_vertices, batch_memory;
    VkAccelerationStructureKHR batch_blas;
    uint64_t batch_mesh_revision, batch_world_epoch;
    uint32_t batch_mesh_triangles, batch_count, scene_instance_count;
    bool batch_ready, batch_recorded;
    uint64_t static_epoch;
    bool static_ready, static_recorded;
    uint64_t static_upload_bytes, dynamic_upload_bytes;
    uint32_t static_builds, dynamic_builds, tlas_builds;
    uint32_t dynamic_updates, tlas_updates;
    uint64_t geometry_epoch;
    uint32_t static_triangles, dynamic_triangles, instance_count, refits;
    VkDeviceAddress aligned_scratch;
    bool geometry_ready, geometry_recorded, recorded_refit;
} rc_scene_rt_frame_t;

typedef struct {
    VkDevice device;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR properties;
    PFN_vkGetBufferDeviceAddress get_buffer_address;
    PFN_vkCreateAccelerationStructureKHR create_as;
    PFN_vkDestroyAccelerationStructureKHR destroy_as;
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes;
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_as_address;
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build;
    uint32_t frames;
    rc_scene_rt_frame_t frame[RC_SCENE_RT_MAX_FRAMES];
} rc_scene_rt_t;

bool RCSceneRT_Create(rc_scene_rt_t *rt, VkPhysicalDevice physical, VkDevice device, uint32_t frames);
/* Records build and barriers. False leaves no usable TLAS for this slot; use
 * software tracing. Empty scenes are deliberately unsupported. */
bool RCSceneRT_Build(rc_scene_rt_t *rt, uint32_t frame, const rc_scene_t *scene, VkCommandBuffer command);
bool RCSceneRT_BuildSplit(rc_scene_rt_t *rt, uint32_t frame, const rc_scene_t *scene,
    uint32_t static_count, uint64_t world_epoch, VkCommandBuffer command);
bool RCSceneRT_BuildSplitBatch(rc_scene_rt_t *rt, uint32_t frame, const rc_scene_t *scene,
    uint32_t static_count, uint64_t world_epoch, const rc_scene_rt_batch_t *batch, VkCommandBuffer command);
void RCSceneRT_Submitted(rc_scene_rt_t *rt, uint32_t frame);
void RCSceneRT_Discard(rc_scene_rt_t *rt, uint32_t frame);
VkAccelerationStructureKHR RCSceneRT_Acceleration(const rc_scene_rt_t *rt, uint32_t frame);
void RCSceneRT_Destroy(rc_scene_rt_t *rt);
#endif
