/* Isolated Quake texture-source/material adapter test with fake file/image
 * providers. Exercises actual indexed/RGBA bytes, alpha and animation. */
#include "quakedef.h"
#include "radiance_materials_quake.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

cvar_t gl_fullbrights = {.name = "gl_fullbrights", .string = "1", .flags = CVAR_NONE};
cvar_t gl_nocolors = {.name = "gl_nocolors", .string = "0", .flags = CVAR_NONE};
client_state_t cl;
viddef_t vid;
gltexture_t *playertextures[MAX_SCOREBOARD];
unsigned int d_8to24table[256];
static uint8_t file_bytes[1024];
static size_t file_size;
/* Simulate a pack member whose bytes begin after an unrelated prefix. */
static uint8_t pack_bytes[1040];
static size_t pack_pos, pack_size;
static bool animated;

texture_t *R_TextureAnimation (texture_t *base, int frame)
{
	return (animated || frame) && base->anim_next ? base->anim_next : base;
}

qfilesize_t COM_OpenFile (const char *name, int *handle, unsigned int *path_id)
{
	(void)path_id;
	if (strcmp (name, "fixture") && strcmp (name, "embedded")) { *handle = -1; return -1; }
	assert (file_size <= sizeof (file_bytes));
	memset (pack_bytes, 0xee, 13);
	memcpy (pack_bytes + 13, file_bytes, file_size);
	pack_pos = 13; pack_size = 13 + file_size; *handle = 1;
	return (qfilesize_t)file_size;
}
qfileofs_t Sys_FilePos (int handle) { assert (handle == 1); return (qfileofs_t)pack_pos; }
int Sys_FileSeek (int handle, qfileofs_t offset)
{
	assert (handle == 1);
	if (offset < 0 || (uint64_t)offset > pack_size) return -1;
	pack_pos = (size_t)offset; return 0;
}
int Sys_FileRead (int handle, void *data, int size)
{
	assert (handle == 1 && size >= 0);
	size_t n = (size_t)size < pack_size - pack_pos ? (size_t)size : pack_size - pack_pos;
	memcpy (data, pack_bytes + pack_pos, n); pack_pos += n; return (int)n;
}
void Sys_FileClose (int handle) { assert (handle == 1); }

byte *Image_LoadImage (const char *name, int *width, int *height, enum srcformat *format, unsigned int path_id)
{
	(void)width; (void)height; (void)format; (void)path_id;
	if (strcmp (name, "external")) return NULL;
	uint8_t *pixels = malloc (16); assert (pixels);
	for (int i = 0; i < 16; ++i) pixels[i] = (uint8_t)(i * 11);
	*width = *height = 2; *format = SRC_RGBA;
	return pixels;
}
void Mem_Free (const void *pointer) { free ((void *)pointer); }

static const uint8_t *texel (const radiance_material_atlas_t *atlas, size_t index, bool emission, unsigned x, unsigned y)
{
	const radiance_material_t *m = &atlas->materials[index];
	const uint32_t *r = emission ? m->emission_rect : m->albedo_rect;
	uint32_t layer = m->layers_flags[emission ? 1 : 0];
	assert (x < r[2] && y < r[3]);
	return atlas->rgba + (((size_t)layer * atlas->height + r[1] + y) * atlas->width + r[0] + x) * 4;
}

int main (void)
{
	for (int i = 0; i < 256; ++i)
	{
		uint8_t *rgba = (uint8_t *)&d_8to24table[i];
		rgba[0] = (uint8_t)i; rgba[1] = (uint8_t)(255 - i); rgba[2] = (uint8_t)(i / 2); rgba[3] = 255;
	}
	gl_fullbrights.value = 1;
	uint8_t pixels[] = {1, 224, 255, 10};
	gltexture_t base = {0}, emission = {0};
	base.source_width = base.source_height = 2; base.source_format = SRC_INDEXED;
	base.source_offset = (src_offset_t)(uintptr_t)pixels; base.flags = TEXPREF_NOBRIGHT | TEXPREF_ALPHA;
	base.shirt = base.pants = -1;
	emission = base; emission.flags = TEXPREF_FULLBRIGHT | TEXPREF_ALPHA;
	texture_t brush = {0}; brush.gltexture = &base; brush.fullbright = &emission; brush.type = TEXTYPE_DEFAULT;
	radiance_quake_materials_t materials;
	RadianceQuakeMaterials_Init (&materials);
	assert (RadianceQuakeMaterials_Brush (&brush, &materials) == 0);
	assert (RadianceQuakeMaterials_Brush (&brush, &materials) == 0); /* stable IDs */
	uint64_t revision = RadianceQuakeMaterials_Revision (&materials);
	radiance_material_atlas_t atlas = {0};
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (atlas.width == 8 && atlas.height == 8 && atlas.layers == 1);
	assert (texel (&atlas, 0, false, 1, 0)[0] == 0 && texel (&atlas, 0, false, 1, 0)[3] == 255);
	assert (texel (&atlas, 0, false, 0, 1)[3] == 0); /* transparent palette index 255 */
	assert (texel (&atlas, 0, true, 1, 0)[0] == 224 && texel (&atlas, 0, true, 1, 0)[1] == 31); /* emitting texel */
	assert (texel (&atlas, 0, true, 0, 0)[0] == 0 && texel (&atlas, 0, true, 0, 0)[1] == 0); /* no emission from ordinary texel */
	gl_fullbrights.value = 0;
	assert (RadianceQuakeMaterials_Revision (&materials) != revision);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (atlas.layers == 1 && texel (&atlas, 0, false, 1, 0)[0] == 224);
	gl_fullbrights.value = 1;

	/* An animated brush resolves its current frame at atlas-refresh time. */
	uint8_t alt_pixels[] = {7, 7, 7, 7};
	gltexture_t alternate = base; alternate.flags = 0; alternate.source_offset = (src_offset_t)(uintptr_t)alt_pixels;
	texture_t second = {0}; second.gltexture = &alternate; brush.anim_next = &second;
	revision = RadianceQuakeMaterials_Revision (&materials); animated = true;
	assert (RadianceQuakeMaterials_Revision (&materials) != revision);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (atlas.layers == 1 && texel (&atlas, 0, false, 0, 0)[0] == 7);
	animated = false;

	/* Read indexed BSP/MDL-style source from a file at its recorded offset. */
	memset (file_bytes, 0, sizeof (file_bytes)); file_bytes[0] = 99; memcpy (file_bytes + 1, pixels, sizeof (pixels)); file_size = 5;
	gltexture_t file_texture = base; memcpy (file_texture.source_file, "fixture", sizeof ("fixture"));
	file_texture.source_offset = 1; file_texture.flags = TEXPREF_ALPHA;
	entity_t entity = {0};
	assert (RadianceQuakeMaterials_Alias (&entity, 0, &file_texture, NULL, &materials) == 1);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (texel (&atlas, 1, false, 0, 0)[0] == 1 && texel (&atlas, 1, false, 0, 1)[3] == 0);
	/* This source exists only in a memory-backed pack. Its file offset is
	 * relative to the member, not the beginning of the enclosing pack. */
	memcpy (file_texture.source_file, "embedded", sizeof ("embedded"));
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (texel (&atlas, 1, false, 0, 0)[0] == 1 && texel (&atlas, 1, false, 0, 1)[3] == 0);
	memcpy (file_texture.source_file, "fixture", sizeof ("fixture"));

	/* External RGBA source images retain their real colors and alpha. */
	gltexture_t external = {0}; memcpy (external.source_file, "external", sizeof ("external"));
	external.source_width = external.source_height = 2; external.source_format = SRC_RGBA;
	external.shirt = external.pants = -1;
	assert (RadianceQuakeMaterials_Alias (&entity, 0, &external, NULL, &materials) == 2);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (texel (&atlas, 2, false, 0, 0)[3] == 33);
	/* Lightmap data cannot be passed off as a runtime material source. */
	file_texture.source_format = SRC_LIGHTMAP;
	uint8_t *last_valid = atlas.rgba;
	assert (!RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096) && atlas.rgba == last_valid);
	file_texture.source_format = SRC_INDEXED;
	/* Failed source loads preserve the prior atlas rather than invent colors. */
	external.source_width = 1;
	uint8_t *old = atlas.rgba;
	assert (!RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096) && atlas.rgba == old);
	external.source_width = 2;

	/* Valve BSP palette follows the four mip levels. */
	memset (file_bytes, 0, sizeof (file_bytes));
	for (int i = 0; i < 64; ++i) file_bytes[1 + i] = i == 1 ? 255 : 224;
	file_bytes[1 + 85] = 0; file_bytes[1 + 86] = 1; /* 256 colors, little endian */
	file_bytes[1 + 87 + 224 * 3] = 200;
	file_bytes[1 + 87 + 224 * 3 + 1] = 100;
	file_bytes[1 + 87 + 224 * 3 + 2] = 50;
	file_size = 1 + 85 + 2 + 256 * 3;
	gltexture_t valve = {0}; memcpy (valve.source_file, "fixture", sizeof ("fixture"));
	valve.source_offset = 1; valve.source_width = valve.source_height = 8; valve.source_format = SRC_INDEXED_PALETTE;
	valve.flags = TEXPREF_NOBRIGHT | TEXPREF_ALPHA; valve.shirt = valve.pants = -1;
	gltexture_t valve_emission = valve; valve_emission.flags = TEXPREF_FULLBRIGHT | TEXPREF_ALPHA;
	assert (RadianceQuakeMaterials_Alias (&entity, 0, &valve, &valve_emission, &materials) == 3);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (texel (&atlas, 3, false, 0, 0)[0] == 0 && texel (&atlas, 3, false, 0, 0)[3] == 255);
	assert (texel (&atlas, 3, true, 0, 0)[0] == 200 && texel (&atlas, 3, true, 0, 0)[1] == 100);
	assert (texel (&atlas, 3, false, 1, 0)[3] == 0 && texel (&atlas, 3, true, 1, 0)[3] == 0);
	/* The memory-backed pack is only available via COM_OpenFile. Rebuild
	 * both palette sources from its nonzero member origin. */
	memcpy (valve.source_file, "embedded", sizeof ("embedded"));
	memcpy (valve_emission.source_file, "embedded", sizeof ("embedded"));
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (texel (&atlas, 3, true, 0, 0)[0] == 200 && texel (&atlas, 3, true, 0, 0)[1] == 100);
	/* A truncated member cannot read palette data belonging to the next pack
	 * entry, and must preserve the previously built atlas. */
	last_valid = atlas.rgba; --file_size;
	assert (!RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096) && atlas.rgba == last_valid);
	++file_size;
	RadianceMaterialAtlas_Clear (&atlas); RadianceQuakeMaterials_Clear (&materials);
	/* Quake warp textures have no separate fullbright image: recover only
	 * emitting palette indices from their indexed source. */
	RadianceQuakeMaterials_Init (&materials);
	uint8_t lava_pixels[] = {1, 224, 225, 3};
	uint8_t water_pixels[] = {1, 2, 3, 4};
	gltexture_t lava_image = base, water_image = base;
	lava_image.flags = water_image.flags = 0;
	lava_image.source_offset = (src_offset_t)(uintptr_t)lava_pixels;
	water_image.source_offset = (src_offset_t)(uintptr_t)water_pixels;
	texture_t lava = {0}, water = {0}, rgba_lava = {0};
	lava.type = TEXTYPE_LAVA; lava.gltexture = &lava_image;
	water.type = TEXTYPE_WATER; water.gltexture = &water_image;
	rgba_lava.type = TEXTYPE_LAVA; rgba_lava.gltexture = &external;
	assert (RadianceQuakeMaterials_Brush (&lava, &materials) == 0);
	assert (RadianceQuakeMaterials_Brush (&water, &materials) == 1);
	assert (RadianceQuakeMaterials_Brush (&rgba_lava, &materials) == 2);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (atlas.materials[0].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE);
	assert (texel (&atlas, 0, true, 0, 0)[0] == 0 && texel (&atlas, 0, true, 1, 0)[0] == 224);
	assert (!(atlas.materials[1].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE));
	assert (!(atlas.materials[2].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE));
	/* A high-resolution replacement has no indexed gltexture. Recover the
	 * original BSP lava mask, keeping the replacement's albedo and size.
	 * The two rectangles may have different dimensions: normalized UVs
	 * address the same relative position in each. */
	uint8_t original_lava[] = {1, 2, 224, 225, 1, 2, 224, 225,
		1, 2, 224, 225, 1, 2, 224, 225};
	revision = RadianceQuakeMaterials_Revision (&materials);
	rgba_lava.width = rgba_lava.height = 4;
	rgba_lava.source_offset = (src_offset_t)(uintptr_t)original_lava;
	assert (RadianceQuakeMaterials_Revision (&materials) != revision);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (atlas.materials[2].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE);
	assert (atlas.materials[2].albedo_rect[2] == 2 && atlas.materials[2].emission_rect[2] == 4);
	assert (texel (&atlas, 2, false, 0, 0)[3] == 33);
	assert (texel (&atlas, 2, true, 0, 0)[0] == 0 && texel (&atlas, 2, true, 2, 0)[0] == 224);
	/* BSP-backed texture offsets are relative to the PAK member. */
	file_bytes[0] = 99;
	memcpy (file_bytes + 1, original_lava, sizeof (original_lava));
	file_size = sizeof (original_lava) + 1;
	memcpy (rgba_lava.source_file, "embedded", sizeof ("embedded"));
	rgba_lava.source_offset = 1;
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (texel (&atlas, 2, true, 2, 0)[0] == 224);
	rgba_lava.source_file[0] = 0;
	rgba_lava.source_offset = (src_offset_t)(uintptr_t)original_lava;
	/* An explicit glow image always wins over the original palette mask. */
	rgba_lava.fullbright = &emission;
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (texel (&atlas, 2, true, 0, 1)[3] == 0);
	rgba_lava.fullbright = NULL;
	/* Valve textures use their own palettes and cannot use Quake's indices. */
	rgba_lava.palette = true;
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (!(atlas.materials[2].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE));
	rgba_lava.palette = false;
	/* A missing optional BSP source does not invalidate all materials. */
	memcpy (rgba_lava.source_file, "missing", sizeof ("missing"));
	rgba_lava.source_offset = 1;
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (!(atlas.materials[2].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE));
	rgba_lava.source_file[0] = 0;
	rgba_lava.source_offset = (src_offset_t)(uintptr_t)original_lava;
	revision = RadianceQuakeMaterials_Revision (&materials);
	gl_fullbrights.value = 0;
	assert (RadianceQuakeMaterials_Revision (&materials) != revision);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 16, 4096));
	assert (!(atlas.materials[0].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE));
	assert (!(atlas.materials[2].layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE));
	gl_fullbrights.value = 1;
	RadianceMaterialAtlas_Clear (&atlas); RadianceQuakeMaterials_Clear (&materials);
	/* A world consisting entirely of sky surfaces has no source texture to
	 * load, but must still produce valid atlas and material descriptors. */
	RadianceQuakeMaterials_Init (&materials);
	texture_t sky = {0}; sky.type = TEXTYPE_SKY;
	assert (RadianceQuakeMaterials_Brush (&sky, &materials) == 0);
	assert (RadianceQuakeMaterials_BuildAtlas (&materials, &atlas, 64, 1, 4));
	assert (atlas.width == 1 && atlas.height == 1 && atlas.layers == 1 && atlas.bytes == 4 &&
		atlas.material_count == 1 && atlas.materials[0].layers_flags[2] == 0);
	RadianceMaterialAtlas_Clear (&atlas); RadianceQuakeMaterials_Clear (&materials);
	puts ("PASS: real indexed/RGBA sources, fullbright masks, alpha, animation and transactional fallback");
	return 0;
}
