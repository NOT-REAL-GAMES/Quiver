/* Runtime light and emissive-source reconstruction for radiance cascades.
 *
 * This interface deliberately does not read BSP lightmaps. It extracts the
 * source metadata that survived map compilation and reports capacity and
 * malformed-input failures to the caller. The five 16-byte fields of
 * radiance_light_t match the std430 record in radiance_lighting.glsl.
 */
#ifndef QUIVER_RADIANCE_LIGHTS_H
#define QUIVER_RADIANCE_LIGHTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
	RADIANCE_LIGHT_POINT = 0,
	RADIANCE_LIGHT_SPOT = 1,
	RADIANCE_LIGHT_DIRECTIONAL = 2,
	RADIANCE_LIGHT_EMISSIVE = 3
} radiance_light_type_t;

enum
{
	RADIANCE_LIGHT_SOURCE_MAP = 1u,
	RADIANCE_LIGHT_SOURCE_DYNAMIC = 2u,
	RADIANCE_LIGHT_SOURCE_TEXTURE = 4u,
	/* Classic Quake dlight: linear falloff and radius-based brightness. */
	RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC = 8u,
	RADIANCE_LIGHT_SOURCE_KEX = 16u,
	/* Brush emissive proxy emits only from its visible side. */
	RADIANCE_LIGHT_SOURCE_ONE_SIDED = 32u,
	/* Even an explicit zero means the mapper requested a hard cone. */
	RADIANCE_LIGHT_SOURCE_EXPLICIT_SOFTANGLE = 64u,
	/* KEX dynamiclight spawnflag 1 excludes it from cooperative games. */
	RADIANCE_LIGHT_SOURCE_NOT_IN_COOP = 128u
};

typedef struct
{
	float position_radius[4];
	float color_intensity[4];
	float direction_cone[4];
	/* type, lightstyle (or legacy-dlight minlight float bits), source flags,
	 * attenuation descriptor. For map lights,
	 * metadata[3] has the Quake delay in bits 0..2 and the positive `wait`
	 * float bits in 3..31 (three insignificant mantissa bits are discarded).
	 * Non-map lights leave it zero and use the bounded dynamic-light falloff. */
	uint32_t metadata[4];
	/* Emissive area lights: tangent U and half-width; direction_cone.xyz is
	 * surface normal and direction_cone.w is half-height. Compiler-style map
	 * lights use x=angle sensitivity, y=inner cone cosine, z=1 marker. KEX
	 * map lights use x/y for camera-distance fade start/end. */
	float area_axis[4];
} radiance_light_t;

/* ericw _surface entities are templates, not lights at their own origins.
 * The coordinator instantiates them on matching brush surfaces. */
typedef struct
{
	radiance_light_t light;
	char texture[128];
	float offset;
	bool surface_spotlight;
} radiance_surface_template_t;

/* Change only compiler-style spotlights without an authored _softangle.
 * The outer cone and all distance attenuation remain unchanged. */
void Radiance_ApplyDefaultSpotSoftness (radiance_light_t *light, float softness);

enum
{
	RADIANCE_MATERIAL_ALBEDO_TEXTURE = 1u,
	RADIANCE_MATERIAL_EMISSION_TEXTURE = 2u
};

/* std430 material record and std140 per-dispatch light parameters. Texture
 * layer numbers address a host-built 2D array; see radiance_lighting.glsl. */
typedef struct
{
	float albedo_alpha[4];
	float emission[4];
	uint32_t layers_flags[4];
	/* Packed atlas x, y, width, height, in texels. */
	uint32_t albedo_rect[4], emission_rect[4];
} radiance_material_t;

typedef struct
{
	uint32_t counts[4]; /* lights, materials, layers, flags: grid/fullbright/gains */
	float sky_radiance[4];
	float ground_radiance[4];
	float grid_origin[4]; /* xyz grid origin, w point-light gain when enabled */
	float grid_inv_cell_size[4]; /* xyz inverse cell size, w emissive gain */
	uint32_t grid_dimensions[4]; /* x/y/z and index count */
	float bounce_parameters[4]; /* x indirect gain (flag bit 3); y single-sample area visibility */
	float gi_camera_fade[4]; /* xyz camera origin, w indirect fade end distance (0 disables) */
	float receiver_filter[4]; /* x: indirect probe filter strength (0..2) */
	float shadow_parameters[4]; /* receiver-only: enabled, finite radius, sun angular radius in radians, visibility filter strength */
} radiance_lighting_parameters_t;

typedef struct
{
	float origin[3];
	float cell_size[3];
	uint32_t dimensions[3];
} radiance_light_grid_t;

/* Plan a world-stable, at most 32^3 conservative grid. Small maps get
 * 128-unit cells; larger worlds use progressively coarser power-of-two
 * cells. Bounds receive one-cell padding. False leaves output untouched. */
bool Radiance_PlanLightGrid (const float mins[3], const float maxs[3], radiance_light_grid_t *output);
/* True only if the old conservative non-spot membership contains all cells
 * needed by the new light. Used to retain index lists while lights shrink or
 * move without crossing their previously indexed bounds. */
bool Radiance_LightGridBoundsCover (const radiance_light_grid_t *grid,
	const radiance_light_t *old_light, const radiance_light_t *new_light);

typedef struct
{
	size_t cell_count;
	size_t required_indices;
	size_t indexed_lights;
} radiance_light_grid_stats_t;

/* Build a conservative CSR grid: offsets has cell_count + 1 entries, indices
 * references the original light array. Directional lights enter every cell;
 * finite lights enter cells touched by their radius AABB. Spotlights are
 * conservatively pruned against their outer cone; local-minimum lights are
 * retained irrespective of direction. Out-of-grid
 * shader samples must evaluate the full array. Returns false without a usable
 * grid on invalid data, allocation failure, overflow or insufficient capacity;
 * stats.required_indices allows the caller to resize and retry. */
bool Radiance_BuildLightGrid (const radiance_light_grid_t *grid, const radiance_light_t *lights,
	size_t light_count, uint32_t *offsets, size_t offsets_capacity, uint32_t *indices,
	size_t indices_capacity, radiance_light_grid_stats_t *stats);

typedef struct
{
	size_t entities;
	size_t found;
	size_t written;
	size_t dropped;
	size_t malformed;
	size_t unresolved_targets;
	/* Explicit worldspawn _sunlight2 / _sunlight3 hemispheres, RGB and
	 * intensity/300. Zero unless specified; no ambient is invented indoors. */
	float sky_upper[4];
	float sky_lower[4];
	bool sky_authored; /* any explicit sunlight intensity, including zero */
} radiance_light_parse_stats_t;

/* Apply a low-energy sky texture fallback only when no sun/sky intensity
 * was authored and sky portal geometry exists. Color is linear RGB. */
bool Radiance_ApplySkyFallback (const radiance_light_parse_stats_t *stats, bool has_portal,
	const float color[3], float gain, float upper[4]);

/* Sky-field cache key includes whether misses return environment radiance. */
bool Radiance_SkyLightingChanged (const float previous[8], bool previous_portals_only,
	const radiance_lighting_parameters_t *current);

/* Reads a BSP entity lump, which may or may not end in NUL. Returns false for
 * malformed/truncated syntax, allocation failure, or invalid arguments; bad
 * light records are skipped and counted in stats.malformed. A full output
 * buffer is reported in stats.dropped. Output remains in entity order and
 * targetnames are resolved even when declared later. */
bool Radiance_ParseMapLights (const char *text, size_t text_size, radiance_light_t *output,
	size_t capacity, radiance_light_parse_stats_t *stats);
bool Radiance_ParseMapLightsWithSurfaceTemplates (const char *text, size_t text_size, radiance_light_t *output,
	size_t capacity, radiance_surface_template_t *templates, size_t template_capacity,
	size_t *template_count, radiance_light_parse_stats_t *stats);

/* Evaluates a source's Quake 8.8 lightstyle scale without mutating the stored
 * base intensity. Invalid or absent styles use the normal value (256). */
radiance_light_t Radiance_ApplyLightstyle (const radiance_light_t *source, const int *styles, size_t style_count);

/* Convert one active runtime light. A nonpositive radius, expired lifetime or
 * non-finite input returns false. direction may be NULL for an omni light. */
bool Radiance_MakeDynamicLight (radiance_light_t *output, const float position[3], float radius,
	const float color[3], float intensity, const float direction[3], float cone_cos, double die, double now);
/* Apply Quake's legacy dlight units/cutoff after successful construction.
 * Quake's world lightmap is scaled by two; compensate for the GI shader's
 * Lambert 1/PI factor while keeping its physically oriented response. */
void Radiance_ConfigureLegacyDynamicLight (radiance_light_t *light, float minlight);

typedef struct
{
	float radiance[3]; /* linear RGB averaged over the complete texture */
	float coverage;    /* fraction of opaque emitting texels */
	size_t emitting_texels;
} radiance_emissive_t;

/* Quake indexed textures: palette is 256 RGBA entries. Indices 224..254 emit;
 * 255 is excluded when transparent_255 is true. Other texels contribute zero. */
bool Radiance_AnalyzeIndexedEmission (const uint8_t *pixels, size_t count, const uint8_t palette_rgba[1024],
	bool transparent_255, radiance_emissive_t *output);

/* External fullbright masks: RGB is sRGB and alpha weights emission/coverage. */
bool Radiance_AnalyzeMaskEmission (const uint8_t *rgba, size_t count, radiance_emissive_t *output);

#endif
