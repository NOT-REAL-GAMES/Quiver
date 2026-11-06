/* Quake texture-source adapter for the radiance material atlas. Include
 * quakedef.h before this header for texture_t, gltexture_t and entity_t. */
#ifndef QUIVER_RADIANCE_MATERIALS_QUAKE_H
#define QUIVER_RADIANCE_MATERIALS_QUAKE_H
#include "radiance_material_atlas.h"

typedef struct
{
	const texture_t *brush;
	const gltexture_t *albedo, *emission;
	int brush_frame;
	bool sky;
} radiance_quake_material_t;

typedef struct radiance_quake_materials_s
{
	radiance_quake_material_t *entries;
	size_t count, capacity;
	int brush_frame;
	struct radiance_quake_atlas_cache_s *atlas_cache;
} radiance_quake_materials_t;

void RadianceQuakeMaterials_Init (radiance_quake_materials_t *materials);
void RadianceQuakeMaterials_Clear (radiance_quake_materials_t *materials);
/* Set this before extracting a brush entity; world geometry uses frame 0.
 * Like R_TextureAnimation, all nonzero values select the same alternate. */
void RadianceQuakeMaterials_SetBrushFrame (radiance_quake_materials_t *materials, int frame);
/* Scene extraction callbacks. IDs are stable until Clear or an explicit
 * scene compaction; UINT32_MAX means unavailable and requires fallback. */
uint32_t RadianceQuakeMaterials_Brush (const texture_t *texture, void *user);
uint32_t RadianceQuakeMaterials_Alias (const entity_t *entity, uint32_t surface,
	const gltexture_t *texture, const gltexture_t *fullbright, void *user);
/* Changes when a brush animation, skin, texture reload or fullbright setting
 * changes. A caller can avoid rebuilding unchanged CPU/GPU atlas snapshots. */
uint64_t RadianceQuakeMaterials_Revision (const radiance_quake_materials_t *materials);
/* Loads real gltexture source data and builds a transactional, bounded atlas.
 * Images that cannot be read or represented cause false, not synthetic colors. */
bool RadianceQuakeMaterials_BuildAtlas (const radiance_quake_materials_t *materials,
	radiance_material_atlas_t *atlas, uint32_t max_dimension, uint32_t max_layers, size_t max_bytes);
/* Pack primary and alternate brush animation frames once. Ordinary selection
 * changes update only the logical material records, preserving atlas pixels.
 * Checks inactive texture revisions on every call. Inventory growth reuses
 * unchanged decoded texels from the old atlas and loads only new/reloaded
 * records. Compaction may keep unused
 * packed pixels until the next inventory rebuild. Failure preserves the prior
 * atlas/cache; Clear releases the registry-owned cache. */
bool RadianceQuakeMaterials_UpdateAtlas (radiance_quake_materials_t *materials,
	radiance_material_atlas_t *atlas, uint32_t max_dimension, uint32_t max_layers,
	size_t max_bytes, bool *pixels_changed);

#endif
