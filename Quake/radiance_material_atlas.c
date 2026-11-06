#include "radiance_material_atlas.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t layer, x, y; } radiance_placement_t;

void RadianceMaterialAtlas_Clear (radiance_material_atlas_t *atlas)
{
	if (!atlas) return;
	free (atlas->materials);
	free (atlas->rgba);
	memset (atlas, 0, sizeof (*atlas));
}

bool RadianceMaterialAtlas_Build (radiance_material_atlas_t *atlas, const radiance_material_source_t *sources,
	size_t count, uint32_t max_dimension, uint32_t max_layers, size_t max_bytes)
{
	radiance_material_atlas_t pending = {0};
	radiance_placement_t *placements = NULL;
	if (!atlas || (!sources && count) || !count || !max_dimension || !max_layers ||
		count > SIZE_MAX / sizeof (*pending.materials) || count > SIZE_MAX / (2 * sizeof (*placements))) return false;
	/* Choose page dimensions after inspecting the largest source. */
	pending.width = pending.height = 1;
	pending.material_count = count;
	for (size_t i = 0; i < count; ++i)
	{
		const radiance_material_image_t *images[2] = {&sources[i].albedo, &sources[i].emission};
		if (!sources[i].sky && !images[0]->rgba) return false;
		for (int j = 0; j < 2; ++j)
		{
			const radiance_material_image_t *image = images[j];
			if (!image->rgba) { if (image->width || image->height) return false; continue; }
			if (!image->width || !image->height || image->width > max_dimension || image->height > max_dimension) return false;
			if (image->width > pending.width) pending.width = image->width;
			if (image->height > pending.height) pending.height = image->height;
		}
	}
	/* A power-of-two page (at least 8 texels when permitted) allows smaller
	 * images to share space without exploding dimensions to their LCM. */
	uint32_t page_floor = max_dimension < 8 ? max_dimension : 8;
	uint32_t dimensions[2] = {pending.width, pending.height};
	for (int axis = 0; axis < 2; ++axis)
	{
		uint32_t rounded = page_floor;
		while (rounded < dimensions[axis])
		{
			if (rounded > max_dimension / 2) { rounded = max_dimension; break; }
			rounded *= 2;
		}
		dimensions[axis] = rounded;
	}
	pending.width = dimensions[0]; pending.height = dimensions[1];
	placements = calloc (count * 2, sizeof (*placements));
	if (!placements) return false;
	uint32_t x = 0, y = 0, row_height = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const radiance_material_image_t *images[2] = {&sources[i].albedo, &sources[i].emission};
		for (int j = 0; j < 2; ++j)
		{
			const radiance_material_image_t *image = images[j];
			if (!image->rgba) continue;
			if (!pending.layers) pending.layers = 1;
			if (image->width > pending.width - x) { x = 0; y += row_height; row_height = 0; }
			if (image->height > pending.height - y)
			{
				if (pending.layers >= max_layers) goto fail;
				++pending.layers; x = y = row_height = 0;
			}
			placements[2 * i + j] = (radiance_placement_t){pending.layers - 1, x, y};
			x += image->width;
			if (image->height > row_height) row_height = image->height;
		}
	}
	/* A sky-only world has no texels to pack, but the Vulkan descriptor still
	 * requires a valid image. Supply a single transparent texel instead of
	 * rejecting an otherwise valid scene. Sky hits never sample this image. */
	if (!pending.layers) { pending.width = pending.height = pending.layers = 1; }
	if (pending.width > SIZE_MAX / pending.height ||
		(size_t)pending.width * pending.height > SIZE_MAX / 4 ||
		(size_t)pending.width * pending.height * 4 > SIZE_MAX / pending.layers) goto fail;
	pending.bytes = (size_t)pending.width * pending.height * 4 * pending.layers;
	if (pending.bytes > max_bytes) goto fail;
	pending.materials = calloc (count, sizeof (*pending.materials));
	pending.rgba = calloc (1, pending.bytes);
	if (!pending.materials || !pending.rgba) goto fail;
	const size_t layer_bytes = (size_t)pending.width * pending.height * 4;
	for (size_t i = 0; i < count; ++i)
	{
		radiance_material_t *material = &pending.materials[i];
		const radiance_material_image_t *images[2] = {&sources[i].albedo, &sources[i].emission};
		if (!sources[i].sky) material->albedo_alpha[0] = material->albedo_alpha[1] = material->albedo_alpha[2] = material->albedo_alpha[3] = 1;
		for (int j = 0; j < 2; ++j)
		{
			const radiance_material_image_t *image = images[j];
			if (!image->rgba) continue;
			radiance_placement_t placement = placements[2 * i + j];
			uint32_t *rect = j ? material->emission_rect : material->albedo_rect;
			rect[0] = placement.x; rect[1] = placement.y; rect[2] = image->width; rect[3] = image->height;
			material->layers_flags[j] = placement.layer;
			material->layers_flags[2] |= j ? RADIANCE_MATERIAL_EMISSION_TEXTURE : RADIANCE_MATERIAL_ALBEDO_TEXTURE;
			if (j) material->emission[0] = material->emission[1] = material->emission[2] = material->emission[3] = 1;
			uint8_t *dest = pending.rgba + (size_t)placement.layer * layer_bytes + ((size_t)placement.y * pending.width + placement.x) * 4;
			for (uint32_t row = 0; row < image->height; ++row)
				memcpy (dest + (size_t)row * pending.width * 4, image->rgba + (size_t)row * image->width * 4, (size_t)image->width * 4);
		}
	}
	free (placements);
	RadianceMaterialAtlas_Clear (atlas);
	*atlas = pending;
	return true;
fail:
	free (placements);
	RadianceMaterialAtlas_Clear (&pending);
	return false;
}
