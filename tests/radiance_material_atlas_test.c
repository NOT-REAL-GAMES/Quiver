#include "radiance_material_atlas.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t *pixel (const radiance_material_atlas_t *atlas, uint32_t layer, const uint32_t rect[4], unsigned x, unsigned y)
{
	assert (x < rect[2] && y < rect[3] && layer < atlas->layers);
	return atlas->rgba + (((size_t)layer * atlas->height + rect[1] + y) * atlas->width + rect[0] + x) * 4;
}

int main (void)
{
	const uint8_t albedo[] = {255,0,0,0, 0,255,0,255, 0,0,255,128, 255,255,255,255};
	const uint8_t emission[] = {255,128,0,255, 0,0,0,0};
	const uint8_t other[] = {1,2,3,255, 4,5,6,255, 7,8,9,255};
	radiance_material_source_t sources[3] = {0};
	sources[0].albedo = (radiance_material_image_t){albedo, 2, 2};
	sources[0].emission = (radiance_material_image_t){emission, 1, 2};
	sources[1].albedo = (radiance_material_image_t){other, 3, 1};
	sources[2].sky = true;
	radiance_material_atlas_t atlas = {0};
	assert (RadianceMaterialAtlas_Build (&atlas, sources, 3, 64, 1, 4096));
	assert (atlas.width == 8 && atlas.height == 8 && atlas.layers == 1 && atlas.bytes == 256);
	assert (atlas.material_count == 3 && atlas.materials[0].layers_flags[0] == 0 && atlas.materials[0].layers_flags[1] == 0);
	assert (atlas.materials[0].layers_flags[2] == (RADIANCE_MATERIAL_ALBEDO_TEXTURE | RADIANCE_MATERIAL_EMISSION_TEXTURE));
	assert (atlas.materials[2].layers_flags[2] == 0);
	for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 2; ++x)
		assert (!memcmp (pixel (&atlas, 0, atlas.materials[0].albedo_rect, x, y), albedo + (y * 2 + x) * 4, 4));
	for (unsigned y = 0; y < 2; ++y)
		assert (!memcmp (pixel (&atlas, 0, atlas.materials[0].emission_rect, 0, y), emission + y * 4, 4));
	for (unsigned x = 0; x < 3; ++x)
		assert (!memcmp (pixel (&atlas, 0, atlas.materials[1].albedo_rect, x, 0), other + x * 4, 4));
	/* Failed repacks preserve the last complete snapshot. */
	uint8_t *old = atlas.rgba;
	assert (!RadianceMaterialAtlas_Build (&atlas, sources, 3, 2, 8, 4096));
	assert (atlas.rgba == old && atlas.width == 8);
	assert (!RadianceMaterialAtlas_Build (&atlas, sources, 3, 64, 1, 255));
	sources[1].albedo.rgba = NULL;
	assert (!RadianceMaterialAtlas_Build (&atlas, sources, 3, 64, 8, 4096));
	/* Multiple packed pages preserve complete source bytes and respect the
	 * device layer limit even when every rectangle fills its own page. */
	uint8_t full[4 * 4 * 4]; memset (full, 77, sizeof (full));
	radiance_material_source_t pages[3] = {0};
	for (int i = 0; i < 3; ++i) pages[i].albedo = (radiance_material_image_t){full, 4, 4};
	assert (!RadianceMaterialAtlas_Build (&atlas, pages, 3, 4, 2, 1024) && atlas.rgba == old);
	assert (RadianceMaterialAtlas_Build (&atlas, pages, 3, 4, 3, 1024));
	assert (atlas.layers == 3 && atlas.bytes == 192);
	for (int i = 0; i < 3; ++i) assert (!memcmp (pixel (&atlas, i, atlas.materials[i].albedo_rect, 0, 0), full, 4));
	/* A sky-only map still needs a valid descriptor image, even though sky
	 * tracing uses the sky flag and never samples an atlas rectangle. */
	radiance_material_source_t skies[2] = {{.sky = true}, {.sky = true}};
	old = atlas.rgba;
	assert (!RadianceMaterialAtlas_Build (&atlas, skies, 2, 64, 1, 3) && atlas.rgba == old);
	assert (RadianceMaterialAtlas_Build (&atlas, skies, 2, 64, 1, 4));
	assert (atlas.width == 1 && atlas.height == 1 && atlas.layers == 1 && atlas.bytes == 4 && atlas.material_count == 2);
	for (unsigned i = 0; i < 4; ++i) assert (atlas.rgba[i] == 0);
	for (unsigned i = 0; i < 2; ++i) assert (atlas.materials[i].layers_flags[2] == 0);
	RadianceMaterialAtlas_Clear (&atlas);
	assert (!atlas.rgba && !atlas.materials && !atlas.bytes);
	puts ("PASS: packed texture pixels and alpha; bounded failures are transactional");
	return 0;
}
