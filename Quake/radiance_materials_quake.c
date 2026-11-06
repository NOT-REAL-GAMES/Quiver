#include "quakedef.h"
#include "radiance_materials_quake.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

extern cvar_t gl_fullbrights, gl_nocolors;
extern gltexture_t *playertextures[MAX_SCOREBOARD];

typedef struct
{
	radiance_quake_material_t source;
	uint64_t signature;
	radiance_material_t packed;
} radiance_quake_atlas_record_t;

struct radiance_quake_atlas_cache_s
{
	radiance_quake_atlas_record_t *records;
	size_t count, max_bytes;
	uint32_t max_dimension, max_layers;
	const uint8_t *pixels;
	uint32_t width, height, layers;
	size_t bytes;
};

static void RadianceQuakeMaterials_ClearAtlasCache (struct radiance_quake_atlas_cache_s *cache)
{
	if (cache) { free (cache->records); free (cache); }
}

void RadianceQuakeMaterials_Init (radiance_quake_materials_t *materials)
{
	if (materials) memset (materials, 0, sizeof (*materials));
}

void RadianceQuakeMaterials_Clear (radiance_quake_materials_t *materials)
{
	if (!materials) return;
	RadianceQuakeMaterials_ClearAtlasCache (materials->atlas_cache);
	free (materials->entries);
	memset (materials, 0, sizeof (*materials));
}

void RadianceQuakeMaterials_SetBrushFrame (radiance_quake_materials_t *materials, int frame)
{
	/* R_TextureAnimation treats frame as a boolean alternate-sequence flag. */
	if (materials) materials->brush_frame = frame != 0;
}

static uint32_t RadianceQuakeMaterials_Add (radiance_quake_materials_t *materials, radiance_quake_material_t value)
{
	if (!materials) return UINT32_MAX;
	for (size_t i = 0; i < materials->count; ++i)
	{
		const radiance_quake_material_t *old = &materials->entries[i];
		if (old->brush == value.brush && old->albedo == value.albedo && old->emission == value.emission &&
			old->brush_frame == value.brush_frame && old->sky == value.sky) return (uint32_t)i;
	}
	if (materials->count >= UINT32_MAX) return UINT32_MAX;
	if (materials->count == materials->capacity)
	{
		size_t capacity = materials->capacity ? materials->capacity * 2 : 64;
		if (capacity < materials->capacity || capacity > SIZE_MAX / sizeof (*materials->entries)) return UINT32_MAX;
		void *entries = realloc (materials->entries, capacity * sizeof (*materials->entries));
		if (!entries) return UINT32_MAX;
		materials->entries = entries; materials->capacity = capacity;
	}
	materials->entries[materials->count] = value;
	return (uint32_t)materials->count++;
}

uint32_t RadianceQuakeMaterials_Brush (const texture_t *texture, void *user)
{
	radiance_quake_materials_t *materials = user;
	if (!materials || !texture) return UINT32_MAX;
	radiance_quake_material_t value = {.brush = texture, .brush_frame = materials->brush_frame,
		.sky = texture->type == TEXTYPE_SKY};
	return RadianceQuakeMaterials_Add (materials, value);
}

uint32_t RadianceQuakeMaterials_Alias (const entity_t *entity, uint32_t surface,
	const gltexture_t *texture, const gltexture_t *fullbright, void *user)
{
	(void)surface;
	if (!entity || !texture || !user) return UINT32_MAX;
	/* Match the normal alias renderer's player-color substitution. */
	if (entity->colormap != vid.colormap && !gl_nocolors.value && cl.entities && cl.maxclients > 0)
	{
		uintptr_t address = (uintptr_t)entity, first = (uintptr_t)&cl.entities[1], last = (uintptr_t)&cl.entities[cl.maxclients];
		if (address >= first && address <= last && (address - first) % sizeof (*entity) == 0)
		{
			size_t player = (address - first) / sizeof (*entity);
			if (player < MAX_SCOREBOARD && playertextures[player]) texture = playertextures[player];
		}
	}
	radiance_quake_material_t value = {.albedo = texture, .emission = fullbright};
	return RadianceQuakeMaterials_Add (user, value);
}

static const texture_t *RadianceQuakeMaterials_Animated (const radiance_quake_material_t *entry)
{
	return entry->brush && !entry->sky ? R_TextureAnimation ((texture_t *)entry->brush, entry->brush_frame) : entry->brush;
}

static uint64_t RadianceQuakeMaterials_Hash (uint64_t hash, const void *data, size_t size)
{
	const uint8_t *bytes = data;
	while (size--) { hash ^= *bytes++; hash *= UINT64_C (1099511628211); }
	return hash;
}

static uint64_t RadianceQuakeMaterials_TextureHash (uint64_t hash, const gltexture_t *texture)
{
	hash = RadianceQuakeMaterials_Hash (hash, &texture, sizeof (texture));
	if (!texture) return hash;
	hash = RadianceQuakeMaterials_Hash (hash, &texture->image, sizeof (texture->image));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->source_crc, sizeof (texture->source_crc));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->source_width, sizeof (texture->source_width));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->source_height, sizeof (texture->source_height));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->source_format, sizeof (texture->source_format));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->source_offset, sizeof (texture->source_offset));
	hash = RadianceQuakeMaterials_Hash (hash, texture->source_file, strlen (texture->source_file));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->path_id, sizeof (texture->path_id));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->shirt, sizeof (texture->shirt));
	hash = RadianceQuakeMaterials_Hash (hash, &texture->pants, sizeof (texture->pants));
	return RadianceQuakeMaterials_Hash (hash, &texture->flags, sizeof (texture->flags));
}

uint64_t RadianceQuakeMaterials_Revision (const radiance_quake_materials_t *materials)
{
	if (!materials) return 0;
	uint64_t hash = UINT64_C (1469598103934665603);
	hash = RadianceQuakeMaterials_Hash (hash, &materials->count, sizeof (materials->count));
	bool fullbright_enabled = gl_fullbrights.value != 0;
	hash = RadianceQuakeMaterials_Hash (hash, &fullbright_enabled, sizeof (fullbright_enabled));
	for (size_t i = 0; i < materials->count; ++i)
	{
		const radiance_quake_material_t *entry = &materials->entries[i];
		const texture_t *animated = RadianceQuakeMaterials_Animated (entry);
		const gltexture_t *base = animated && !entry->sky ? animated->gltexture : entry->albedo;
		const gltexture_t *emission = animated && !entry->sky ? animated->fullbright : entry->emission;
		hash = RadianceQuakeMaterials_TextureHash (hash, base);
		hash = RadianceQuakeMaterials_TextureHash (hash, fullbright_enabled ? emission : NULL);
		if (fullbright_enabled && !emission && animated && TEXTYPE_ISLIQUID (animated->type) &&
			base && base->source_format != SRC_INDEXED && !animated->palette)
		{
			hash = RadianceQuakeMaterials_Hash (hash, &animated->width, sizeof (animated->width));
			hash = RadianceQuakeMaterials_Hash (hash, &animated->height, sizeof (animated->height));
			hash = RadianceQuakeMaterials_Hash (hash, &animated->source_offset, sizeof (animated->source_offset));
			hash = RadianceQuakeMaterials_Hash (hash, animated->source_file, strlen (animated->source_file));
		}
		hash = RadianceQuakeMaterials_Hash (hash, &entry->sky, sizeof (entry->sky));
	}
	return hash;
}

static uint8_t RadianceQuakeMaterials_Translate (const gltexture_t *texture, uint8_t index)
{
	if (texture->shirt < 0 || texture->pants < 0) return index;
	int color = -1, offset = -1;
	if (index >= TOP_RANGE && index < TOP_RANGE + 16) { color = texture->shirt * 16; offset = index - TOP_RANGE; }
	else if (index >= BOTTOM_RANGE && index < BOTTOM_RANGE + 16) { color = texture->pants * 16; offset = index - BOTTOM_RANGE; }
	if (color < 0 || color > 240) return index;
	return (uint8_t)(color + (color < 128 ? offset : 15 - offset));
}

static bool RadianceQuakeMaterials_ReadFile (int handle, uint8_t *data, size_t size)
{
	while (size)
	{
		int chunk = size > INT_MAX ? INT_MAX : (int)size;
		if (Sys_FileRead (handle, data, chunk) != chunk) return false;
		data += chunk;
		size -= (size_t)chunk;
	}
	return true;
}

/* Re-read the exact source, following TexMgr_ReloadImage's path/offset rules.
 * The caller owns the malloc-backed converted RGBA pixels. */
static bool RadianceQuakeMaterials_Load (const gltexture_t *texture, bool emissive,
	uint32_t max_dimension, radiance_material_image_t *result)
{
	if (!texture || !result || !texture->source_width || !texture->source_height ||
		texture->source_width > max_dimension || texture->source_height > max_dimension ||
		texture->source_width > SIZE_MAX / texture->source_height) return false;
	size_t count = (size_t)texture->source_width * texture->source_height;
	if (count > SIZE_MAX / 4 || count > INT_MAX) return false;
	uint8_t *data = NULL, *allocated = NULL;
	bool engine_allocated = false;
	enum srcformat format = texture->source_format;
	uint32_t width = texture->source_width, height = texture->source_height;
	size_t palette_offset = 0, palette_count = 0;
	if (texture->source_file[0] && texture->source_offset)
	{
		/* COM_OpenFile supports both disk-backed and memory-backed packs. The
		 * returned handle is positioned at the beginning of this pack member,
		 * so source_offset must be relative to that position, not the PAK. */
		int handle = -1;
		qfilesize_t file_size = COM_OpenFile (texture->source_file, &handle, NULL);
		if (handle < 0 || file_size < 0 || texture->source_offset > (src_offset_t)file_size)
		{ if (handle >= 0) Sys_FileClose (handle); return false; }
		qfileofs_t file_start = Sys_FilePos (handle);
		if (file_start < 0 || (uint64_t)texture->source_offset > (uint64_t)(LLONG_MAX - file_start))
		{ Sys_FileClose (handle); return false; }
		size_t bytes = format == SRC_RGBA ? count * 4 : count;
		if (format == SRC_INDEXED_PALETTE)
		{
			if (count % 64 || count / 64 > (SIZE_MAX - 2) / 85) { Sys_FileClose (handle); return false; }
			palette_offset = count / 64 * 85;
			bytes = palette_offset + 2;
		}
		else if (format != SRC_INDEXED && format != SRC_RGBA) { Sys_FileClose (handle); return false; }
		if (bytes > (uint64_t)file_size - texture->source_offset ||
			Sys_FileSeek (handle, file_start + (qfileofs_t)texture->source_offset))
		{ Sys_FileClose (handle); return false; }
		allocated = malloc (bytes);
		if (!allocated || !RadianceQuakeMaterials_ReadFile (handle, allocated, bytes))
		{ free (allocated); Sys_FileClose (handle); return false; }
		if (format == SRC_INDEXED_PALETTE)
		{
			palette_count = allocated[palette_offset] | ((size_t)allocated[palette_offset + 1] << 8);
			if (!palette_count || palette_count > 256 || palette_count > (SIZE_MAX - bytes) / 3 ||
				bytes + palette_count * 3 > (uint64_t)file_size - texture->source_offset)
			{ free (allocated); Sys_FileClose (handle); return false; }
			void *grown = realloc (allocated, bytes + palette_count * 3);
			if (!grown) { free (allocated); Sys_FileClose (handle); return false; }
			allocated = grown;
			if (!RadianceQuakeMaterials_ReadFile (handle, allocated + bytes, palette_count * 3))
			{ free (allocated); Sys_FileClose (handle); return false; }
		}
		Sys_FileClose (handle); data = allocated;
	}
	else if (texture->source_file[0])
	{
		int w = (int)width, h = (int)height;
		data = Image_LoadImage (texture->source_file, &w, &h, &format, texture->path_id);
		allocated = data; engine_allocated = true;
		if (!data || w <= 0 || h <= 0 || (uint32_t)w != width || (uint32_t)h != height ||
			(format != SRC_INDEXED && format != SRC_RGBA)) goto failed;
	}
	else if (texture->source_offset)
	{
		data = (uint8_t *)(uintptr_t)texture->source_offset;
		if (format == SRC_INDEXED_PALETTE)
		{
			if (count % 64 || count / 64 > (SIZE_MAX - 2) / 85) goto failed;
			palette_offset = count / 64 * 85;
			palette_count = data[palette_offset] | ((size_t)data[palette_offset + 1] << 8);
			if (!palette_count || palette_count > 256) goto failed;
		}
		else if (format != SRC_INDEXED && format != SRC_RGBA) goto failed;
	}
	if (!data) goto failed;
	uint8_t *rgba = malloc (count * 4);
	if (!rgba) goto failed;
	if (format == SRC_RGBA)
		memcpy (rgba, data, count * 4);
	else for (size_t i = 0; i < count; ++i)
	{
		uint8_t index = RadianceQuakeMaterials_Translate (texture, data[i]);
		uint8_t *pixel = rgba + i * 4;
		if (format == SRC_INDEXED_PALETTE)
		{
			if (index >= palette_count) { free (rgba); goto failed; }
			memcpy (pixel, data + palette_offset + 2 + (size_t)index * 3, 3);
		}
		else memcpy (pixel, (const uint8_t *)&d_8to24table[index], 3);
		if ((emissive && index < 224) || (!emissive && (texture->flags & TEXPREF_NOBRIGHT) && gl_fullbrights.value && index >= 224))
			pixel[0] = pixel[1] = pixel[2] = 0;
		pixel[3] = (texture->flags & TEXPREF_ALPHA) && index == 255 ? 0 : 255;
	}
	if (engine_allocated) Mem_Free (allocated); else free (allocated);
	*result = (radiance_material_image_t){rgba, width, height};
	return true;
failed:
	if (engine_allocated) Mem_Free (allocated); else free (allocated);
	return false;
}

static bool RadianceQuakeMaterials_Charge (const gltexture_t *texture, uint32_t max_dimension,
	size_t max_bytes, size_t *bytes)
{
	if (!texture || !texture->source_width || !texture->source_height || texture->source_width > max_dimension ||
		texture->source_height > max_dimension || texture->source_width > SIZE_MAX / texture->source_height) return false;
	size_t pixels = (size_t)texture->source_width * texture->source_height;
	if (pixels > SIZE_MAX / 4 || pixels * 4 > max_bytes || *bytes > max_bytes - pixels * 4) return false;
	*bytes += pixels * 4;
	return true;
}

static bool RadianceQuakeMaterials_LoadSource (const radiance_quake_material_t *entry,
	const texture_t *animated, uint32_t max_dimension, size_t max_bytes,
	size_t *source_bytes, radiance_material_source_t *source)
{
	const gltexture_t *base = animated && !entry->sky ? animated->gltexture : entry->albedo;
	const gltexture_t *emission = animated && !entry->sky ? animated->fullbright : entry->emission;
	/* An external replacement can hide the original indexed liquid texture
	 * from gltexture. Its BSP source still determines the fullbright mask. */
	gltexture_t original_liquid = {0};
	bool original_mask = false;
	/* Warp textures do not get the separate fullbright image that ordinary
	 * brush textures receive. Quake indexed liquids still contain the same
	 * 224..255 fullbright palette indices (e.g. *lava1). Recover only those
	 * pixels; an ordinary water texture must not become a light. Valve's
	 * per-texture palettes do not have that general convention. */
	bool liquid_mask = !emission && animated && TEXTYPE_ISLIQUID (animated->type) &&
		base && base->source_format == SRC_INDEXED;
	if (liquid_mask) emission = base;
	else if (!emission && animated && TEXTYPE_ISLIQUID (animated->type) && base &&
		!animated->palette && animated->source_offset && animated->width && animated->height)
	{
		original_liquid.source_width = animated->width;
		original_liquid.source_height = animated->height;
		original_liquid.source_format = SRC_INDEXED;
		original_liquid.source_offset = animated->source_offset;
		memcpy (original_liquid.source_file, animated->source_file, sizeof (original_liquid.source_file));
		original_liquid.shirt = original_liquid.pants = -1;
		emission = &original_liquid;
		liquid_mask = original_mask = true;
	}
	(*source).sky = entry->sky;
	if (!entry->sky && (!RadianceQuakeMaterials_Charge (base, max_dimension, max_bytes, source_bytes) ||
		!RadianceQuakeMaterials_Load (base, false, max_dimension, &(*source).albedo))) return false;
	if (gl_fullbrights.value && emission)
	{
		size_t before = *source_bytes;
		if (!RadianceQuakeMaterials_Charge (emission, max_dimension, max_bytes, source_bytes) ||
			!RadianceQuakeMaterials_Load (emission, true, max_dimension, &(*source).emission))
		{
			/* An optional original source can be absent for external WADs or
			 * exceed the atlas budget; keep the replacement material usable. */
			if (!original_mask) return false;
			*source_bytes = before;
			return true;
		}
		if (liquid_mask)
		{
			const uint8_t *rgba = (*source).emission.rgba;
			size_t pixels = (size_t)(*source).emission.width * (*source).emission.height;
			bool emits = false;
			for (size_t p = 0; p < pixels && !emits; ++p)
				emits = rgba[p * 4 + 3] && (rgba[p * 4] || rgba[p * 4 + 1] || rgba[p * 4 + 2]);
			if (!emits)
			{
				free ((void *)(*source).emission.rgba);
				(*source).emission = (radiance_material_image_t){0};
				*source_bytes = before;
			}
		}
	}
	return true;
}

bool RadianceQuakeMaterials_BuildAtlas (const radiance_quake_materials_t *materials,
	radiance_material_atlas_t *atlas, uint32_t max_dimension, uint32_t max_layers, size_t max_bytes)
{
	if (!materials || !atlas || !materials->count || materials->count > SIZE_MAX / sizeof (radiance_material_source_t)) return false;
	radiance_material_source_t *sources = calloc (materials->count, sizeof (*sources));
	if (!sources) return false;
	bool success = false;
	size_t source_bytes = 0;
	for (size_t i = 0; i < materials->count; ++i)
		if (!RadianceQuakeMaterials_LoadSource (&materials->entries[i], RadianceQuakeMaterials_Animated (&materials->entries[i]),
			max_dimension, max_bytes, &source_bytes, &sources[i])) goto done;
	success = RadianceMaterialAtlas_Build (atlas, sources, materials->count, max_dimension, max_layers, max_bytes);
done:
	for (size_t i = 0; i < materials->count; ++i)
	{
		free ((void *)sources[i].albedo.rgba);
		free ((void *)sources[i].emission.rgba);
	}
	free (sources);
	return success;
}

/* A source is an explicit frame, never a clock-dependent animation root.
 * Cached records keep pointer values only for identity comparisons; old source
 * pointers are not dereferenced after registry compaction or model removal. */
static radiance_quake_atlas_record_t RadianceQuakeMaterials_Record (
	const radiance_quake_material_t *entry, const texture_t *frame)
{
	radiance_quake_atlas_record_t record = {0};
	record.source = *entry; record.source.brush = frame; record.source.brush_frame = 0;
	if (frame && !entry->sky)
	{
		record.source.albedo = frame->gltexture;
		record.source.emission = frame->fullbright;
	}
	uint64_t hash = UINT64_C (1469598103934665603);
	bool fullbright = gl_fullbrights.value != 0;
	hash = RadianceQuakeMaterials_Hash (hash, &fullbright, sizeof (fullbright));
	hash = RadianceQuakeMaterials_Hash (hash, &entry->sky, sizeof (entry->sky));
	hash = RadianceQuakeMaterials_TextureHash (hash, record.source.albedo);
	hash = RadianceQuakeMaterials_TextureHash (hash, fullbright ? record.source.emission : NULL);
	if (frame && !entry->sky)
	{
		hash = RadianceQuakeMaterials_Hash (hash, &frame->type, sizeof (frame->type));
		if (TEXTYPE_ISLIQUID (frame->type))
		{
			hash = RadianceQuakeMaterials_Hash (hash, &frame->width, sizeof (frame->width));
			hash = RadianceQuakeMaterials_Hash (hash, &frame->height, sizeof (frame->height));
			hash = RadianceQuakeMaterials_Hash (hash, &frame->palette, sizeof (frame->palette));
			hash = RadianceQuakeMaterials_Hash (hash, &frame->source_offset, sizeof (frame->source_offset));
			hash = RadianceQuakeMaterials_Hash (hash, frame->source_file, strlen (frame->source_file));
		}
	}
	record.signature = hash;
	return record;
}

static bool RadianceQuakeMaterials_SameRecord (const radiance_quake_atlas_record_t *a,
	const radiance_quake_atlas_record_t *b)
{
	return a->source.brush == b->source.brush && a->source.albedo == b->source.albedo &&
		a->source.emission == b->source.emission && a->source.sky == b->source.sky && a->signature == b->signature;
}

static size_t RadianceQuakeMaterials_FindRecord (const radiance_quake_atlas_record_t *records,
	size_t count, const radiance_quake_atlas_record_t *wanted)
{
	for (size_t i = 0; i < count; ++i) if (RadianceQuakeMaterials_SameRecord (&records[i], wanted)) return i;
	return SIZE_MAX;
}

static bool RadianceQuakeMaterials_AddRecord (radiance_quake_atlas_record_t **records,
	size_t *count, size_t *capacity, const radiance_quake_material_t *entry, const texture_t *frame)
{
	radiance_quake_atlas_record_t record = RadianceQuakeMaterials_Record (entry, frame);
	if (RadianceQuakeMaterials_FindRecord (*records, *count, &record) != SIZE_MAX) return true;
	if (*count == *capacity)
	{
		size_t grown = *capacity ? *capacity * 2 : 64;
		if (grown < *capacity || grown > SIZE_MAX / sizeof (**records)) return false;
		void *memory = realloc (*records, grown * sizeof (**records));
		if (!memory) return false;
		*records = memory; *capacity = grown;
	}
	(*records)[(*count)++] = record;
	return true;
}

static bool RadianceQuakeMaterials_AddSequence (radiance_quake_atlas_record_t **records,
	size_t *count, size_t *capacity, const radiance_quake_material_t *entry, const texture_t *first)
{
	const texture_t *frame = first;
	/* Match the renderer's bounded animation-chain traversal. A broken chain
 * cannot leave a partial new atlas published. */
	for (unsigned i = 0; frame && i < 100; ++i)
	{
		if (!RadianceQuakeMaterials_AddRecord (records, count, capacity, entry, frame)) return false;
		frame = frame->anim_next;
		if (frame == first) return true;
	}
	return frame == NULL;
}

static radiance_material_t *RadianceQuakeMaterials_SelectRecords (const radiance_quake_materials_t *materials,
	const radiance_quake_atlas_record_t *records, size_t count)
{
	if (materials->count > SIZE_MAX / sizeof (radiance_material_t)) return NULL;
	radiance_material_t *selected = malloc (materials->count * sizeof (*selected));
	if (!selected) return NULL;
	for (size_t i = 0; i < materials->count; ++i)
	{
		radiance_quake_atlas_record_t wanted = RadianceQuakeMaterials_Record (&materials->entries[i],
			RadianceQuakeMaterials_Animated (&materials->entries[i]));
		size_t at = RadianceQuakeMaterials_FindRecord (records, count, &wanted);
		if (at == SIZE_MAX) { free (selected); return NULL; }
		selected[i] = records[at].packed;
	}
	return selected;
}

/* Inventory growth must not reopen every immutable image in a large model.
 * A matching record owns exact decoded texels in the old atlas; recover those
 * before the transactional pack replaces it. Never dereference retired source
 * handles kept only as cache identities. */
static bool RadianceQuakeMaterials_UnpackRecord (const radiance_material_atlas_t *atlas,
	const radiance_quake_atlas_record_t *record, uint32_t max_dimension,
	size_t max_bytes, size_t *source_bytes, radiance_material_source_t *source, uint8_t *borrowed)
{
	source->sky = record->source.sky;
	for (unsigned kind = 0; kind < 2; ++kind)
	{
		const radiance_material_t *material = &record->packed;
		uint32_t flag = kind ? RADIANCE_MATERIAL_EMISSION_TEXTURE : RADIANCE_MATERIAL_ALBEDO_TEXTURE;
		if (!(material->layers_flags[2] & flag))
		{
			if (!kind && !source->sky) return false;
			continue;
		}
		const uint32_t *rect = kind ? material->emission_rect : material->albedo_rect;
		uint32_t layer = material->layers_flags[kind], width = rect[2], height = rect[3];
		if (!atlas->rgba || !width || !height || width > max_dimension || height > max_dimension ||
			layer >= atlas->layers || rect[0] > atlas->width || width > atlas->width - rect[0] ||
			rect[1] > atlas->height || height > atlas->height - rect[1] ||
			(size_t)width > SIZE_MAX / height || (size_t)width * height > SIZE_MAX / 4) return false;
		size_t bytes = (size_t)width * height * 4;
		if (bytes > max_bytes || *source_bytes > max_bytes - bytes) return false;
		uint64_t layer_pixels = (uint64_t)atlas->width * atlas->height;
		if (layer_pixels > UINT64_MAX / (layer + UINT64_C (1)) / 4 ||
			layer_pixels * (layer + UINT64_C (1)) * 4 > atlas->bytes) return false;
		const uint8_t *packed = atlas->rgba + ((size_t)layer * atlas->height + rect[1]) * atlas->width * 4 + (size_t)rect[0] * 4;
		const uint8_t *pixels = packed;
		if (width == atlas->width)
			*borrowed |= (uint8_t)(1u << kind); /* Full-width rows are already tightly packed. */
		else
		{
			uint8_t *copy = malloc (bytes);
			if (!copy) return false;
			for (uint32_t row = 0; row < height; ++row)
				memcpy (copy + (size_t)row * width * 4, packed + (size_t)row * atlas->width * 4, (size_t)width * 4);
			pixels = copy;
		}
		*(kind ? &source->emission : &source->albedo) = (radiance_material_image_t){pixels, width, height};
		*source_bytes += bytes;
	}
	return true;
}

bool RadianceQuakeMaterials_UpdateAtlas (radiance_quake_materials_t *materials,
	radiance_material_atlas_t *atlas, uint32_t max_dimension, uint32_t max_layers,
	size_t max_bytes, bool *pixels_changed)
{
	if (pixels_changed) *pixels_changed = false;
	if (!materials || !atlas || !materials->count || !materials->entries || !max_dimension || !max_layers || !max_bytes) return false;
	radiance_quake_atlas_record_t *inventory = NULL;
	size_t count = 0, capacity = 0;
	radiance_material_source_t *sources = NULL;
	uint8_t *borrowed = NULL;
	radiance_material_atlas_t pending = {0};
	struct radiance_quake_atlas_cache_s *replacement = NULL;
	radiance_material_t *selected = NULL;
	bool success = false;
	for (size_t i = 0; i < materials->count; ++i)
	{
		const radiance_quake_material_t *entry = &materials->entries[i];
		if (entry->brush && !entry->sky)
		{
			if (!RadianceQuakeMaterials_AddSequence (&inventory, &count, &capacity, entry, entry->brush) ||
				!RadianceQuakeMaterials_AddSequence (&inventory, &count, &capacity, entry, entry->brush->alternate_anims)) goto done;
		}
		else if (!RadianceQuakeMaterials_AddRecord (&inventory, &count, &capacity, entry, entry->brush)) goto done;
	}
	struct radiance_quake_atlas_cache_s *cache = materials->atlas_cache;
	bool cached_pixels = cache && cache->pixels == atlas->rgba && cache->width == atlas->width &&
		cache->height == atlas->height && cache->layers == atlas->layers && cache->bytes == atlas->bytes;
	bool reusable = cached_pixels &&
		cache->max_dimension == max_dimension && cache->max_layers == max_layers && cache->max_bytes == max_bytes;
	for (size_t i = 0; reusable && i < count; ++i)
		if (RadianceQuakeMaterials_FindRecord (cache->records, cache->count, &inventory[i]) == SIZE_MAX) reusable = false;
	if (reusable)
	{
		selected = RadianceQuakeMaterials_SelectRecords (materials, cache->records, cache->count);
		if (!selected) goto done;
		/* A compacted registry changes IDs and material count, but no pixels. */
		free (atlas->materials); atlas->materials = selected; selected = NULL;
		atlas->material_count = materials->count;
		success = true; goto done;
	}
	if (count > SIZE_MAX / sizeof (*sources)) goto done;
	sources = calloc (count, sizeof (*sources)); borrowed = calloc (count, sizeof (*borrowed));
	replacement = calloc (1, sizeof (*replacement));
	if (!sources || !borrowed || !replacement) goto done;
	size_t source_bytes = 0;
	for (size_t i = 0; i < count; ++i)
	{
		size_t old = cached_pixels ? RadianceQuakeMaterials_FindRecord (cache->records, cache->count, &inventory[i]) : SIZE_MAX;
		if (old != SIZE_MAX)
		{
			if (!RadianceQuakeMaterials_UnpackRecord (atlas, &cache->records[old], max_dimension,
				max_bytes, &source_bytes, &sources[i], &borrowed[i])) goto done;
		}
		else if (!RadianceQuakeMaterials_LoadSource (&inventory[i].source, inventory[i].source.brush,
			max_dimension, max_bytes, &source_bytes, &sources[i])) goto done;
	}
	if (!RadianceMaterialAtlas_Build (&pending, sources, count, max_dimension, max_layers, max_bytes)) goto done;
	for (size_t i = 0; i < count; ++i) inventory[i].packed = pending.materials[i];
	selected = RadianceQuakeMaterials_SelectRecords (materials, inventory, count);
	if (!selected) goto done;
	free (pending.materials); pending.materials = selected; selected = NULL; pending.material_count = materials->count;
	replacement->records = inventory; inventory = NULL; replacement->count = count;
	replacement->max_dimension = max_dimension; replacement->max_layers = max_layers; replacement->max_bytes = max_bytes;
	replacement->pixels = pending.rgba; replacement->width = pending.width; replacement->height = pending.height;
	replacement->layers = pending.layers; replacement->bytes = pending.bytes;
	RadianceMaterialAtlas_Clear (atlas); *atlas = pending; memset (&pending, 0, sizeof (pending));
	RadianceQuakeMaterials_ClearAtlasCache (materials->atlas_cache); materials->atlas_cache = replacement; replacement = NULL;
	if (pixels_changed) *pixels_changed = true;
	success = true;
done:
	if (sources) for (size_t i = 0; i < count; ++i)
	{
		if (!borrowed || !(borrowed[i] & 1u)) free ((void *)sources[i].albedo.rgba);
		if (!borrowed || !(borrowed[i] & 2u)) free ((void *)sources[i].emission.rgba);
	}
	free (sources); free (borrowed); free (inventory); free (selected);
	RadianceQuakeMaterials_ClearAtlasCache (replacement); RadianceMaterialAtlas_Clear (&pending);
	return success;
}
