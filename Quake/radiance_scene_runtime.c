#include "quakedef.h"
#include "radiance_scene_runtime.h"
#include "radiance_materials_quake.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	entity_t *entity;
	uint32_t index;
	unsigned priority; /* BSP doors/brushes before alias props. */
	double distance2;
} rc_candidate_t;

typedef struct
{
	float mins[3], maxs[3];
	uint64_t fingerprint;
	bool found;
} rc_dynamic_summary_t;

struct rc_scene_entity_cache_s
{
	const entity_t *entity;
	const qmodel_t *model;
	uint64_t identity_revision, pose_revision;
	uint32_t instance, deferred_age;
	bool persistent;
	rc_scene_t geometry;
	rc_dynamic_summary_t summary;
};

typedef struct
{
	int old_index;
	uint64_t identity_revision, pose_revision;
	uint32_t deferred_age;
	bool needs_update, urgent, refresh, fresh, persistent;
	rc_scene_t geometry;
} rc_staged_entity_t;

static bool RCScene_BetterCandidate (const rc_candidate_t *a, const rc_candidate_t *b)
{
	if (a->priority != b->priority) return a->priority < b->priority;
	if (a->distance2 != b->distance2) return a->distance2 < b->distance2;
	return a->index < b->index;
}

/* A bounded max heap keeps the worst selected item at the root. */
static void RCScene_HeapPush (rc_candidate_t *heap, uint32_t *count, rc_candidate_t item)
{
	uint32_t at = (*count)++;
	while (at)
	{
		uint32_t parent = (at - 1) / 2;
		if (!RCScene_BetterCandidate (&heap[parent], &item)) break;
		heap[at] = heap[parent]; at = parent;
	}
	heap[at] = item;
}

static void RCScene_HeapReplaceWorst (rc_candidate_t *heap, uint32_t count, rc_candidate_t item)
{
	uint32_t at = 0;
	while (at * 2 + 1 < count)
	{
		uint32_t child = at * 2 + 1;
		if (child + 1 < count && RCScene_BetterCandidate (&heap[child], &heap[child + 1])) ++child;
		if (!RCScene_BetterCandidate (&item, &heap[child])) break;
		heap[at] = heap[child]; at = child;
	}
	heap[at] = item;
}

static uint64_t RCScene_HashBytes (uint64_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	while (size--) { hash ^= *bytes++; hash *= UINT64_C (1099511628211); }
	return hash;
}

static void RCScene_SummarizeEntity (rc_scene_entity_cache_t *entry)
{
	rc_dynamic_summary_t *summary = &entry->summary;
	memset (summary, 0, sizeof (*summary));
	summary->fingerprint = UINT64_C (1469598103934665603);
	for (uint32_t i = 0; i < entry->geometry.triangle_count; ++i)
	{
		rc_scene_triangle_t triangle = entry->geometry.triangles[i];
		if (!(triangle.metadata[1] & RC_SCENE_DYNAMIC)) continue;
		/* Instance IDs are assigned by the selected candidate order. Fold the
		 * current ID at aggregation, without rescanning a reordered mesh. */
		triangle.metadata[3] = 0;
		summary->fingerprint = RCScene_HashBytes (summary->fingerprint, &triangle, sizeof (triangle));
		for (unsigned vertex = 0; vertex < 3; ++vertex)
			for (unsigned axis = 0; axis < 3; ++axis)
			{
				float coordinate = triangle.position[vertex][axis];
				if (!summary->found && !vertex) summary->mins[axis] = summary->maxs[axis] = coordinate;
				else { summary->mins[axis] = q_min (summary->mins[axis], coordinate); summary->maxs[axis] = q_max (summary->maxs[axis], coordinate); }
			}
		summary->found = true;
	}
}

static bool RCScene_ValidateFreshGeometry (const rc_scene_t *geometry)
{
	if (geometry->triangle_count && !geometry->triangles) return false;
	/* An arbitrary extractor may fill its array directly. Validate it once,
 * even when it did not use RCScene_AppendTriangle. Cached geometry has
 * already passed this check and cannot acquire new positions on reuse. */
	for (uint32_t i = 0; i < geometry->triangle_count; ++i)
		for (unsigned vertex = 0; vertex < 3; ++vertex)
			for (unsigned axis = 0; axis < 3; ++axis)
				if (!isfinite (geometry->triangles[i].position[vertex][axis])) return false;
	return true;
}

static bool RCScene_EqualEntityGeometry (const rc_scene_t *a, const rc_scene_t *b,
	rc_scene_runtime_stats_t *stats)
{
	if (a->triangle_count != b->triangle_count) return false;
	for (uint32_t i = 0; i < a->triangle_count; ++i)
	{
		++stats->cache_triangles_compared;
		/* Assembly supplies the current candidate's instance ID. The cached
		 * source may still hold the ID from an earlier selection ordering. */
		if (memcmp (&a->triangles[i], &b->triangles[i],
			offsetof (rc_scene_triangle_t, metadata) + 3 * sizeof (uint32_t))) return false;
	}
	return true;
}

bool RCSceneRuntime_DynamicBounds (const rc_scene_runtime_t *runtime,
	float mins[3], float maxs[3], uint64_t *fingerprint)
{
	uint64_t hash = UINT64_C (1469598103934665603);
	bool found = false;
	/* Eligibility callbacks may invalidate the next update without deleting
	 * the last committed scene. Its old bounds must still be refreshed. */
	if (runtime)
	{
		hash = RCScene_HashBytes (hash, &runtime->entity_cache_count, sizeof (runtime->entity_cache_count));
		for (uint32_t i = 0; i < runtime->entity_cache_count; ++i)
		{
			const rc_scene_entity_cache_t *entry = &runtime->entity_cache[i];
			hash = RCScene_HashBytes (hash, &entry->instance, sizeof (entry->instance));
			hash = RCScene_HashBytes (hash, &entry->geometry.triangle_count, sizeof (entry->geometry.triangle_count));
			hash = RCScene_HashBytes (hash, &entry->summary.fingerprint, sizeof (entry->summary.fingerprint));
			if (!entry->summary.found) continue;
			for (unsigned axis = 0; axis < 3; ++axis)
			{
				if (!found) { mins[axis] = entry->summary.mins[axis]; maxs[axis] = entry->summary.maxs[axis]; }
				else { mins[axis] = q_min (mins[axis], entry->summary.mins[axis]); maxs[axis] = q_max (maxs[axis], entry->summary.maxs[axis]); }
			}
			found = true;
		}
	}
	if (fingerprint) *fingerprint = hash;
	return found;
}

void RCSceneRuntime_Init (rc_scene_runtime_t *runtime)
{
	memset (runtime, 0, sizeof (*runtime)); runtime->alias_budget = 8;
}
void RCSceneRuntime_SetBrushContext (rc_scene_runtime_t *runtime, rc_scene_brush_context_fn callback)
{
	if (runtime && runtime->brush_context != callback) { runtime->brush_context = callback; runtime->valid = false; }
}
void RCSceneRuntime_SetAliasSupport (rc_scene_runtime_t *runtime, rc_scene_alias_supported_fn callback)
{
	if (runtime && runtime->alias_supported != callback) { runtime->alias_supported = callback; runtime->valid = false; }
}
void RCSceneRuntime_SetAliasRevision (rc_scene_runtime_t *runtime, rc_scene_alias_revision_fn callback)
{
	if (runtime && runtime->alias_revision != callback) { runtime->alias_revision = callback; runtime->valid = false; }
}
void RCSceneRuntime_SetAliasPersistent (rc_scene_runtime_t *runtime, rc_scene_alias_persistent_fn callback)
{
	if (runtime && runtime->alias_persistent != callback) { runtime->alias_persistent = callback; runtime->valid = false; }
}
uint32_t RCSceneRuntime_BaseTriangleCount (const rc_scene_runtime_t *runtime)
{
	return runtime ? (runtime->base_valid ? runtime->base_scene.triangle_count : runtime->static_scene.triangle_count) : 0;
}
uint64_t RCSceneRuntime_BaseGeneration (const rc_scene_runtime_t *runtime)
{
	return runtime ? runtime->base_geometry_generation : 0;
}
uint64_t RCSceneRuntime_BaseContentGeneration (const rc_scene_runtime_t *runtime)
{
	return runtime ? runtime->base_content_generation : 0;
}
void RCSceneRuntime_SetAliasBudget (rc_scene_runtime_t *runtime, uint32_t budget)
{
	if (runtime) runtime->alias_budget = budget;
}
bool RCSceneRuntime_AssembleCandidates (entity_t **output, uint32_t capacity,
	entity_t *const *statics, uint32_t static_count, entity_t *const *visible,
	uint32_t visible_count, uint32_t *count)
{
	if (!count || (static_count && !statics) || (visible_count && !visible) ||
		static_count > UINT32_MAX - visible_count || capacity < static_count + visible_count ||
		((static_count || visible_count) && !output)) return false;
	uint32_t written = 0;
	for (uint32_t i = 0; i < static_count; ++i)
	{
		entity_t *entity = statics[i];
		if (!entity || !entity->model) continue;
		if (entity->compiled_instance) continue; /* already in the immutable world prefix */
		/* R_StoreEfrags does not draw a model replaced by its configured
		 * particle system. Explicit per-entity effects take precedence. */
		if (!entity->compiled_instance && entity->netstate.emiteffectnum <= 0 && entity->model->emiteffect >= 0 &&
			(entity->model->flags & MOD_EMITREPLACE)) continue;
		output[written++] = entity;
	}
	for (uint32_t i = 0; i < visible_count; ++i)
		if (visible[i] && !visible[i]->is_static && !visible[i]->compiled_instance) output[written++] = visible[i];
	*count = written;
	return true;
}

bool RCSceneRuntime_AppendBatch (entity_t **output, uint32_t capacity, uint32_t *count,
	entity_t *batch, uint32_t batch_count)
{
	if (!count || *count > capacity || batch_count > capacity - *count ||
		(batch_count && (!output || !batch))) return false;
	for (uint32_t i = 0; i < batch_count; ++i) output[(*count)++] = &batch[i];
	return true;
}
static void RCSceneRuntime_ClearEntities (rc_scene_runtime_t *runtime)
{
	for (uint32_t i = 0; i < runtime->entity_cache_count; ++i) RCScene_Clear (&runtime->entity_cache[i].geometry);
	free (runtime->entity_cache); runtime->entity_cache = NULL; runtime->entity_cache_count = 0;
}
void RCSceneRuntime_Clear (rc_scene_runtime_t *runtime)
{
	RCSceneRuntime_ClearEntities (runtime);
	RCScene_Clear (&runtime->static_scene); RCScene_Clear (&runtime->base_scene);
	RCScene_Clear (&runtime->mutable_scene); RCScene_Clear (&runtime->scene);
	memset (runtime, 0, sizeof (*runtime));
}

bool RCSceneRuntime_SetWorld (rc_scene_runtime_t *runtime, const qmodel_t *world,
	rc_scene_material_fn material, void *user)
{
	if (!runtime || !world) return false;
	if (runtime->brush_context) runtime->brush_context (NULL, user);
	rc_scene_t pending; RCScene_Init (&pending);
	if (!RCScene_AppendBrush (&pending, world, NULL, 0, false, material, user))
	{
		RCScene_Clear (&pending); return false;
	}
	RCScene_Clear (&runtime->static_scene); RCScene_Clear (&runtime->base_scene);
	RCScene_Clear (&runtime->mutable_scene); RCScene_Clear (&runtime->scene);
	RCSceneRuntime_ClearEntities (runtime);
	runtime->static_scene = pending; runtime->world = world;
	runtime->signature = 0; runtime->valid = runtime->base_valid = false;
	return true;
}

uint32_t RCSceneRuntime_CountBatchSelected (const rc_scene_runtime_t *runtime,
    const entity_t *batch, uint32_t batch_count)
{
    if (!runtime || !batch || !batch_count || !runtime->valid) return 0;
    uintptr_t first=(uintptr_t)batch;
    if ((uint64_t)batch_count * sizeof (*batch) > UINTPTR_MAX - first) return 0;
    uintptr_t last=first+(uintptr_t)batch_count*sizeof(*batch);
    uint32_t selected=0;
    for (uint32_t i=0; i<runtime->entity_cache_count; ++i) {
        uintptr_t candidate=(uintptr_t)runtime->entity_cache[i].entity;
        if (candidate<first || candidate>=last || (candidate-first)%sizeof(*batch)) continue;
        bool duplicate=false;
        for (uint32_t j=0; j<i; ++j)
            if (runtime->entity_cache[j].entity==runtime->entity_cache[i].entity) { duplicate=true; break; }
        if (!duplicate) ++selected;
    }
    return selected;
}

typedef struct { bool content, geometry, materials; uint32_t triangles; } rc_partition_delta_t;

static rc_partition_delta_t RCScene_PartitionDelta (const rc_scene_runtime_t *runtime,
	const rc_scene_entity_cache_t *next, uint32_t count, bool persistent, rc_scene_runtime_stats_t *stats)
{
	rc_partition_delta_t delta = {0}; uint32_t old_at = 0;
	if (!runtime->valid || !runtime->base_valid) delta.content = delta.geometry = delta.materials = true;
	for (uint32_t i = 0; i < count; ++i)
	{
		if (next[i].persistent != persistent) continue;
		delta.triangles += next[i].geometry.triangle_count;
		while (old_at < runtime->entity_cache_count && runtime->entity_cache[old_at].persistent != persistent) ++old_at;
		const rc_scene_entity_cache_t *before = old_at < runtime->entity_cache_count ? &runtime->entity_cache[old_at++] : NULL;
		if (!before || before->entity != next[i].entity || before->model != next[i].model ||
			before->geometry.triangle_count != next[i].geometry.triangle_count)
		{
			delta.content = delta.geometry = delta.materials = true; continue;
		}
		if (before->instance != next[i].instance) delta.content = true;
		if (before->geometry.triangles == next[i].geometry.triangles) continue;
		if (RCScene_EqualEntityGeometry (&before->geometry, &next[i].geometry, stats)) continue;
		delta.content = true;
		for (uint32_t t = 0; t < next[i].geometry.triangle_count; ++t)
		{
			const rc_scene_triangle_t *a = &before->geometry.triangles[t], *b = &next[i].geometry.triangles[t];
			delta.materials |= a->metadata[0] != b->metadata[0];
			for (unsigned v = 0; v < 3 && !delta.geometry; ++v)
				if (memcmp (a->position[v], b->position[v], 3 * sizeof (float))) delta.geometry = true;
		}
	}
	while (old_at < runtime->entity_cache_count && runtime->entity_cache[old_at].persistent != persistent) ++old_at;
	if (old_at != runtime->entity_cache_count) delta.content = delta.geometry = delta.materials = true;
	return delta;
}

static bool RCScene_CopyTopology (rc_scene_t *target, const rc_scene_t *source)
{
	if (target->triangle_count != source->triangle_count) return false;
	if (!source->triangle_count) return true;
	if (!source->nodes || !source->indices || source->index_count != source->triangle_count || !source->node_count) return false;
	target->nodes = malloc ((size_t)source->node_count * sizeof (*target->nodes));
	target->indices = malloc ((size_t)source->index_count * sizeof (*target->indices));
	if (!target->nodes || !target->indices) return false;
	memcpy (target->nodes, source->nodes, (size_t)source->node_count * sizeof (*target->nodes));
	memcpy (target->indices, source->indices, (size_t)source->index_count * sizeof (*target->indices));
	target->node_count = source->node_count; target->index_count = source->index_count;
	return true;
}

static bool RCScene_AssemblePartition (rc_scene_t *target, const rc_scene_t *world,
	const rc_scene_entity_cache_t *entries, uint32_t count, bool persistent, uint32_t total)
{
	if ((uint64_t)total * sizeof (*target->triangles) > SIZE_MAX) return false;
	if (total && !(target->triangles = malloc ((size_t)total * sizeof (*target->triangles)))) return false;
	target->triangle_count = target->triangle_capacity = total;
	uint32_t at = world ? world->triangle_count : 0;
	if (at) memcpy (target->triangles, world->triangles, (size_t)at * sizeof (*target->triangles));
	for (uint32_t i = 0; i < count; ++i)
	{
		if (entries[i].persistent != persistent) continue;
		uint32_t n = entries[i].geometry.triangle_count;
		if (n) memcpy (target->triangles + at, entries[i].geometry.triangles, (size_t)n * sizeof (*target->triangles));
		for (uint32_t t = 0; t < n; ++t) target->triangles[at+t].metadata[3] = entries[i].instance;
		at += n;
	}
	return at == total;
}

static bool RCScene_UpdatePartitions (rc_scene_runtime_t *runtime,
	const rc_scene_entity_cache_t *next, uint32_t count, rc_scene_runtime_stats_t *stats)
{
	rc_partition_delta_t base = RCScene_PartitionDelta (runtime, next, count, true, stats);
	rc_partition_delta_t moving = RCScene_PartitionDelta (runtime, next, count, false, stats);
	uint32_t world_count = runtime->static_scene.triangle_count;
	if (base.triangles > UINT32_MAX - world_count) return false;
	base.triangles += world_count;
	stats->base_content_changed = base.content; stats->base_geometry_changed = base.geometry;
	if (!base.content && !moving.content) return true;
	rc_scene_t new_base = {0}, new_moving = {0}, combined = {0};
	if (base.content)
	{
		if (!RCScene_AssemblePartition (&new_base, &runtime->static_scene, next, count, true, base.triangles)) goto failed;
		if (base.geometry)
		{
			if (!RCScene_Build (&new_base)) goto failed;
			stats->base_triangles_built += base.triangles;
		}
		else if (!RCScene_CopyTopology (&new_base, &runtime->base_scene)) goto failed;
		stats->assembly_triangles_copied += base.triangles;
	}
	if (moving.content)
	{
		if (!RCScene_AssemblePartition (&new_moving, NULL, next, count, false, moving.triangles)) goto failed;
		if (moving.geometry)
		{
			if (!RCScene_Build (&new_moving)) goto failed;
			stats->mutable_triangles_built += moving.triangles;
		}
		else if (!RCScene_CopyTopology (&new_moving, &runtime->mutable_scene)) goto failed;
		stats->assembly_triangles_copied += moving.triangles;
	}
	/* Emissive selection decorates only the assembled scene. Reuse its base
	 * triangle bytes when unchanged, with the immutable cached BVH topology,
	 * so mutable projectile churn cannot erase those derived flags. */
	rc_scene_t base_view = runtime->base_scene;
	if (!base.content) base_view.triangles = runtime->scene.triangles;
	rc_scene_t moving_view = runtime->mutable_scene;
	if (!moving.content && moving_view.triangle_count)
		moving_view.triangles = runtime->scene.triangles + runtime->base_scene.triangle_count;
	bool patched = !base.content && moving.content &&
		RCScene_ReplaceSuffix (&runtime->scene, &runtime->base_scene, &new_moving);
	if (!patched && !RCScene_Compose (&combined, base.content ? &new_base : &base_view,
		moving.content ? &new_moving : &moving_view)) goto failed;
	stats->materials_changed = base.materials || moving.materials;
	stats->geometry_changed = base.geometry || moving.geometry;
	stats->rebuilt = !runtime->valid || base.geometry || (!patched && runtime->scene.triangle_count != combined.triangle_count);
	stats->refit = !stats->rebuilt;
	if (base.content) { RCScene_Clear (&runtime->base_scene); runtime->base_scene = new_base; memset (&new_base, 0, sizeof (new_base)); }
	if (moving.content) { RCScene_Clear (&runtime->mutable_scene); runtime->mutable_scene = new_moving; memset (&new_moving, 0, sizeof (new_moving)); }
	if (!patched) { RCScene_Clear (&runtime->scene); runtime->scene = combined; }
	runtime->base_valid = true;
	if (base.geometry && ++runtime->base_geometry_generation == 0) runtime->base_geometry_generation = 1;
	if (base.content && ++runtime->base_content_generation == 0) runtime->base_content_generation = 1;
	return true;
failed:
	RCScene_Clear (&new_base); RCScene_Clear (&new_moving); RCScene_Clear (&combined);
	return false;
}

bool RCSceneRuntime_Update (rc_scene_runtime_t *runtime, entity_t *const *candidates,
	uint32_t candidate_count, const float camera[3], uint64_t scene_revision,
	rc_scene_material_fn material, rc_scene_alias_fn alias, void *user,
	rc_scene_runtime_stats_t *stats)
{
	if (!runtime || !runtime->world || !camera || (candidate_count && !candidates)) return false;
	rc_scene_runtime_stats_t result = {0};
	for (int a = 0; a < 3; ++a) if (!isfinite (camera[a])) return false;
	rc_candidate_t heap[RC_SCENE_MAX_DYNAMIC_OCCLUDERS]; uint32_t count = 0, eligible = 0;
	for (uint32_t i = 0; i < candidate_count; ++i)
	{
		entity_t *entity = candidates[i];
		if (!entity || !entity->model || entity->model == runtime->world || entity->model->needload) continue;
		++result.considered;
		if (entity->alpha != ENTALPHA_DEFAULT && ENTALPHA_DECODE (entity->alpha) < 1.0f)
		{
			++result.omitted_transparent; continue;
		}
		if (entity->model->type != mod_brush && (entity->model->type != mod_alias || !alias))
		{
			++result.omitted_unsupported; continue;
		}
		if (entity->model->type == mod_alias && runtime->alias_supported && !runtime->alias_supported (entity, user))
		{
			++result.omitted_unsupported; continue;
		}
		double distance2 = 0.0;
		bool finite = true;
		for (int a = 0; a < 3; ++a)
		{
			if (!isfinite (entity->origin[a]) || !isfinite (entity->angles[a])) finite = false;
			double delta = (double)entity->origin[a] - camera[a]; distance2 += delta * delta;
		}
		if (!finite || !isfinite (distance2)) { ++result.omitted_unsupported; continue; }
		++eligible;
		rc_candidate_t candidate = {entity, i, entity->model->type == mod_brush ? 0u : 1u, distance2};
		if (count < RC_SCENE_MAX_DYNAMIC_OCCLUDERS) RCScene_HeapPush (heap, &count, candidate);
		else if (RCScene_BetterCandidate (&candidate, &heap[0])) RCScene_HeapReplaceWorst (heap, count, candidate);
	}
	result.selected = count; result.omitted_limit = eligible - count;
	/* Scene order must stay stable as the camera moves within a selection. */
	for (uint32_t i = 1; i < count; ++i)
	{
		rc_candidate_t item = heap[i]; uint32_t j = i;
		while (j && heap[j-1].index > item.index) { heap[j] = heap[j-1]; --j; }
		heap[j] = item;
	}
	/* Build a transaction of per-entity replacements. Additions, removals,
	 * identity changes and transforms are immediate. Alias interpolation may wait. */
	rc_staged_entity_t staged[RC_SCENE_MAX_DYNAMIC_OCCLUDERS] = {0};
	bool claimed[RC_SCENE_MAX_DYNAMIC_OCCLUDERS] = {0};
	bool changed = !runtime->valid || count != runtime->entity_cache_count;
	rc_scene_t generated; RCScene_Init (&generated);
	rc_scene_entity_cache_t *next_cache = NULL;
	uint64_t hash = UINT64_C (1469598103934665603);
	/* In the common case the selected entities retain their order. Once a
	 * mismatch occurs, search from the beginning as before: this preserves
	 * first-unclaimed matching even if the candidate list contains duplicates. */
	bool cache_aligned = true;
	for (uint32_t i = 0; i < count; ++i)
	{
		entity_t *e = heap[i].entity;
		rc_staged_entity_t *stage = &staged[i];
		stage->old_index = -1;
		if (cache_aligned && i < runtime->entity_cache_count &&
			runtime->entity_cache[i].entity == e && runtime->entity_cache[i].model == e->model)
		{
			stage->old_index = (int)i; claimed[i] = true;
		}
		else
		{
			cache_aligned = false;
			for (uint32_t j = 0; j < runtime->entity_cache_count; ++j)
				if (!claimed[j] && runtime->entity_cache[j].entity == e && runtime->entity_cache[j].model == e->model)
				{ stage->old_index = (int)j; claimed[j] = true; break; }
		}
		uint64_t identity = UINT64_C (1469598103934665603);
		identity = RCScene_HashBytes (identity, &e->model, sizeof (e->model));
		identity = RCScene_HashBytes (identity, &e->render_generation, sizeof (e->render_generation));
		identity = RCScene_HashBytes (identity, e->origin, sizeof (e->origin));
		identity = RCScene_HashBytes (identity, e->angles, sizeof (e->angles));
		identity = RCScene_HashBytes (identity, &e->netstate.scale, sizeof (e->netstate.scale));
		identity = RCScene_HashBytes (identity, &e->modelscale_override, sizeof (e->modelscale_override));
		identity = RCScene_HashBytes (identity, &e->frame, sizeof (e->frame));
		identity = RCScene_HashBytes (identity, &e->skinnum, sizeof (e->skinnum));
		identity = RCScene_HashBytes (identity, &e->colormap, sizeof (e->colormap));
		/* Existing users without a per-entity revision keep the prior global
		 * invalidation behavior and never defer potentially unknown changes. */
		if (!runtime->alias_revision) identity = RCScene_HashBytes (identity, &scene_revision, sizeof (scene_revision));
		stage->identity_revision = identity;
		stage->pose_revision = runtime->alias_revision && e->model->type == mod_alias ? runtime->alias_revision (e, user) : 0;
		const rc_scene_entity_cache_t *old = stage->old_index >= 0 ? &runtime->entity_cache[stage->old_index] : NULL;
		stage->persistent = runtime->alias_persistent && e->model->type == mod_alias && runtime->alias_persistent (e, user);
		const bool content_changed = old && old->pose_revision != stage->pose_revision;
		stage->urgent = !runtime->valid || !old || old->identity_revision != identity || e->model->type == mod_brush;
		stage->needs_update = stage->urgent || content_changed;
		if (e->model->type == mod_brush && old && runtime->valid && old->identity_revision == identity) stage->needs_update = false;
		stage->refresh = stage->needs_update && stage->urgent;
		stage->deferred_age = old ? old->deferred_age : 0;
		if (!old || (uint32_t)stage->old_index != i || old->instance != heap[i].index || old->persistent != stage->persistent) changed = true;
	}
	/* Oldest deferred pose wins; ties favor proximity, then stable index. */
	for (uint32_t n = 0; n < runtime->alias_budget; ++n)
	{
		int best = -1;
		for (uint32_t i = 0; i < count; ++i)
			if (staged[i].needs_update && !staged[i].refresh &&
				(best < 0 || staged[i].deferred_age > staged[best].deferred_age ||
				 (staged[i].deferred_age == staged[best].deferred_age && RCScene_BetterCandidate (&heap[i], &heap[best])))) best = (int)i;
		if (best < 0) break;
		staged[best].refresh = true;
	}
	for (uint32_t i = 0; i < count; ++i)
	{
		rc_staged_entity_t *stage = &staged[i]; entity_t *e = heap[i].entity;
		if (stage->refresh)
		{
			float matrix[16], angles[3];
			VectorCopy (e->angles, angles); angles[0] = -angles[0];
			IdentityMatrix (matrix); R_RotateForEntityScale (matrix, e->origin, angles, R_EntityModelScale (e));
			RCScene_Init (&stage->geometry); stage->fresh = true;
			if (e->model->type == mod_brush)
			{
				if (runtime->brush_context) runtime->brush_context (e, user);
				if (!RCScene_AppendBrush (&stage->geometry, e->model, matrix, heap[i].index, true, material, user)) goto failed;
				if (runtime->brush_context) runtime->brush_context (NULL, user);
			}
			else if (!alias (&stage->geometry, e, matrix, heap[i].index, material, user)) goto failed;
			stage->deferred_age = 0; ++result.updated; changed = true;
		}
		else
		{
			++result.cached;
			if (stage->needs_update) { ++result.deferred; if (stage->deferred_age != UINT32_MAX) ++stage->deferred_age; }
			else stage->deferred_age = 0;
		}
	}
	if (!changed)
	{
		for (uint32_t i = 0; i < count; ++i) runtime->entity_cache[staged[i].old_index].deferred_age = staged[i].deferred_age;
		if (stats) *stats = result;
		return true;
	}
	if (count && !(next_cache = calloc (count, sizeof (*next_cache)))) goto failed;
	uint32_t static_count = runtime->static_scene.triangle_count, dynamic_count = 0;
	bool stable_ranges = runtime->valid && count == runtime->entity_cache_count;
	for (uint32_t i = 0; i < count; ++i)
	{
		rc_staged_entity_t *stage = &staged[i];
		const rc_scene_entity_cache_t *old = stage->old_index >= 0 ? &runtime->entity_cache[stage->old_index] : NULL;
		rc_scene_entity_cache_t *next = &next_cache[i];
		next->entity = heap[i].entity; next->model = heap[i].entity->model; next->instance = heap[i].index;
		next->identity_revision = stage->fresh ? stage->identity_revision : old->identity_revision;
		next->pose_revision = stage->fresh ? stage->pose_revision : old->pose_revision;
		next->deferred_age = stage->deferred_age;
		next->persistent = stage->persistent;
		next->geometry = stage->fresh ? stage->geometry : old->geometry;
		if (stage->fresh)
		{
			if (!RCScene_ValidateFreshGeometry (&next->geometry)) goto failed;
			RCScene_SummarizeEntity (next);
			result.summary_triangles_scanned += next->geometry.triangle_count;
		}
		else next->summary = old->summary;
		if (next->geometry.triangle_count > UINT32_MAX - dynamic_count) goto failed;
		dynamic_count += next->geometry.triangle_count;
		if (!old || stage->old_index != (int)i || old->instance != heap[i].index ||
			old->geometry.triangle_count != next->geometry.triangle_count) stable_ranges = false;
		hash = RCScene_HashBytes (hash, &next->identity_revision, sizeof (next->identity_revision));
		hash = RCScene_HashBytes (hash, &next->pose_revision, sizeof (next->pose_revision));
	}
	if (dynamic_count > UINT32_MAX - static_count) goto failed;
	if (runtime->alias_persistent || runtime->base_valid)
	{
		if (!RCScene_UpdatePartitions (runtime, next_cache, count, &result)) goto failed;
		goto commit;
	}
	stable_ranges &= runtime->scene.triangle_count == static_count + dynamic_count;
	if (stable_ranges)
	{
		/* Common animation case: keep large unchanged entity ranges in place.
		 * Compare fresh source bytes exactly; a new pose revision can emit the
		 * same geometry and need neither scene upload nor a BVH refit. */
		bool replace[RC_SCENE_MAX_DYNAMIC_OCCLUDERS] = {0};
		uint32_t changed_count = 0, offset = static_count;
		for (uint32_t i = 0; i < count; ++i)
		{
			const rc_scene_t *source = &next_cache[i].geometry;
			if (staged[i].fresh && !RCScene_EqualEntityGeometry (source, &runtime->entity_cache[i].geometry, &result))
			{
				replace[i] = true; changed_count += source->triangle_count;
				for (uint32_t t = 0; t < source->triangle_count; ++t)
				{
					const rc_scene_triangle_t *before = &runtime->scene.triangles[offset+t], *after = &source->triangles[t];
					result.materials_changed |= before->metadata[0] != after->metadata[0];
					for (unsigned vertex = 0; vertex < 3 && !result.geometry_changed; ++vertex)
						if (memcmp (before->position[vertex], after->position[vertex], 3 * sizeof (float))) result.geometry_changed = true;
				}
			}
			offset += source->triangle_count;
		}
		if (!changed_count) goto commit;
		if ((uint64_t)changed_count * sizeof (rc_scene_triangle_t) > SIZE_MAX) goto failed;
		rc_scene_triangle_t *previous = malloc ((size_t)changed_count * sizeof (*previous));
		if (!previous) goto failed;
		offset = static_count;
		uint32_t saved = 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			const rc_scene_t *source = &next_cache[i].geometry;
			if (replace[i])
			{
				size_t bytes = (size_t)source->triangle_count * sizeof (*previous);
				memcpy (previous + saved, runtime->scene.triangles + offset, bytes);
				memcpy (runtime->scene.triangles + offset, source->triangles, bytes);
				for (uint32_t t = 0; t < source->triangle_count; ++t) runtime->scene.triangles[offset+t].metadata[3] = heap[i].index;
				saved += source->triangle_count;
			}
			offset += source->triangle_count;
		}
		if (result.geometry_changed && !RCScene_Refit (&runtime->scene))
		{
			offset = static_count; saved = 0;
			for (uint32_t i = 0; i < count; ++i)
			{
				uint32_t n = next_cache[i].geometry.triangle_count;
				if (replace[i]) { memcpy (runtime->scene.triangles + offset, previous + saved, (size_t)n * sizeof (*previous)); saved += n; }
				offset += n;
			}
			free (previous); goto failed;
		}
		free (previous);
		result.assembly_triangles_copied += changed_count;
		result.refit = true;
		goto commit;
	}
	/* Reordering or inventory changes need a complete new dynamic sequence.
	 * Every fresh source was validated above; reused sources are immutable. */
	if ((uint64_t)dynamic_count * sizeof (*generated.triangles) > SIZE_MAX) goto failed;
	if (dynamic_count && !(generated.triangles = malloc ((size_t)dynamic_count * sizeof (*generated.triangles)))) goto failed;
	generated.triangle_count = generated.triangle_capacity = dynamic_count;
	uint32_t offset = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		const rc_scene_t *source = &next_cache[i].geometry;
		if (source->triangle_count) memcpy (generated.triangles + offset, source->triangles,
			(size_t)source->triangle_count * sizeof (*generated.triangles));
		for (uint32_t t = 0; t < source->triangle_count; ++t) generated.triangles[offset+t].metadata[3] = heap[i].index;
		offset += source->triangle_count;
	}
	result.assembly_triangles_copied += dynamic_count;
	result.materials_changed = !runtime->valid || runtime->scene.triangle_count != static_count + dynamic_count;
	for (uint32_t i = 0; !result.materials_changed && i < dynamic_count; ++i)
		if (runtime->scene.triangles[static_count + i].metadata[0] != generated.triangles[i].metadata[0])
			result.materials_changed = true;
	/* Material, animation and light-source metadata can change without moving
	 * a triangle. Hardware AS geometry needs only positions and primitive order. */
	result.geometry_changed = !runtime->valid || runtime->scene.triangle_count != static_count + dynamic_count;
	for (uint32_t i = 0; !result.geometry_changed && i < dynamic_count; ++i)
		for (unsigned p = 0; p < 3; ++p)
			if (memcmp (runtime->scene.triangles[static_count+i].position[p], generated.triangles[i].position[p], 3*sizeof(float)))
			{
				result.geometry_changed = true; break;
			}
	bool same_topology = runtime->valid && runtime->scene.triangle_count == static_count + dynamic_count;
	if (same_topology)
	{
		/* Save only dynamic data. The expensive static-world BVH partition is
		 * retained; refit updates bounds bottom-up without sorting geometry. */
		size_t bytes = (size_t)dynamic_count * sizeof (*generated.triangles);
		rc_scene_triangle_t *previous = bytes ? malloc (bytes) : NULL;
		if (bytes && !previous) goto failed;
		if (bytes)
		{
			memcpy (previous, runtime->scene.triangles + static_count, bytes);
			memcpy (runtime->scene.triangles + static_count, generated.triangles, bytes);
		}
		if (result.geometry_changed && !RCScene_Refit (&runtime->scene))
		{
			if (bytes) memcpy (runtime->scene.triangles + static_count, previous, bytes);
			free (previous);
			goto failed;
		}
		free (previous); RCScene_Clear (&generated);
		result.refit = true;
		goto commit;
	}
	rc_scene_t pending; RCScene_Init (&pending);
	uint32_t total = static_count + dynamic_count;
	if (total)
	{
		if ((uint64_t)total * sizeof (*pending.triangles) > SIZE_MAX) goto failed;
		pending.triangles = malloc ((size_t)total * sizeof (*pending.triangles));
		if (!pending.triangles) goto failed;
		if (static_count) memcpy (pending.triangles, runtime->static_scene.triangles, (size_t)static_count * sizeof (*pending.triangles));
		if (dynamic_count) memcpy (pending.triangles + static_count, generated.triangles, (size_t)dynamic_count * sizeof (*pending.triangles));
		pending.triangle_count = pending.triangle_capacity = total;
	}
	if (!RCScene_Build (&pending)) { RCScene_Clear (&pending); goto failed; }
	RCScene_Clear (&generated); RCScene_Clear (&runtime->scene); runtime->scene = pending;
	result.rebuilt = true;
commit:
	/* Transfer ownership only after the full scene and BVH have succeeded. */
	for (uint32_t i = 0; i < count; ++i)
	{
		if (staged[i].fresh) memset (&staged[i].geometry, 0, sizeof (staged[i].geometry));
		else memset (&runtime->entity_cache[staged[i].old_index].geometry, 0, sizeof (runtime->entity_cache[staged[i].old_index].geometry));
	}
	RCSceneRuntime_ClearEntities (runtime); runtime->entity_cache = next_cache; runtime->entity_cache_count = count;
	runtime->signature = hash; runtime->valid = true;
	if (stats) *stats = result;
	return true;
failed:
	if (runtime->brush_context) runtime->brush_context (NULL, user);
	RCScene_Clear (&generated); free (next_cache);
	for (uint32_t i = 0; i < count; ++i) if (staged[i].fresh) RCScene_Clear (&staged[i].geometry);
	if (stats) *stats = result;
	return false;
}

/* Scene, world and retained entity snapshots all own copies of triangle
 * material IDs. Validate every copy before changing any of them, since an
 * incomplete remap would turn a later cache hit into a wrong material. */
static bool RCSceneRuntime_MaterialsPresent (const rc_scene_t *scene, const uint32_t *remap, size_t count)
{
	if (scene->triangle_count && !scene->triangles) return false;
	for (uint32_t i = 0; i < scene->triangle_count; ++i)
	{
		uint32_t material = scene->triangles[i].metadata[0];
		if (material >= count || remap[material] == UINT32_MAX) return false;
	}
	return true;
}

static bool RCSceneRuntime_RemapScene (rc_scene_t *scene, const uint32_t *remap)
{
	bool changed = false;
	for (uint32_t i = 0; i < scene->triangle_count; ++i)
	{
		uint32_t old = scene->triangles[i].metadata[0], mapped = remap[old];
		scene->triangles[i].metadata[0] = mapped;
		changed |= old != mapped;
	}
	return changed;
}

bool RCSceneRuntime_CompactMaterials (rc_scene_runtime_t *runtime,
	radiance_quake_materials_t *materials, bool *changed)
{
	if (changed) *changed = false;
	if (!runtime || !materials || !runtime->valid || materials->count > UINT32_MAX ||
		(materials->count && !materials->entries) || (runtime->scene.triangle_count && !runtime->scene.triangles) ||
		materials->count > SIZE_MAX / sizeof (uint32_t)) return false;
	if (!materials->count)
	{
		if (runtime->scene.triangle_count || runtime->static_scene.triangle_count) return false;
		for (uint32_t i = 0; i < runtime->entity_cache_count; ++i)
			if (runtime->entity_cache[i].geometry.triangle_count) return false;
		return true;
	}
	uint32_t *remap = malloc (materials->count * sizeof (*remap));
	if (!remap) return false;
	for (size_t i = 0; i < materials->count; ++i) remap[i] = UINT32_MAX;
	for (uint32_t i = 0; i < runtime->scene.triangle_count; ++i)
	{
		uint32_t id = runtime->scene.triangles[i].metadata[0];
		if (id >= materials->count) { free (remap); return false; }
		remap[id] = 0;
	}
	for (size_t i = 0; i < materials->count; ++i)
		if (materials->entries[i].pinned) remap[i] = 0;
	uint32_t used = 0;
	for (size_t i = 0; i < materials->count; ++i)
		if (remap[i] != UINT32_MAX) remap[i] = used++;
	if (!RCSceneRuntime_MaterialsPresent (&runtime->static_scene, remap, materials->count)) { free (remap); return false; }
	if (!RCSceneRuntime_MaterialsPresent (&runtime->base_scene, remap, materials->count) ||
		!RCSceneRuntime_MaterialsPresent (&runtime->mutable_scene, remap, materials->count)) { free (remap); return false; }
	for (uint32_t i = 0; i < runtime->entity_cache_count; ++i)
		if (!RCSceneRuntime_MaterialsPresent (&runtime->entity_cache[i].geometry, remap, materials->count)) { free (remap); return false; }
	if (used != materials->count)
	{
		RCSceneRuntime_RemapScene (&runtime->scene, remap);
		bool world_changed = RCSceneRuntime_RemapScene (&runtime->static_scene, remap);
		bool base_changed = RCSceneRuntime_RemapScene (&runtime->base_scene, remap);
		if ((runtime->base_valid ? base_changed : world_changed) && ++runtime->base_content_generation == 0)
			runtime->base_content_generation = 1;
		RCSceneRuntime_RemapScene (&runtime->mutable_scene, remap);
		for (uint32_t i = 0; i < runtime->entity_cache_count; ++i)
			if (RCSceneRuntime_RemapScene (&runtime->entity_cache[i].geometry, remap))
				RCScene_SummarizeEntity (&runtime->entity_cache[i]);
		for (size_t i = 0; i < materials->count; ++i)
			if (remap[i] != UINT32_MAX) materials->entries[remap[i]] = materials->entries[i];
		materials->count = used;
		if (changed) *changed = true;
	}
	free (remap);
	return true;
}
