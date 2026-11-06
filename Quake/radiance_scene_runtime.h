/* Runtime scene assembly for BSP radiance tracing. */
#ifndef RADIANCE_SCENE_RUNTIME_H
#define RADIANCE_SCENE_RUNTIME_H
#include "radiance_scene_bsp.h"

#define RC_SCENE_MAX_DYNAMIC_OCCLUDERS 256u

/* Animated alias model geometry needs a pose-aware extractor supplied by the
 * renderer. It must append world-space triangles and preserve material IDs. */
typedef bool (*rc_scene_alias_fn) (rc_scene_t *scene, const entity_t *entity,
	const float matrix[16], uint32_t instance, rc_scene_material_fn material, void *user);
/* Optional per-entity context for material callbacks. NULL resets the context. */
typedef void (*rc_scene_brush_context_fn) (const entity_t *entity, void *user);
typedef bool (*rc_scene_alias_supported_fn) (const entity_t *entity, void *user);
/* Changes when an alias's interpolated pose or animated material changes. */
typedef uint64_t (*rc_scene_alias_revision_fn) (const entity_t *entity, void *user);
/* Stable retained geometry may form a persistent BVH/RT prefix. Transform,
 * selection, material and content changes still invalidate that prefix. */
typedef bool (*rc_scene_alias_persistent_fn) (const entity_t *entity, void *user);
typedef struct rc_scene_entity_cache_s rc_scene_entity_cache_t;

typedef struct
{
	uint32_t considered, selected, omitted_limit, omitted_unsupported, omitted_transparent;
	uint32_t cached, updated, deferred;
	/* Geometry visited while refreshing cached bounds/content fingerprints. */
	uint64_t summary_triangles_scanned;
	/* Work on changed snapshots; reused ranges need no per-triangle assembly. */
	uint64_t cache_triangles_compared, assembly_triangles_copied;
	uint64_t base_triangles_built, mutable_triangles_built;
	bool base_content_changed, base_geometry_changed;
	bool rebuilt, refit, geometry_changed, materials_changed;
} rc_scene_runtime_stats_t;

typedef struct
{
	rc_scene_t static_scene, base_scene, mutable_scene, scene;
	const qmodel_t *world;
	uint64_t signature;
	rc_scene_brush_context_fn brush_context;
	rc_scene_alias_supported_fn alias_supported;
	rc_scene_alias_revision_fn alias_revision;
	rc_scene_alias_persistent_fn alias_persistent;
	rc_scene_entity_cache_t *entity_cache;
	uint32_t entity_cache_count, alias_budget;
	uint64_t base_geometry_generation, base_content_generation;
	bool valid, base_valid;
} rc_scene_runtime_t;

void RCSceneRuntime_Init (rc_scene_runtime_t *runtime);
void RCSceneRuntime_Clear (rc_scene_runtime_t *runtime);
void RCSceneRuntime_SetBrushContext (rc_scene_runtime_t *runtime, rc_scene_brush_context_fn callback);
/* Called before selecting the bounded dynamic occluder set. Unsupported
 * aliases are reported and omitted without consuming its capacity. */
void RCSceneRuntime_SetAliasSupport (rc_scene_runtime_t *runtime, rc_scene_alias_supported_fn callback);
void RCSceneRuntime_SetAliasRevision (rc_scene_runtime_t *runtime, rc_scene_alias_revision_fn callback);
void RCSceneRuntime_SetAliasPersistent (rc_scene_runtime_t *runtime, rc_scene_alias_persistent_fn callback);
uint32_t RCSceneRuntime_BaseTriangleCount (const rc_scene_runtime_t *runtime);
uint64_t RCSceneRuntime_BaseGeneration (const rc_scene_runtime_t *runtime);
uint64_t RCSceneRuntime_BaseContentGeneration (const rc_scene_runtime_t *runtime);
void RCSceneRuntime_SetAliasBudget (rc_scene_runtime_t *runtime, uint32_t budget);
/* Assemble the client-known occluders: all static entities in their stable
 * map order, then non-static renderer candidates. Visible statics occur in
 * both input lists and must only be emitted once. Objects replaced by a
 * particle emitter are excluded, matching R_StoreEfrags. Output storage must
 * hold static_count + visible_count entries. */
bool RCSceneRuntime_AssembleCandidates (entity_t **output, uint32_t capacity,
	entity_t *const *statics, uint32_t static_count, entity_t *const *visible,
	uint32_t visible_count, uint32_t *count);
/* Batched ECS visuals are contiguous client-owned entities and are omitted
 * from the renderer's ordinary visible list. Append their stable addresses so
 * they compete in the same bounded nearest-occluder selection. */
bool RCSceneRuntime_AppendBatch (entity_t **output, uint32_t capacity, uint32_t *count,
	entity_t *batch, uint32_t batch_count);
/* Exact number of distinct ECS entries in the current bounded scene. */
uint32_t RCSceneRuntime_CountBatchSelected (const rc_scene_runtime_t *runtime,
	const entity_t *batch, uint32_t batch_count);
/* Bounds and content identity of committed dynamic geometry. Reads only the
 * per-entity summaries, so unchanged large meshes are not visited per frame.
 * Selection/order/instance IDs and triangle content all affect fingerprint. */
bool RCSceneRuntime_DynamicBounds (const rc_scene_runtime_t *runtime,
	float mins[3], float maxs[3], uint64_t *fingerprint);
/* Must be called again on map changes and when material IDs are rebuilt. */
bool RCSceneRuntime_SetWorld (rc_scene_runtime_t *runtime, const qmodel_t *world,
	rc_scene_material_fn material, void *user);
/* Candidates are stable entity pointers for this call; order is the stable
 * tie-breaker. Brush objects are prioritized, then nearest models. Geometry is
 * rebuilt only when that selected set, its transforms/poses, or revision changes;
 * unchanged topology is refit instead of repartitioning the static world.
 * Without a revision callback, increment scene_revision when interpolated
 * poses or material IDs change. With a callback, cached aliases whose
 * identity and transform are unchanged may defer interpolation-only updates
 * up to alias_budget per call; the oldest waiting aliases are updated first.
 * New/replaced entities, brush entities and transforms are immediate.
 * Call SetWorld again if static material IDs change.
 * On failure the last complete scene is retained; caller must not silently use
 * it as if current. The caller owns GPU upload and fenced resource lifetime. */
bool RCSceneRuntime_Update (rc_scene_runtime_t *runtime, entity_t *const *candidates,
	uint32_t candidate_count, const float camera[3], uint64_t scene_revision,
	rc_scene_material_fn material, rc_scene_alias_fn alias, void *user,
	rc_scene_runtime_stats_t *stats);
/* Remove material entries no longer referenced by the assembled scene and
 * remap the static and per-entity caches as one transaction. Call only after a
 * successful Update and before consulting texture pointers in the registry. */
struct radiance_quake_materials_s;
bool RCSceneRuntime_CompactMaterials (rc_scene_runtime_t *runtime,
	struct radiance_quake_materials_s *materials, bool *changed);
#endif
