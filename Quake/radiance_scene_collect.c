#include "quakedef.h"
#include "radiance_scene_collect.h"
#include "radiance_scene_alias.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool RCCollect_TextureEqual(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)) { ++a; ++b; }
    return *a==*b;
}

void RCCollect_SetSurfaceTemplates(rc_scene_collect_t *c, const radiance_surface_template_t *templates, size_t count)
{
    if (!c) return;
    c->surface_templates=templates; c->surface_template_count=templates ? count : 0;
    free(c->surface_material_mask); c->surface_material_mask=NULL; c->surface_mask_count=0; c->surface_mask_revision=0;
}

static bool RCCollect_Alias(rc_scene_t *scene, const entity_t *entity, const float matrix[16],
    uint32_t instance, rc_scene_material_fn material, void *user)
{
    (void)matrix; (void)material;
    /* The lerped helper recomputes the renderer's actual interpolated matrix. */
    return RCScene_AppendAliasLerped(scene, entity, instance, RadianceQuakeMaterials_Alias, user);
}

static bool RCCollect_AliasSupported(const entity_t *entity, void *user)
{
    (void)user;
    return RCScene_AliasSupported(entity);
}

static uint64_t RCCollect_AliasRevision(const entity_t *entity, void *user)
{
    (void)user;
    return RCScene_AliasRevision(entity);
}

static bool RCCollect_AliasPersistent(const entity_t *entity, void *user)
{
    (void)user;
    return RCScene_AliasPersistent(entity);
}

static void RCCollect_BrushContext(const entity_t *entity, void *user)
{
    RadianceQuakeMaterials_SetBrushFrame(user, entity ? entity->frame : 0);
}

void RCCollect_Destroy(rc_scene_collect_t *c)
{
    if (!c) return;
    free(c->surface_material_mask);
    Radiance_EmissiveCacheDestroy(c->emissive_cache);
    RadianceInputs_Destroy(&c->inputs);
    RCSceneVk_Destroy(&c->scene_gpu);
    RCSceneRT_Destroy(&c->rt);
    RadianceMaterialAtlas_Clear(&c->atlas);
    RadianceQuakeMaterials_Clear(&c->materials);
    RCSceneRuntime_Clear(&c->runtime);
    memset(c, 0, sizeof(*c));
}

bool RCCollect_Create(rc_scene_collect_t *c, VkPhysicalDevice physical, VkDevice device,
    const VkPhysicalDeviceMemoryProperties *memory, const VkPhysicalDeviceLimits *limits,
    uint32_t frames, bool hardware_supported, size_t max_atlas_bytes)
{
    if (!c) return false;
    memset(c, 0, sizeof(*c));
    if (!physical || !device || !memory || !limits || !frames || frames > 2 || !max_atlas_bytes) return false;
    c->device=device; c->frames=frames; c->max_dimension=limits->maxImageDimension2D;
    c->max_layers=limits->maxImageArrayLayers; c->max_atlas_bytes=max_atlas_bytes;
    RCSceneRuntime_Init(&c->runtime); RadianceQuakeMaterials_Init(&c->materials);
    c->rt_available=hardware_supported && RCSceneRT_Create(&c->rt, physical, device, frames);
    if (!(c->emissive_cache=Radiance_EmissiveCacheCreate()) ||
        !RCSceneVk_Create(&c->scene_gpu, device, memory, limits, frames, c->rt_available) ||
        !RadianceInputs_Create(&c->inputs, physical, device, memory, limits, frames)) {
        RCCollect_Destroy(c); return false;
    }
    if (!c->scene_gpu.hardware_layout) c->rt_available=false;
    return true;
}

/* Projectiles appear mid-game. Their skins must already own atlas rectangles
 * and stable IDs, or each first shot repacks the whole atlas and refreshes
 * every probe. Skins of models precached later are still handled on first use. */
static bool RCCollect_PinPrecachedSkins(radiance_quake_materials_t *materials)
{
    for (int i=1; i<MAX_MODELS && cl.model_precache[i]; ++i) {
        qmodel_t *model=cl.model_precache[i];
        if (model->type != mod_alias) continue;
        for (const aliashdr_t *surface=Mod_Extradata_CheckSkin(model,0); surface; surface=surface->nextsurface)
            for (int skin=0; skin<surface->numskins; ++skin)
                for (int animation=0; animation<MAX_FRAMEGROUPS; ++animation) {
                    const gltexture_t *base=surface->gltextures[skin][animation];
                    if (base && !RadianceQuakeMaterials_Pin(materials,base,surface->fbtextures[skin][animation])) return false;
                }
    }
    return true;
}

bool RCCollect_SetWorld(rc_scene_collect_t *c, const qmodel_t *world)
{
    if (!c) return false;
    c->last_error=NULL; c->last_warning=NULL;
    if (!c->device || !world) { c->last_error="invalid world or device"; return false; }
    for (uint32_t i=0; i<c->frames; ++i) if (c->pending[i]) { c->last_error="world change has unresolved recording"; return false; }
    rc_scene_runtime_t runtime; radiance_quake_materials_t materials;
    RCSceneRuntime_Init(&runtime); RadianceQuakeMaterials_Init(&materials);
    RCSceneRuntime_SetBrushContext(&runtime, RCCollect_BrushContext);
    RCSceneRuntime_SetAliasSupport(&runtime, RCCollect_AliasSupported);
    RCSceneRuntime_SetAliasRevision(&runtime, RCCollect_AliasRevision);
    RCSceneRuntime_SetAliasPersistent(&runtime, RCCollect_AliasPersistent);
    if (!RCSceneRuntime_SetWorld(&runtime, world, RadianceQuakeMaterials_Brush, &materials) ||
        !RCScene_AppendCompiledWorld(&runtime.static_scene,world,RadianceQuakeMaterials_Alias,&materials) ||
        !RCCollect_PinPrecachedSkins(&materials)) {
        RCSceneRuntime_Clear(&runtime); RadianceQuakeMaterials_Clear(&materials);
        c->last_error="world geometry or material extraction failed"; return false;
    }
    RCSceneRuntime_Clear(&c->runtime); RadianceQuakeMaterials_Clear(&c->materials);
    RadianceMaterialAtlas_Clear(&c->atlas);
    Radiance_EmissiveCacheReset(c->emissive_cache);
    RCCollect_SetSurfaceTemplates(c,NULL,0);
    c->runtime=runtime; c->materials=materials; c->atlas_valid=false; c->atlas_revision=0;
    c->material_source_revision=0; c->scene_prepared=false;
    c->compacted_material_count=materials.count; c->materials_need_compaction=false;
    if (++c->world_epoch == 0) c->world_epoch=1;
    c->generation=0;
    c->geometry_generation=0;
    c->base_geometry_generation=c->base_content_generation=0;
    memset(c->slot_generation, 0, sizeof(c->slot_generation));
    memset(c->pending_generation, 0, sizeof(c->pending_generation));
    memset(c->slot_geometry_generation, 0, sizeof(c->slot_geometry_generation));
    memset(c->pending_geometry_generation, 0, sizeof(c->pending_geometry_generation));
    memset(c->ready, 0, sizeof(c->ready));
    memset(c->used_rt, 0, sizeof(c->used_rt));
    memset(c->split_scene, 0, sizeof(c->split_scene));
    memset(&c->stats, 0, sizeof(c->stats));
    return true;
}

bool RCCollect_PrepareScene(rc_scene_collect_t *c, entity_t *const *candidates, uint32_t candidate_count,
    const float camera[3], uint64_t revision, radiance_light_t *emitters, size_t emitter_capacity,
    radiance_emissive_stats_t *emissive_stats)
{
    if (emissive_stats) memset(emissive_stats,0,sizeof(*emissive_stats));
    if (!c) return false;
    c->last_error=NULL; c->last_warning=NULL;
    c->scene_prepared=false;
    if (!c->device || !camera || (candidate_count && !candidates) || (emitter_capacity && !emitters)) {
        c->last_error="invalid scene preparation arguments"; return false;
    }
    rc_scene_runtime_stats_t stats={0};
    if (!RCSceneRuntime_Update(&c->runtime, candidates, candidate_count, camera, revision,
        RadianceQuakeMaterials_Brush, RCCollect_Alias, &c->materials, &stats)) {
        c->materials_need_compaction=true; c->last_error="dynamic scene extraction failed"; return false;
    }
    c->stats=stats;
    if (stats.rebuilt || stats.refit) {
        if (++c->generation == 0) { c->generation=1; memset(c->slot_generation, 0, sizeof(c->slot_generation)); }
    }
    if (stats.geometry_changed) {
        if (++c->geometry_generation == 0) { c->geometry_generation=1; memset(c->slot_geometry_generation,0,sizeof(c->slot_geometry_generation)); }
    }
    if (!c->generation) { c->last_error="scene has no initialized generation"; return false; }
    /* Discard material sources belonging only to entities that left the
     * selected scene before examining texture pointers. Their texture slots
     * can have been freed or recycled, and keeping them also grows the atlas
     * without bound as models/skins change. Pose-only updates skip this scan. */
    c->materials_need_compaction |= stats.materials_changed || c->materials.count != c->compacted_material_count;
    if (c->materials_need_compaction) {
        bool compacted=false;
        if (!RCSceneRuntime_CompactMaterials(&c->runtime, &c->materials, &compacted)) {
            c->last_error="material registry compaction failed"; return false;
        }
        c->compacted_material_count=c->materials.count; c->materials_need_compaction=false;
        /* Logical brush identity can change while its current source image
         * stays shared. Template matching must not retain the old brush. */
        free(c->surface_material_mask); c->surface_material_mask=NULL;
        c->surface_mask_count=0; c->surface_mask_revision=0;
        if (compacted) {
            if (++c->generation == 0) { c->generation=1; memset(c->slot_generation,0,sizeof(c->slot_generation)); }
        }
    }
    uint64_t base_geometry=RCSceneRuntime_BaseGeneration(&c->runtime);
    uint64_t base_content=RCSceneRuntime_BaseContentGeneration(&c->runtime);
    if (c->base_geometry_generation != base_geometry) {
        c->base_geometry_generation=base_geometry;
        if (++c->world_epoch == 0) c->world_epoch=1;
    }
    c->stats.base_content_changed |= c->base_content_generation != base_content;
    c->base_content_generation=base_content;
    if (c->stats.base_content_changed) Radiance_EmissiveCacheReset(c->emissive_cache);
    uint64_t material_revision=RadianceQuakeMaterials_Revision(&c->materials);
    bool pixels_changed=false;
    /* The inventory check includes inactive animation frames and reloads.
     * Logical frame selection can update only material rectangles; preserve
     * the GPU pixel identity when its packed inventory is unchanged. */
    if (c->atlas_failed && c->atlas_failed_revision==material_revision) { c->last_error="material atlas build failed"; return false; }
    const double atlas_start=Sys_DoubleTime();
    bool atlas_ready=RadianceQuakeMaterials_UpdateAtlas(&c->materials, &c->atlas, c->max_dimension, c->max_layers,
        c->max_atlas_bytes, &pixels_changed);
    /* Pins are an optimization: an inventory they overflow must still render. */
    if (!atlas_ready && RadianceQuakeMaterials_ClearPins(&c->materials)) {
        Con_Printf("Radiance: material atlas could not include precached skins; first use may hitch\n");
        bool compacted=false;
        if (RCSceneRuntime_CompactMaterials(&c->runtime,&c->materials,&compacted)) {
            c->compacted_material_count=c->materials.count;
            if (compacted && ++c->generation == 0) { c->generation=1; memset(c->slot_generation,0,sizeof(c->slot_generation)); }
            material_revision=RadianceQuakeMaterials_Revision(&c->materials);
            atlas_ready=RadianceQuakeMaterials_UpdateAtlas(&c->materials, &c->atlas, c->max_dimension, c->max_layers,
                c->max_atlas_bytes, &pixels_changed);
        }
    }
    if (!atlas_ready) {
        Con_Printf("Radiance: material atlas build failed for %zu materials\n",c->materials.count);
        c->atlas_failed=true; c->atlas_failed_revision=material_revision;
        c->last_error="material atlas build failed"; return false;
    }
    c->atlas_failed=false;
    if (pixels_changed) { ++c->atlas_repacks; c->atlas_repack_ms=(Sys_DoubleTime()-atlas_start)*1000.0; }
    if (!c->atlas_valid || pixels_changed) {
        if (++c->atlas_upload_generation == 0) {
            c->atlas_upload_generation=1;
            for (uint32_t i=0; i<c->frames; ++i) c->inputs.frame[i].atlas_revision=0;
        }
    }
    /* A repacked inventory also invalidates material/emitter interpretations
     * even when the current source signatures happen to remain unchanged. */
    if (!c->atlas_valid || pixels_changed || c->material_source_revision != material_revision)
        if (++c->atlas_revision == 0) c->atlas_revision=1;
    c->material_source_revision=material_revision; c->atlas_valid=true;
    if (c->atlas.material_count > UINT32_MAX) { c->last_error="material count overflow"; return false; }
    if (c->surface_template_count && (!c->surface_material_mask || c->surface_mask_count != c->materials.count ||
        c->surface_mask_revision != c->atlas_revision)) {
        uint8_t *mask=calloc(c->materials.count ? c->materials.count : 1,1);
        if (!mask) { c->last_error="surface light material allocation failed"; return false; }
        for (size_t i=0; i<c->materials.count; ++i) {
            const radiance_quake_material_t *m=&c->materials.entries[i];
            if (!m->brush || m->sky) continue;
            for (size_t j=0; j<c->surface_template_count; ++j)
                if (RCCollect_TextureEqual(m->brush->name,c->surface_templates[j].texture)) { mask[i]=1; break; }
        }
        free(c->surface_material_mask); c->surface_material_mask=mask;
        c->surface_mask_count=c->materials.count; c->surface_mask_revision=c->atlas_revision;
    }
    bool flags_changed=false;
    if (!Radiance_BuildEmissiveLightsCachedFiltered(c->emissive_cache,&c->runtime.scene,RCSceneRuntime_BaseTriangleCount(&c->runtime),
        &c->atlas,c->atlas_revision,c->stats.base_content_changed,stats.rebuilt || stats.refit,
        c->surface_material_mask,camera,emitters,emitter_capacity,&c->emissive_stats,&flags_changed)) {
        c->last_error="emissive surface extraction failed"; return false;
    }
    if (flags_changed && ++c->generation == 0) { c->generation=1; memset(c->slot_generation,0,sizeof(c->slot_generation)); }
    if (emissive_stats) *emissive_stats=c->emissive_stats;
    c->scene_prepared=true; return true;
}

bool RCCollect_RecordPreparedBatch(rc_scene_collect_t *c, uint32_t frame, VkFence fence, VkCommandBuffer command,
    const radiance_input_snapshot_t *lighting, bool request_hardware, const rc_scene_rt_batch_t *batch, rc_scene_collect_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));
    if (!c) return false;
    c->last_error=NULL; c->last_warning=NULL;
    if (!c->device || frame >= c->frames || !fence || !command || !lighting || !result || !c->scene_prepared) {
        c->last_error="invalid collector recording arguments or unprepared scene"; return false;
    }
    if (c->pending[frame]) { c->last_error="frame has unresolved recording"; return false; }
    VkResult fence_status=vkGetFenceStatus(c->device, fence);
    if (fence_status != VK_SUCCESS) {
        c->last_error=fence_status == VK_NOT_READY ? "frame fence not signaled" : "frame fence query failed"; return false;
    }
    /* Abandoned hardware work requires a fresh upload/build on retry. */
    uint64_t committed_generation=c->slot_generation[frame];
    uint64_t committed_geometry=c->slot_geometry_generation[frame];
    c->slot_generation[frame]=0; c->ready[frame]=false;
    radiance_input_snapshot_t snapshot=*lighting;
    snapshot.materials=c->atlas.materials; snapshot.material_count=c->atlas.material_count;
    snapshot.atlas_rgba=c->atlas.rgba; snapshot.atlas_width=c->atlas.width;
    snapshot.atlas_height=c->atlas.height; snapshot.atlas_layers=c->atlas.layers;
    snapshot.atlas_revision=c->atlas_upload_generation;
    snapshot.parameters.counts[1]=(uint32_t)c->atlas.material_count;
    snapshot.parameters.counts[2]=c->atlas.layers;
    if (!RadianceInputs_Update(&c->inputs, frame, &snapshot)) { c->last_error="lighting input upload preparation failed"; return false; }
    bool changed=committed_generation != c->generation;
    c->scene_gpu.last_upload_bytes[frame]=0;
    if (changed) {
        c->split_scene[frame]=RCSceneVk_UpdateSplit(&c->scene_gpu, frame, &c->runtime.scene);
        if (!c->split_scene[frame] && !RCSceneVk_Update(&c->scene_gpu, frame, &c->runtime.scene)) {
            c->last_error="scene buffer upload failed"; return false;
        }
    }
    bool use_rt=request_hardware && c->rt_available && c->split_scene[frame];
    c->rt.frame[frame].static_upload_bytes=c->rt.frame[frame].dynamic_upload_bytes=0;
    c->rt.frame[frame].static_builds=c->rt.frame[frame].dynamic_builds=c->rt.frame[frame].tlas_builds=0;
    c->rt.frame[frame].dynamic_updates=c->rt.frame[frame].tlas_updates=0;
    bool rt_rebuilt=false;
    if (request_hardware && !c->rt_available) c->last_warning="ray-query backend unavailable; using software tracing";
    else if (request_hardware && !c->split_scene[frame]) c->last_warning="split ray-query remap unavailable; using software tracing";
    bool batch_recorded=false;
    if (use_rt && (committed_geometry != c->geometry_generation || !RCSceneRT_Acceleration(&c->rt, frame) ||
        (batch && batch->count) || c->rt.frame[frame].batch_count)) {
        bool built=RCSceneRT_BuildSplitBatch(&c->rt, frame, &c->runtime.scene,
            RCSceneRuntime_BaseTriangleCount(&c->runtime), c->world_epoch, batch, command);
        if (!built && batch && batch->count) {
            c->last_warning="ECS shadow batch unavailable; using bounded scene";
            built=RCSceneRT_BuildSplitBatch(&c->rt, frame, &c->runtime.scene,
                RCSceneRuntime_BaseTriangleCount(&c->runtime), c->world_epoch, NULL, command);
        } else if (built && batch && batch->count) batch_recorded=true;
        if (!built) { use_rt=false; c->last_warning="ray-query acceleration build failed; using software tracing"; }
        else rt_rebuilt=true;
    }
    if (use_rt && (rt_rebuilt || !RCSceneVk_Descriptor(&c->scene_gpu, frame, true)) &&
        !RCSceneVk_SetAcceleration(&c->scene_gpu, frame, RCSceneRT_Acceleration(&c->rt, frame))) {
        use_rt=false; c->last_warning="ray-query descriptor update failed; using software tracing";
    }
    if (!RadianceInputs_RecordUpload(&c->inputs, frame, command)) { c->last_error="lighting atlas upload recording failed"; return false; }
    VkDescriptorSet scene=RCSceneVk_Descriptor(&c->scene_gpu, frame, use_rt);
    VkDescriptorSet inputs=RadianceInputs_Descriptor(&c->inputs, frame);
    if (!scene || !inputs) { RadianceInputs_DiscardRecorded(&c->inputs, frame); c->last_error="scene or lighting descriptor unavailable"; return false; }
    c->pending[frame]=true; c->pending_generation[frame]=c->generation;
    c->pending_geometry_generation[frame]=use_rt ? c->geometry_generation : 0; c->used_rt[frame]=use_rt;
    *result=(rc_scene_collect_result_t){.scene=scene, .lighting=inputs, .hardware=use_rt, .stats=c->stats,
        .triangles=c->runtime.scene.triangle_count, .nodes=c->runtime.scene.node_count, .materials=(uint32_t)c->atlas.material_count,
        .ecs_shadow_count=use_rt && batch_recorded ? batch->count : 0};
    return true;
}

bool RCCollect_RecordPrepared(rc_scene_collect_t *c, uint32_t frame, VkFence fence, VkCommandBuffer command,
    const radiance_input_snapshot_t *lighting, bool request_hardware, rc_scene_collect_result_t *result)
{
    return RCCollect_RecordPreparedBatch(c, frame, fence, command, lighting, request_hardware, NULL, result);
}

bool RCCollect_Record(rc_scene_collect_t *c, uint32_t frame, VkFence fence, VkCommandBuffer command,
    entity_t *const *candidates, uint32_t candidate_count, const float camera[3], uint64_t revision,
    const radiance_input_snapshot_t *lighting, bool request_hardware, rc_scene_collect_result_t *result)
{
    if (result) memset(result,0,sizeof(*result));
    if (!c || !c->device || frame >= c->frames || !fence || !command || !lighting || !result) {
        if(c) c->last_error="invalid collector recording arguments";
        return false;
    }
    if (c->pending[frame]) { c->last_error="frame has unresolved recording"; return false; }
    VkResult status=vkGetFenceStatus(c->device,fence);
    if (status != VK_SUCCESS) { c->last_error=status == VK_NOT_READY ? "frame fence not signaled" : "frame fence query failed"; return false; }
    if (!RCCollect_PrepareScene(c,candidates,candidate_count,camera,revision,NULL,0,NULL)) return false;
    return RCCollect_RecordPrepared(c,frame,fence,command,lighting,request_hardware,result);
}

const char *RCCollect_LastError(const rc_scene_collect_t *c) { return c ? c->last_error : "collector unavailable"; }
const char *RCCollect_LastWarning(const rc_scene_collect_t *c) { return c ? c->last_warning : NULL; }

void RCCollect_Submitted(rc_scene_collect_t *c, uint32_t frame)
{
    if (!c || frame >= c->frames || !c->pending[frame]) return;
    RadianceInputs_MarkSubmitted(&c->inputs, frame);
    RCSceneRT_Submitted(&c->rt, frame);
    c->slot_generation[frame]=c->pending_generation[frame];
    c->slot_geometry_generation[frame]=c->pending_geometry_generation[frame];
    c->pending[frame]=false; c->ready[frame]=true;
}
void RCCollect_Discard(rc_scene_collect_t *c, uint32_t frame)
{
    if (!c || frame >= c->frames) return;
    RCSceneRT_Discard(&c->rt, frame);
    if (!c->pending[frame]) return;
    RadianceInputs_DiscardRecorded(&c->inputs, frame);
    /* The recorded TLAS build may not have run; force a fresh snapshot/build. */
    c->pending[frame]=false; c->ready[frame]=false; c->slot_generation[frame]=0; c->slot_geometry_generation[frame]=0;
}
