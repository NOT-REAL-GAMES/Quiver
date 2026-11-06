/* Optional read-only smoke test against a local Quake PAK. Exercises real BSP
 * entity lumps and indexed texture sources without loading the engine/config. */
#include "quakedef.h"
#include "radiance_coordinator.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cvar_t gl_fullbrights = {.name = "gl_fullbrights", .string = "1", .flags = CVAR_NONE};
cvar_t gl_nocolors = {.name = "gl_nocolors", .string = "0", .flags = CVAR_NONE};
client_state_t cl;
viddef_t vid;
gltexture_t *playertextures[MAX_SCOREBOARD];
unsigned int d_8to24table[256];
static const char *pak_path;
static long bsp_start, bsp_size;
static size_t upload_count;
static FILE *source_file;
texture_t *R_TextureAnimation (texture_t *texture, int frame) { (void)frame; return texture; }
qfilesize_t COM_OpenFile (const char *name, int *handle, unsigned int *path_id)
{
	(void)path_id;
	*handle = -1;
	if (strcmp (name, "fixture.bsp")) return -1;
	source_file = fopen (pak_path, "rb");
	if (!source_file || fseek (source_file, bsp_start, SEEK_SET))
	{ if (source_file) fclose (source_file); source_file = NULL; return -1; }
	*handle = 1;
	return bsp_size;
}
qfileofs_t Sys_FilePos (int handle) { assert (handle == 1 && source_file); return ftell (source_file); }
int Sys_FileSeek (int handle, qfileofs_t offset) { assert (handle == 1 && source_file); return fseek (source_file, (long)offset, SEEK_SET); }
int Sys_FileRead (int handle, void *data, int size) { assert (handle == 1 && source_file && size >= 0); return (int)fread (data, 1, (size_t)size, source_file); }
void Sys_FileClose (int handle) { assert (handle == 1 && source_file); fclose (source_file); source_file = NULL; }
byte *Image_LoadImage (const char *name, int *width, int *height, enum srcformat *format, unsigned int path_id)
{ (void)name; (void)width; (void)height; (void)format; (void)path_id; return NULL; }
void Mem_Free (const void *pointer) { free ((void *)pointer); }
bool RadianceInputs_Update (radiance_gpu_inputs_t *gpu, uint32_t frame, const radiance_input_snapshot_t *s)
{
	(void)gpu; (void)frame;
	assert (s->material_count > 0 && s->atlas_width && s->atlas_height && s->atlas_layers && s->atlas_rgba);
	assert (s->grid_offsets[s->grid_offset_count - 1] == s->grid_index_count);
	++upload_count; return true;
}
static uint32_t u32 (const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static bool span (size_t offset, size_t length, size_t size) { return offset <= size && length <= size - offset; }
static bool find_file (const char *name, long *offset, long *length)
{
	FILE *f = fopen (pak_path, "rb"); uint8_t header[12], entry[64];
	if (!f) return false;
	bool ok = fread (header, 1, sizeof (header), f) == sizeof (header) && !memcmp (header, "PACK", 4);
	uint32_t size = ok ? u32 (header + 8) : 0;
	if (!ok || size % 64 || fseek (f, u32 (header + 4), SEEK_SET)) { fclose (f); return false; }
	ok = false;
	for (uint32_t i = 0; i < size / 64 && fread (entry, 1, 64, f) == 64; ++i)
	{
		if (memchr (entry, 0, 56) && !strcmp ((const char *)entry, name))
		{ *offset = (long)u32 (entry + 56); *length = (long)u32 (entry + 60); ok = true; break; }
	}
	fclose (f); return ok;
}
static uint8_t *read_entry (const char *name, long *length, long *start)
{
	if (!find_file (name, start, length) || *length <= 0) return NULL;
	FILE *f = fopen (pak_path, "rb"); if (!f || fseek (f, *start, SEEK_SET)) { if (f) fclose (f); return NULL; }
	uint8_t *data = malloc ((size_t)*length);
	if (!data || fread (data, 1, (size_t)*length, f) != (size_t)*length) { free (data); fclose (f); return NULL; }
	fclose (f); return data;
}
static void run_map (const char *name)
{
	long length, start; uint8_t *data = read_entry (name, &length, &start);
	assert (data && length >= 124 && u32 (data) == 29);
	size_t ents = u32 (data + 4), ent_size = u32 (data + 8);
	size_t textures = u32 (data + 20), tex_size = u32 (data + 24);
	assert (span (ents, ent_size, length) && span (textures, tex_size, length) && tex_size >= 4);
	uint32_t count = u32 (data + textures); assert (count < 10000 && span (4, (size_t)count * 4, tex_size));
	texture_t *brush = calloc (count, sizeof (*brush)); gltexture_t *base = calloc (count, sizeof (*base));
	gltexture_t *emission = calloc (count, sizeof (*emission)); assert (brush && base && emission);
	radiance_gpu_inputs_t gpu = {0}; radiance_coordinator_t coordinator;
	radiance_coordinator_limits_t limits = {4096, 8192, 512, 1000000, 64 * 1024 * 1024, 4096, 64};
	assert (RadianceCoordinator_Init (&coordinator, &gpu, &limits));
	assert (RadianceCoordinator_LoadMap (&coordinator, (const char *)data + ents, ent_size));
	bsp_start = start; bsp_size = length;
	size_t registered = 0, luminous = 0, luminous_lava = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		uint32_t relative = u32 (data + textures + 4 + i * 4);
		if (relative == UINT32_MAX) continue;
		assert (span (relative, 40, tex_size)); size_t mip = textures + relative;
		uint32_t width = u32 (data + mip + 16), height = u32 (data + mip + 20), pixel_offset = u32 (data + mip + 24);
		assert (width && height && (uint64_t)width * height <= SIZE_MAX && span (relative + pixel_offset, (size_t)width * height, tex_size));
		size_t pixels = mip + pixel_offset; bool sky = !memcmp (data + mip, "sky", 3), fullbright = false;
		bool liquid = data[mip] == '*', lava = liquid && !memcmp (data + mip + 1, "lava", 4);
		for (size_t k = 0; k < (size_t)width * height; ++k)
			if (data[pixels + k] >= 224 && data[pixels + k] < 255) { fullbright = true; break; }
		base[i].source_width = width; base[i].source_height = height; base[i].source_format = SRC_INDEXED;
		base[i].source_offset = pixels; memcpy (base[i].source_file, "fixture.bsp", sizeof ("fixture.bsp"));
		base[i].shirt = base[i].pants = -1;
		if (data[mip] == '{') base[i].flags |= TEXPREF_ALPHA;
		/* Warp textures have no separate fullbright image in Mod_LoadTextures. */
		if (fullbright && !sky && !liquid) { base[i].flags |= TEXPREF_NOBRIGHT; emission[i] = base[i]; emission[i].flags |= TEXPREF_FULLBRIGHT; brush[i].fullbright = &emission[i]; ++luminous; }
		brush[i].gltexture = &base[i]; brush[i].type = sky ? TEXTYPE_SKY : lava ? TEXTYPE_LAVA : liquid ? TEXTYPE_WATER : TEXTYPE_DEFAULT;
		assert (RadianceQuakeMaterials_Brush (&brush[i], &coordinator.materials) != UINT32_MAX); ++registered;
	}
	radiance_light_grid_t grid = {{-4096, -4096, -4096}, {1024, 1024, 1024}, {8, 8, 8}};
	int styles[256]; for (int i = 0; i < 256; ++i) styles[i] = 256;
	assert (RadianceCoordinator_Prepare (&coordinator, 0, styles, 256, NULL, 0, 0, &grid));
	/* Compare a source texel from every non-sky BSP texture against the packed
	 * material, including the black base of fullbright palette entries. */
	for (uint32_t i = 0; i < count; ++i)
	{
		if (!brush[i].gltexture || brush[i].type == TEXTYPE_SKY) continue;
		uint32_t id = RadianceQuakeMaterials_Brush (&brush[i], &coordinator.materials);
		const radiance_material_t *m = &coordinator.atlas.materials[id];
		size_t texel = (((size_t)m->layers_flags[0] * coordinator.atlas.height + m->albedo_rect[1]) *
			coordinator.atlas.width + m->albedo_rect[0]) * 4;
		uint8_t palette_index = data[base[i].source_offset];
		const uint8_t *expected = (const uint8_t *)&d_8to24table[palette_index];
		for (int channel = 0; channel < 3; ++channel)
			assert (coordinator.atlas.rgba[texel + channel] ==
				((brush[i].fullbright && palette_index >= 224) ? 0 : expected[channel]));
		if (brush[i].type == TEXTYPE_LAVA && palette_index >= 224)
		{
			assert (m->layers_flags[2] & RADIANCE_MATERIAL_EMISSION_TEXTURE);
			++luminous_lava;
		}
	}
	if (!strcmp (name, "maps/start.bsp")) assert (luminous_lava > 0);
	printf ("%s: %zu lights, %zu textures (%zu emissive, %zu luminous lava), atlas %ux%ux%u %.2f MiB\n", name,
		coordinator.map_count, registered, luminous, luminous_lava, coordinator.atlas.width, coordinator.atlas.height,
		coordinator.atlas.layers, coordinator.atlas.bytes / 1048576.0);
	RadianceCoordinator_Clear (&coordinator); free (emission); free (base); free (brush); free (data);
}
int main (int argc, char **argv)
{
	if (argc != 2) { fprintf (stderr, "usage: %s /path/to/pak0.pak\n", argv[0]); return 2; }
	pak_path = argv[1]; long length, start;
	uint8_t *palette = read_entry ("gfx/palette.lmp", &length, &start); assert (palette && length >= 768);
	for (int i = 0; i < 256; ++i) { uint8_t *rgba = (uint8_t *)&d_8to24table[i]; memcpy (rgba, palette + i * 3, 3); rgba[3] = 255; }
	free (palette); gl_fullbrights.value = 1;
	run_map ("maps/start.bsp"); run_map ("maps/e1m1.bsp"); run_map ("maps/e1m2.bsp");
	assert (upload_count == 3); puts ("PASS: read-only map source and packed atlas smoke test"); return 0;
}
