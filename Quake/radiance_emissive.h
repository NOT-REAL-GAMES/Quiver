/* Approximate actual emissive triangles by bounded surface light proxies. */
#ifndef QUIVER_RADIANCE_EMISSIVE_H
#define QUIVER_RADIANCE_EMISSIVE_H
#include "radiance_scene.h"
#include "radiance_material_atlas.h"

typedef struct {
	size_t emissive_surfaces, emitted_lights, omitted_lights;
} radiance_emissive_stats_t;
typedef struct radiance_emissive_cache_s radiance_emissive_cache_t;
typedef struct { uint64_t material_rebuilds, static_rebuilds, dynamic_rebuilds, static_triangle_scans; } radiance_emissive_cache_counters_t;
radiance_emissive_cache_t *Radiance_EmissiveCacheCreate(void);
void Radiance_EmissiveCacheDestroy(radiance_emissive_cache_t *cache);
void Radiance_EmissiveCacheReset(radiance_emissive_cache_t *cache);
void Radiance_EmissiveCacheCounters(const radiance_emissive_cache_t *cache, radiance_emissive_cache_counters_t *out);
/* Persistent scene triangles are the first static_count entries. Reset on
 * changed persistent geometry/material IDs. scene_rebuilt requests restoring
 * proxy flags after that prefix's flags were regenerated; keep it false when
 * composition preserves the prefix during mutable-only changes.
 * dynamic_changed indicates changed mutable vertices/materials. */
bool Radiance_BuildEmissiveLightsCached(radiance_emissive_cache_t *cache, rc_scene_t *scene,
	uint32_t static_count, const radiance_material_atlas_t *atlas, uint64_t atlas_revision,
	bool scene_rebuilt, bool dynamic_changed, const float camera[3], radiance_light_t *out,
	size_t capacity, radiance_emissive_stats_t *stats, bool *changed);
/* Suppress automatic proxies for material IDs represented by explicit map
 * surface lights. Those triangles are still marked as represented so their
 * texture emission is not counted twice by cascade hits. */
bool Radiance_BuildEmissiveLightsCachedFiltered(radiance_emissive_cache_t *cache, rc_scene_t *scene,
	uint32_t static_count, const radiance_material_atlas_t *atlas, uint64_t atlas_revision,
	bool scene_rebuilt, bool dynamic_changed, const uint8_t *represented_materials, const float camera[3],
	radiance_light_t *out, size_t capacity, radiance_emissive_stats_t *stats, bool *changed);

/* Uses scene material IDs and the collector's actual emission atlas. Output
 * lights are sorted by contribution near the camera; groups beyond capacity
 * retain their ray-traced emission. Marks selected triangles as represented
 * by direct light so a cascade hit does not count their emission twice.
 * Reports whether flags changed, requiring scene GPU buffer refresh. Failure
 * leaves triangle flags unchanged and does not publish partial output. */
bool Radiance_BuildEmissiveLights(rc_scene_t *scene, const radiance_material_atlas_t *atlas,
	const float camera[3], radiance_light_t *out, size_t capacity, radiance_emissive_stats_t *stats, bool *changed);
#endif
