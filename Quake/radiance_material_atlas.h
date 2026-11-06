/* Bounded texture-array packing for real radiance materials. */
#ifndef QUIVER_RADIANCE_MATERIAL_ATLAS_H
#define QUIVER_RADIANCE_MATERIAL_ATLAS_H
#include "radiance_lights.h"

typedef struct
{
	const uint8_t *rgba;
	uint32_t width, height;
} radiance_material_image_t;

typedef struct
{
	radiance_material_image_t albedo, emission;
	/* Sky is terminal and requires no texture read; other materials must
	 * provide actual albedo pixels. */
	bool sky;
} radiance_material_source_t;

typedef struct
{
	radiance_material_t *materials;
	uint8_t *rgba;
	size_t material_count, bytes;
	uint32_t width, height, layers;
} radiance_material_atlas_t;

/* Builds an sRGB RGBA texture array. Textures occupy bounded rectangles in array layers; original pixels and
 * repeat-wrapped nearest sampling are preserved exactly.
 * On overflow, missing inputs, or a caller-supplied resource limit, return
 * false without replacing the previous atlas; the renderer must fall back. */
bool RadianceMaterialAtlas_Build (radiance_material_atlas_t *atlas, const radiance_material_source_t *sources,
	size_t count, uint32_t max_dimension, uint32_t max_layers, size_t max_bytes);
void RadianceMaterialAtlas_Clear (radiance_material_atlas_t *atlas);

#endif
