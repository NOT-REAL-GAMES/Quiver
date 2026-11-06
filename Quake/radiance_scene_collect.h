/* Opt-in collection/orchestration for radiance scene snapshots. Include
 * quakedef.h first. This does not enable the renderer or execute a frame. */
#ifndef QUIVER_RADIANCE_SCENE_COLLECT_H
#define QUIVER_RADIANCE_SCENE_COLLECT_H
#include "radiance_scene_runtime.h"
#include "radiance_materials_quake.h"
#include "radiance_scene_vulkan.h"
#include "radiance_scene_rt.h"
#include "radiance_gpu_inputs.h"
#include "radiance_emissive.h"

typedef struct {
    rc_scene_runtime_t runtime;
    radiance_quake_materials_t materials;
    radiance_material_atlas_t atlas;
    rc_scene_vk_t scene_gpu;
    rc_scene_rt_t rt;
    radiance_gpu_inputs_t inputs;
    radiance_emissive_cache_t *emissive_cache;
    const radiance_surface_template_t *surface_templates;
    size_t surface_template_count, surface_mask_count;
    uint8_t *surface_material_mask;
    uint64_t surface_mask_revision;
    VkDevice device;
    uint32_t frames, max_dimension, max_layers;
    size_t max_atlas_bytes, compacted_material_count;
    /* atlas_revision follows active material selection; atlas_upload_generation
     * follows packed pixels so animation does not upload unchanged images. */
    uint64_t generation, atlas_revision, material_source_revision, atlas_upload_generation, world_epoch, slot_generation[2], pending_generation[2];
    uint64_t geometry_generation, slot_geometry_generation[2], pending_geometry_generation[2];
    uint64_t base_geometry_generation, base_content_generation;
    bool atlas_valid, rt_available, scene_prepared, materials_need_compaction, pending[2], ready[2], used_rt[2], split_scene[2];
    rc_scene_runtime_stats_t stats;
    radiance_emissive_stats_t emissive_stats;
    const char *last_error, *last_warning; /* static diagnostic strings; no logging */
} rc_scene_collect_t;

typedef struct {
    VkDescriptorSet scene, lighting;
    bool hardware;
    rc_scene_runtime_stats_t stats;
    uint32_t triangles, nodes, materials, ecs_shadow_count;
} rc_scene_collect_result_t;

/* Device must already enable BDA, acceleration structures and ray queries if
 * hardware_supported is true. Hardware initialization failure keeps software.
 * max_atlas_bytes bounds atlas and decoded-source data separately; it is not
 * a bound on simultaneous CPU plus GPU memory. */
bool RCCollect_Create(rc_scene_collect_t *c, VkPhysicalDevice physical, VkDevice device,
    const VkPhysicalDeviceMemoryProperties *memory, const VkPhysicalDeviceLimits *limits,
    uint32_t frames, bool hardware_supported, size_t max_atlas_bytes);
/* Requires no unresolved recordings. Clear before old map texture/model memory
 * becomes invalid. Existing submitted GPU frames must remain alive until their
 * fences complete; the next record for each slot verifies its fence. */
bool RCCollect_SetWorld(rc_scene_collect_t *c, const qmodel_t *world);
/* Templates are coordinator-owned and valid until the next map change. */
void RCCollect_SetSurfaceTemplates(rc_scene_collect_t *c, const radiance_surface_template_t *templates, size_t count);
/* Resolve current geometry/materials and generate bounded emissive proxies
 * before the light coordinator builds its spatial grid. CPU-only; serialized
 * with map/texture mutation and other collector calls. The proxy array is
 * caller-owned. For discarded/failed frames prepare the next scene again. */
bool RCCollect_PrepareScene(rc_scene_collect_t *c, entity_t *const *candidates, uint32_t candidate_count,
    const float camera[3], uint64_t revision, radiance_light_t *emitters, size_t emitter_capacity,
    radiance_emissive_stats_t *emissive_stats);
/* Record a scene prepared by RCCollect_PrepareScene, using a light snapshot
 * which already includes the returned proxies. Same fence/submit rules as
 * RCCollect_Record. */
bool RCCollect_RecordPrepared(rc_scene_collect_t *c, uint32_t frame, VkFence fence, VkCommandBuffer command,
    const radiance_input_snapshot_t *lighting, bool request_hardware, rc_scene_collect_result_t *result);
/* Optional opaque translation-only batch is additional hardware local-shadow
 * coverage; material/GI and software tracing retain ordinary selected entities. */
bool RCCollect_RecordPreparedBatch(rc_scene_collect_t *c, uint32_t frame, VkFence fence, VkCommandBuffer command,
    const radiance_input_snapshot_t *lighting, bool request_hardware, const rc_scene_rt_batch_t *batch,
    rc_scene_collect_result_t *result);
/* Record into an already recording primary command buffer. Fence must be the
 * slot's most recent submission fence and signal SUCCESS (call before resetting
 * it); first-use fences must be created signaled. Caller supplies actual runtime
 * light/grid data; material fields are replaced with this scene's real atlas.
 * Increment revision whenever interpolation, pose or material selection changes.
 * Frame pointers and source textures must remain valid during the call.
 * On false, abandon/reset the whole command buffer and select classic lighting;
 * never publish stale prior output. Call Submitted after successful queue submit,
 * or Discard after resetting any abandoned command buffer. A successful result
 * may be referenced while recording commands after this call. */
bool RCCollect_Record(rc_scene_collect_t *c, uint32_t frame, VkFence fence, VkCommandBuffer command,
    entity_t *const *candidates, uint32_t candidate_count, const float camera[3], uint64_t revision,
    const radiance_input_snapshot_t *lighting, bool request_hardware, rc_scene_collect_result_t *result);
void RCCollect_Submitted(rc_scene_collect_t *c, uint32_t frame);
void RCCollect_Discard(rc_scene_collect_t *c, uint32_t frame);
/* Diagnostic reason from most recent SetWorld/Record call, or NULL on
 * success. A warning may report hardware-to-software fallback on success. */
const char *RCCollect_LastError(const rc_scene_collect_t *c);
const char *RCCollect_LastWarning(const rc_scene_collect_t *c);
/* All GPU work must be complete and recordings submitted/discarded first. */
void RCCollect_Destroy(rc_scene_collect_t *c);
#endif
