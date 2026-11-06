/* Alias-model adapter. Include quakedef.h before this header. */
#ifndef RADIANCE_SCENE_ALIAS_H
#define RADIANCE_SCENE_ALIAS_H
#include "radiance_scene.h"

/* Resolve the actual animated skin and fullbright mask into the shared GPU
 * material table. The callback may inspect entity/player color translations. */
typedef uint32_t (*rc_scene_alias_material_fn) (const entity_t *entity, uint32_t surface,
	const struct gltexture_s *texture, const struct gltexture_s *fullbright, void *user);

/* Idempotent cleanup used by GPU-mesh deletion and model memory teardown. */
void RCScene_DetachAliasHeader (aliashdr_t *hdr);
void RCScene_DetachAliasModel (qmodel_t *model);

/* Fast preflight for unsupported model formats or unavailable retained
 * geometry/texture-source metadata. Does not allocate or append geometry. */
bool RCScene_AliasSupported (const entity_t *entity);
/* Cache large immutable MD5 poses with the persistent scene prefix. This
 * changes caching only: smaller or animated aliases are never omitted here. */
bool RCScene_AliasPersistent (const entity_t *entity);
/* Append all compiled placements once to the immutable world geometry,
 * independent of the bounded dynamic-entity candidate selection. */
bool RCScene_AppendCompiledWorld (rc_scene_t *scene, const qmodel_t *world,
	rc_scene_alias_material_fn material, void *user);
/* Revision of the effective lerped pose and animated skin/material, without
 * including unrelated entities. Valid for supported aliases. */
uint64_t RCScene_AliasRevision (const entity_t *entity);
/* Explicit pose and transform must match the renderer's lerped frame. Model
 * matrices omit hdr->scale/scale_origin; retained geometry applies them.
 * Single-frame MD5 uses its retained skinned pose. Animated MD5, missing
 * retained data, invalid frames and material failure return false without
 * publishing a partial model. */
bool RCScene_AppendAlias (rc_scene_t *scene, const entity_t *entity, const float matrix[16],
	uint32_t pose1, uint32_t pose2, float blend, unsigned animation, uint32_t instance,
	rc_scene_alias_material_fn material, void *user);

/* Cache a single opaque local-space mesh and translation-only ECS instances.
 * The returned snapshot belongs to the cache and is valid until Prepare/Clear.
 * Unsupported or invalid entries reject the entire batch and leave ordinary
 * per-entity selection responsible for it. */
typedef struct
{
	rc_scene_t mesh;
	float (*origins)[3];
	uint32_t capacity;
	const qmodel_t *model;
	const aliashdr_t *header;
	const struct rc_alias_mesh_s *mesh_identity;
	uint64_t mesh_revision, retained_revision;
} rc_scene_ecs_batch_t;
void RCScene_ECSBatchInit (rc_scene_ecs_batch_t *batch);
void RCScene_ECSBatchClear (rc_scene_ecs_batch_t *batch);
bool RCScene_ECSBatchPrepare (rc_scene_ecs_batch_t *batch, const entity_t *entities,
	uint32_t count, rc_scene_rt_batch_t *snapshot);

/* Convenience wrapper using the same frame interpolation, model transform,
 * and animated-skin phase as R_DrawAliasModel. */
bool RCScene_AppendAliasLerped (rc_scene_t *scene, const entity_t *entity, uint32_t instance,
	rc_scene_alias_material_fn material, void *user);
#endif
