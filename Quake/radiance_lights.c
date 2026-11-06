/* Runtime light-source reconstruction for radiance cascades. */
#include "radiance_lights.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool Radiance_PlanLightGrid (const float mins[3], const float maxs[3], radiance_light_grid_t *output)
{
	if (!mins || !maxs || !output) return false;
	double span = 0.0, magnitude = 0.0;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (!isfinite (mins[axis]) || !isfinite (maxs[axis]) || mins[axis] > maxs[axis]) return false;
		span = fmax (span, (double)maxs[axis] - (double)mins[axis]);
		magnitude = fmax (magnitude, fmax (fabs ((double)mins[axis]), fabs ((double)maxs[axis])));
	}
	/* Ensure float shader coordinates retain adequate precision, as well as
	 * leaving room for padding and alignment at both sides of each axis. */
	double size = 128.0;
	while (size < span / 29.0 || size < magnitude * FLT_EPSILON * 4.0)
	{
		size *= 2.0;
		if (size > 0x1p60) return false;
	}
	radiance_light_grid_t grid = {0};
	for (int axis = 0; axis < 3; ++axis)
	{
		double origin = (floor ((double)mins[axis] / size) - 1.0) * size;
		double cells = ceil (((double)maxs[axis] - origin) / size) + 1.0;
		if (!isfinite (origin) || fabs (origin) > FLT_MAX || cells < 1.0 || cells > 32.0) return false;
		grid.origin[axis] = (float)origin;
		grid.cell_size[axis] = (float)size;
		grid.dimensions[axis] = (uint32_t)cells;
		/* Check the actual float values that will be uploaded to GLSL. */
		if ((double)grid.origin[axis] > (double)mins[axis] ||
			(double)grid.origin[axis] + size * grid.dimensions[axis] <= (double)maxs[axis]) return false;
	}
	*output = grid;
	return true;
}

typedef struct { const char *text; size_t size, offset; } radiance_parser_t;
typedef struct { char text[4096]; char punctuation; } radiance_token_t;
typedef struct { char name[128]; float origin[3]; size_t order; } radiance_target_t;
typedef struct { char name[128], surface[128]; float surface_offset; bool explicit_falloff, surface_spotlight; } radiance_reference_t;

typedef struct
{
	char classname[128], target[128], targetname[128], surface[128];
	float origin[3], color[3], sun_color[3], sun_angles[3], mangle[3], embedded_color[3], embedded_sun_color[3];
	float upper_color[3], lower_color[3];
	float light, wait, angle, cone, softangle, anglescale, sun, sky_upper, sky_lower, radius, kex_intensity, falloff, world_dist, world_range, surface_offset;
	float kex_fade_start, kex_fade_end;
	int style, delay, surface_spotlight, spawnflags;
	bool has_sky_intensity, has_origin, has_angle, has_cone, has_sun_angles, has_mangle, has_light, has_radius, has_kex_intensity, shadowlight, invalid;
	bool has_color, has_embedded_color, has_sun_color, has_embedded_sun_color, has_anglescale, has_world_dist, has_world_range, has_softangle;
} radiance_entity_t;

/* Returns 1 for a token, 0 for EOF and -1 for malformed input. */
static int Radiance_NextToken (radiance_parser_t *parser, radiance_token_t *token)
{
	const char *s = parser->text;
	size_t p = parser->offset, n = parser->size, length = 0;
	memset (token, 0, sizeof (*token));
	for (;;)
	{
		while (p < n && s[p] && (unsigned char)s[p] <= ' ') ++p;
		if (p >= n || !s[p]) { parser->offset = p; return 0; }
		if (s[p] == '/' && p + 1 < n && s[p + 1] == '/')
		{
			p += 2;
			while (p < n && s[p] && s[p] != '\n') ++p;
			continue;
		}
		if (s[p] == '/' && p + 1 < n && s[p + 1] == '*')
		{
			p += 2;
			while (p + 1 < n && s[p] && !(s[p] == '*' && s[p + 1] == '/')) ++p;
			if (p + 1 >= n || !s[p]) return -1;
			p += 2;
			continue;
		}
		break;
	}
	if (s[p] == '{' || s[p] == '}')
	{
		token->punctuation = s[p]; token->text[0] = s[p]; token->text[1] = 0;
		parser->offset = p + 1; return 1;
	}
	if (s[p] == '"')
	{
		++p;
		while (p < n && s[p] && s[p] != '"')
		{
			if (length + 1 >= sizeof (token->text)) return -1;
			token->text[length++] = s[p++];
		}
		if (p >= n || s[p] != '"') return -1;
		++p;
	}
	else
	{
		while (p < n && s[p] && (unsigned char)s[p] > ' ' && s[p] != '{' && s[p] != '}')
		{
			if (length + 1 >= sizeof (token->text)) return -1;
			token->text[length++] = s[p++];
		}
	}
	token->text[length] = 0;
	parser->offset = p;
	return 1;
}

static bool Radiance_CopyString (char *dst, size_t size, const char *src)
{
	size_t length = strlen (src);
	if (length >= size) return false;
	memcpy (dst, src, length + 1);
	return true;
}

static bool Radiance_ParseFloats (const char *s, float *values, int count)
{
	for (int i = 0; i < count; ++i)
	{
		char *end;
		while (isspace ((unsigned char)*s)) ++s;
		if (!*s) return false;
		values[i] = strtof (s, &end);
		if (end == s || !isfinite (values[i])) return false;
		s = end;
	}
	while (isspace ((unsigned char)*s)) ++s;
	return *s == 0;
}

static bool Radiance_ParseInt (const char *s, int *out)
{
	float value;
	if (!Radiance_ParseFloats (s, &value, 1) || value < -2147483520.0f || value > 2147483520.0f) return false;
	*out = (int)value;
	return true;
}

static bool Radiance_Normalize (float v[3])
{
	float length = sqrtf (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (!isfinite (length) || length <= 1e-8f) return false;
	for (int i = 0; i < 3; ++i) v[i] /= length;
	return true;
}

static void Radiance_NormalizeColor (float color[3])
{
	float scale = (color[0] > 1 || color[1] > 1 || color[2] > 1) ? 1.0f / 255.0f : 1.0f;
	for (int i = 0; i < 3; ++i) color[i] = fmaxf (0.0f, color[i] * scale);
}

static bool Radiance_AppendTarget (radiance_target_t **targets, size_t *count, size_t *capacity, const radiance_entity_t *entity)
{
	if (!entity->targetname[0] || !entity->has_origin) return true;
	if (*count == *capacity)
	{
		size_t next = *capacity ? *capacity * 2 : 64;
		if (next < *capacity || next > SIZE_MAX / sizeof (**targets)) return false;
		void *grown = realloc (*targets, next * sizeof (**targets));
		if (!grown) return false;
		*targets = grown; *capacity = next;
	}
	Radiance_CopyString ((*targets)[*count].name, sizeof ((*targets)[*count].name), entity->targetname);
	memcpy ((*targets)[*count].origin, entity->origin, sizeof (entity->origin));
	(*targets)[*count].order = *count;
	++*count;
	return true;
}

static int Radiance_CompareTargets (const void *a, const void *b)
{
	const radiance_target_t *left = a, *right = b;
	int order = strcmp (left->name, right->name);
	if (order) return order;
	return (left->order > right->order) - (left->order < right->order);
}

static const radiance_target_t *Radiance_FindTarget (const radiance_target_t *targets, size_t count, const char *name)
{
	size_t lo = 0, hi = count;
	while (lo < hi)
	{
		size_t mid = lo + (hi - lo) / 2;
		if (strcmp (targets[mid].name, name) < 0) lo = mid + 1;
		else hi = mid;
	}
	return lo < count && !strcmp (targets[lo].name, name) ? &targets[lo] : NULL;
}

static void Radiance_ParseField (radiance_entity_t *e, const char *key, const char *value)
{
	if (!strcmp (key, "classname")) e->invalid |= !Radiance_CopyString (e->classname, sizeof (e->classname), value);
	else if (!strcmp (key, "target")) e->invalid |= !Radiance_CopyString (e->target, sizeof (e->target), value);
	else if (!strcmp (key, "targetname")) e->invalid |= !Radiance_CopyString (e->targetname, sizeof (e->targetname), value);
	else if (!strcmp (key, "_surface")) e->invalid |= !Radiance_CopyString (e->surface, sizeof (e->surface), value);
	else if (!strcmp (key, "_surface_offset")) e->invalid |= !Radiance_ParseFloats (value, &e->surface_offset, 1);
	else if (!strcmp (key, "_surface_spotlight")) e->invalid |= !Radiance_ParseInt (value, &e->surface_spotlight);
	else if (!strcmp (key, "spawnflags")) (void) Radiance_ParseInt (value, &e->spawnflags);
	else if (!strcmp (key, "origin")) { e->has_origin = Radiance_ParseFloats (value, e->origin, 3); e->invalid |= !e->has_origin; }
	else if (!strcmp (key, "_color") || !strcmp (key, "color")) { e->has_color = Radiance_ParseFloats (value, e->color, 3); e->invalid |= !e->has_color; }
	else if (!strcmp (key, "light") || !strcmp (key, "_light"))
	{
		float rgba[4];
		if (Radiance_ParseFloats (value, rgba, 4))
		{
			memcpy (e->embedded_color, rgba, sizeof (e->embedded_color)); e->has_embedded_color = true; e->light = rgba[3]; e->has_light = true;
		}
		else e->has_light = Radiance_ParseFloats (value, &e->light, 1);
		e->invalid |= !e->has_light;
	}
	else if (!strcmp (key, "wait") || !strcmp (key, "_wait")) e->invalid |= !Radiance_ParseFloats (value, &e->wait, 1);
	else if (!strcmp (key, "delay") || !strcmp (key, "_delay")) e->invalid |= !Radiance_ParseInt (value, &e->delay);
	else if (!strcmp (key, "_falloff")) e->invalid |= !Radiance_ParseFloats (value, &e->falloff, 1);
	else if (!strcmp (key, "_dist")) e->has_world_dist = Radiance_ParseFloats (value, &e->world_dist, 1);
	else if (!strcmp (key, "_range")) e->has_world_range = Radiance_ParseFloats (value, &e->world_range, 1);
	else if (!strcmp (key, "_anglescale") || !strcmp (key, "_anglesense") || !strcmp (key, "anglescale") || !strcmp (key, "anglesense"))
	{ e->has_anglescale = Radiance_ParseFloats (value, &e->anglescale, 1); e->invalid |= !e->has_anglescale; }
	else if (!strcmp (key, "_softangle") || !strcmp (key, "softangle"))
	{ e->has_softangle = Radiance_ParseFloats (value, &e->softangle, 1); e->invalid |= !e->has_softangle; }
	else if (!strcmp (key, "style") || !strcmp (key, "_shadowlightstyle")) e->invalid |= !Radiance_ParseInt (value, &e->style);
	else if (!strcmp (key, "angle")) { e->has_angle = Radiance_ParseFloats (value, &e->angle, 1); e->invalid |= !e->has_angle; }
	else if (!strcmp (key, "_cone") || !strcmp (key, "_shadowlightconeangle")) { e->has_cone = Radiance_ParseFloats (value, &e->cone, 1); e->invalid |= !e->has_cone; }
	else if (!strcmp (key, "_shadowlight")) { int v = 0; e->invalid |= !Radiance_ParseInt (value, &v); e->shadowlight = v != 0; }
	else if (!strcmp (key, "_shadowlightradius")) { e->has_radius = Radiance_ParseFloats (value, &e->radius, 1); e->invalid |= !e->has_radius; }
	else if (!strcmp (key, "_shadowlightintensity")) { e->has_kex_intensity = Radiance_ParseFloats (value, &e->kex_intensity, 1); e->invalid |= !e->has_kex_intensity; }
	else if (!strcmp (key, "_shadowlightstartfadedistance")) e->invalid |= !Radiance_ParseFloats (value, &e->kex_fade_start, 1);
	else if (!strcmp (key, "_shadowlightendfadedistance")) e->invalid |= !Radiance_ParseFloats (value, &e->kex_fade_end, 1);
	else if (!strcmp (key, "_sunlight"))
	{
		e->has_sky_intensity = true;
		float rgb[3];
		if (Radiance_ParseFloats (value, rgb, 3))
		{
			/* Some compiled maps use an RGB-valued sunlight instead of the
			 * scalar compiler key. Preserve that runtime source explicitly. */
			memcpy (e->embedded_sun_color, rgb, sizeof (rgb));
			e->has_embedded_sun_color = true;
			e->sun = (rgb[0] > 0 || rgb[1] > 0 || rgb[2] > 0) ? 300.0f : 0.0f;
		}
		else e->invalid |= !Radiance_ParseFloats (value, &e->sun, 1);
	}
	else if (!strcmp (key, "_sunlight_color")) { e->has_sun_color = Radiance_ParseFloats (value, e->sun_color, 3); e->invalid |= !e->has_sun_color; }
	else if (!strcmp (key, "_sunlight2")) { e->has_sky_intensity = true; e->invalid |= !Radiance_ParseFloats (value, &e->sky_upper, 1); }
	else if (!strcmp (key, "_sunlight3")) { e->has_sky_intensity = true; e->invalid |= !Radiance_ParseFloats (value, &e->sky_lower, 1); }
	else if (!strcmp (key, "_sunlight2_color")) e->invalid |= !Radiance_ParseFloats (value, e->upper_color, 3);
	else if (!strcmp (key, "_sunlight3_color")) e->invalid |= !Radiance_ParseFloats (value, e->lower_color, 3);
	else if (!strcmp (key, "_sun_mangle") || !strcmp (key, "_sunlight_mangle")) { e->has_sun_angles = Radiance_ParseFloats (value, e->sun_angles, 3); e->invalid |= !e->has_sun_angles; }
	else if (!strcmp (key, "mangle") || !strcmp (key, "_mangle")) { e->has_mangle = Radiance_ParseFloats (value, e->mangle, 3); e->invalid |= !e->has_mangle; }
}

static uint32_t Radiance_AttenuationDescriptor (int delay, float wait)
{
	if (!isfinite (wait) || wait <= 0) wait = 1;
	if (delay < 0 || delay > 5) delay = 0;
	uint32_t bits;
	memcpy (&bits, &wait, sizeof (bits));
	return (bits & ~UINT32_C(7)) | (uint32_t)delay;
}

static void Radiance_AngleDirection (float angle, float direction[3])
{
	const float radians = angle * 0.01745329251994329577f;
	if (angle == -1) { direction[0] = 0; direction[1] = 0; direction[2] = 1; }
	else if (angle == -2) { direction[0] = 0; direction[1] = 0; direction[2] = -1; }
	else { direction[0] = cosf (radians); direction[1] = sinf (radians); direction[2] = 0; }
}

bool Radiance_ParseMapLightsWithSurfaceTemplates (const char *text, size_t text_size, radiance_light_t *output,
	size_t capacity, radiance_surface_template_t *templates, size_t template_capacity,
	size_t *template_count, radiance_light_parse_stats_t *stats)
{
	radiance_light_parse_stats_t result = {0};
	radiance_parser_t parser = {text, text_size, 0};
	radiance_target_t *targets = NULL;
	radiance_reference_t *refs = NULL;
	size_t target_count = 0, target_capacity = 0;
	float global_anglescale = 0.5f, global_dist = 1.0f, global_range = 0.5f;
	bool valid = true;
	if (template_count) *template_count = 0;
	if ((!text && text_size) || (!output && capacity) || (template_capacity && (!templates || !template_count)) || capacity > SIZE_MAX / sizeof (*refs)) return false;
	if (capacity && !(refs = calloc (capacity, sizeof (*refs)))) return false;
	for (;;)
	{
		radiance_token_t token, value;
		int status = Radiance_NextToken (&parser, &token);
		if (status == 0) break;
		if (status < 0 || token.punctuation != '{') { valid = false; break; }
		radiance_entity_t e = {0};
		e.color[0] = e.color[1] = e.color[2] = 1;
		e.sun_color[0] = e.sun_color[1] = e.sun_color[2] = 1;
		e.upper_color[0] = e.upper_color[1] = e.upper_color[2] = 1;
		e.lower_color[0] = e.lower_color[1] = e.lower_color[2] = 1;
		e.light = 300; e.wait = 1; e.cone = 40;
		for (;;)
		{
			status = Radiance_NextToken (&parser, &token);
			if (status <= 0) { valid = false; break; }
			if (token.punctuation == '}') break;
			if (token.punctuation || Radiance_NextToken (&parser, &value) != 1 || value.punctuation) { valid = false; break; }
			/* Map tools can append whitespace to a key. */
			size_t len = strlen (token.text);
			while (len && token.text[len - 1] == ' ') token.text[--len] = 0;
			Radiance_ParseField (&e, token.text, value.text);
		}
		if (!valid) break;
		++result.entities;
		if (!Radiance_AppendTarget (&targets, &target_count, &target_capacity, &e)) { valid = false; break; }
		if (!e.has_color && e.has_embedded_color) memcpy (e.color, e.embedded_color, sizeof (e.color));
		if (!e.has_sun_color && e.has_embedded_sun_color) memcpy (e.sun_color, e.embedded_sun_color, sizeof (e.sun_color));
		if (!strcmp (e.classname, "worldspawn")) result.sky_authored |= e.has_sky_intensity;
		if (!strcmp (e.classname, "worldspawn") && !e.invalid)
		{
			if (e.has_world_dist) global_dist = fminf (100.0f, fmaxf (0.0f, e.world_dist));
			if (e.has_world_range) global_range = fminf (100.0f, fmaxf (0.0f, e.world_range));
			if (e.has_anglescale) global_anglescale = fminf (1.0f, fmaxf (0.0f, e.anglescale));
			Radiance_NormalizeColor (e.upper_color);
			Radiance_NormalizeColor (e.lower_color);
			if (e.sky_upper > 0)
			{
				memcpy (result.sky_upper, e.upper_color, sizeof (e.upper_color));
				result.sky_upper[3] = e.sky_upper / 300.0f;
			}
			if (e.sky_lower > 0)
			{
				memcpy (result.sky_lower, e.lower_color, sizeof (e.lower_color));
				result.sky_lower[3] = e.sky_lower / 300.0f;
			}
		}
		bool sun = !strcmp (e.classname, "worldspawn") && e.sun > 0;
		bool point = !strncmp (e.classname, "light", 5) ||
			!strcmp (e.classname, "dynamiclight") || e.shadowlight;
		bool kex = !strcmp (e.classname, "dynamiclight") || e.shadowlight;
		if (!sun && !point) continue;
		/* _surface is a compiler template and never emits at its own origin.
		 * The ordinary parser intentionally omits templates altogether. */
		bool surface_template = !kex && point && e.surface[0];
		if (surface_template && !templates) continue;
		++result.found;
		if (e.invalid || (point && !surface_template && !e.has_origin) || (point && e.has_radius && e.radius <= 0) ||
			(point && e.has_light && (e.light == 0 || (kex && e.light < 0))))
		{ ++result.malformed; continue; }
		if (result.written >= capacity) { ++result.dropped; continue; }
		radiance_light_t *light = &output[result.written];
		if (surface_template)
		{
			memcpy (refs[result.written].surface, e.surface, sizeof (e.surface));
			refs[result.written].surface_offset = e.surface_offset != 0 ? e.surface_offset : 2.0f;
			refs[result.written].surface_spotlight = e.surface_spotlight != 0;
		}
		memset (light, 0, sizeof (*light));
		light->metadata[2] = RADIANCE_LIGHT_SOURCE_MAP;
		if (kex) light->metadata[2] |= RADIANCE_LIGHT_SOURCE_KEX;
		if (!strcmp (e.classname, "dynamiclight") && (e.spawnflags & 1))
			light->metadata[2] |= RADIANCE_LIGHT_SOURCE_NOT_IN_COOP;
		light->metadata[1] = e.style >= 0 ? (uint32_t)e.style : 0;
		light->metadata[3] = kex ? 0u : Radiance_AttenuationDescriptor (e.delay, e.wait);
		/* Non-emissive area_axis carries optional legacy map-light parameters;
		 * the marker keeps older callers and KEX/dynamic lights unchanged. */
		if (!kex)
		{
			light->area_axis[0] = e.has_anglescale && e.anglescale >= 0 && e.anglescale <= 1 ? e.anglescale : -1.0f;
			light->area_axis[2] = 1.0f;
		}
		else
		{
			/* KEX map lights do not use the compiler-light area parameters.
			 * Keep their camera-distance fade with the immutable source. */
			light->area_axis[0] = e.kex_fade_start;
			light->area_axis[1] = e.kex_fade_end;
		}
		if (sun)
		{
			const float yaw = e.sun_angles[0] * 0.01745329251994329577f;
			const float pitch = (e.has_sun_angles ? e.sun_angles[1] : -90.0f) * 0.01745329251994329577f;
			light->direction_cone[0] = cosf (pitch) * cosf (yaw);
			light->direction_cone[1] = cosf (pitch) * sinf (yaw);
			light->direction_cone[2] = sinf (pitch);
			light->direction_cone[3] = -1;
			Radiance_NormalizeColor (e.sun_color);
			memcpy (light->color_intensity, e.sun_color, sizeof (e.sun_color));
			light->color_intensity[3] = e.sun / 300.0f;
			light->metadata[0] = RADIANCE_LIGHT_DIRECTIONAL;
		}
		else
		{
			const float wait = e.wait > 0 ? e.wait : 1.0f;
			const float magnitude = fabsf (e.light);
			refs[result.written].explicit_falloff = e.delay == 0 && e.falloff > 0;
			float radius = e.delay == 0 && e.falloff > 0 ? e.falloff : e.has_radius ? e.radius : magnitude / wait;
			/* Inverse lights reach much further than linear Quake lights. Bound
			 * them where the original compiler contribution falls below one
			 * lightmap unit. Infinite modes bypass this bound in the shader. */
			if (!e.has_radius && e.delay == 1) radius = magnitude * 128.0f / wait;
			else if (!e.has_radius && (e.delay == 2 || e.delay == 5))
				radius = fmaxf (128.0f, sqrtf (magnitude) * 128.0f - (e.delay == 5 ? 128.0f : 0.0f)) / wait;
			if (!isfinite (radius) || radius <= 0) { ++result.malformed; continue; }
			memcpy (light->position_radius, e.origin, sizeof (e.origin));
			light->position_radius[3] = radius;
			Radiance_NormalizeColor (e.color);
			memcpy (light->color_intensity, e.color, sizeof (e.color));
			light->color_intensity[3] = e.has_kex_intensity ? e.kex_intensity :
				(!strcmp (e.classname, "dynamiclight") ? 10.0f : e.light / 300.0f);
			light->direction_cone[3] = -1;
			/* KEX dynamiclight cone angles of zero or less mean an omni light,
			 * even if the entity has a target or orientation. The renderer's
			 * legacy entity-light parser uses the same rule. Compiler lights
			 * have independent angle and default-cone conventions. */
			bool spotlight = kex ? (e.has_cone && e.cone > 0 &&
				(e.target[0] || e.has_angle || e.has_mangle)) :
				(e.target[0] || e.has_cone || e.has_mangle || !strcmp (e.classname, "light_spot"));
			if (spotlight)
			{
				light->metadata[0] = RADIANCE_LIGHT_SPOT;
				/* Classic light `angle` is its cone width. KEX dynamic-light
				 * `angle` is an orientation and _shadowlightconeangle is its
				 * angular cutoff, matching R_ParseEntityDlights. */
				bool kex = e.shadowlight || !strcmp (e.classname, "dynamiclight");
				float cone = e.has_cone ? e.cone : (!kex && e.has_angle ? e.angle : 40.0f);
				if (!kex && cone <= 0) cone = 40.0f;
				light->direction_cone[3] = cosf (fminf (fmaxf (cone, 0.0f), 180.0f) * (kex ? 1.0f : 0.5f) * 0.01745329251994329577f);
				if (!kex)
				{
					if (e.has_softangle) light->metadata[2] |= RADIANCE_LIGHT_SOURCE_EXPLICIT_SOFTANGLE;
					float inner = e.softangle > 0 && e.softangle <= cone ? e.softangle : cone;
					light->area_axis[1] = cosf (fminf (inner, 180.0f) * 0.5f * 0.01745329251994329577f);
				}
				if (e.has_mangle)
				{
					const float yaw = e.mangle[0] * 0.01745329251994329577f;
					const float pitch = e.mangle[1] * 0.01745329251994329577f;
					light->direction_cone[0] = cosf (pitch) * cosf (yaw);
					light->direction_cone[1] = cosf (pitch) * sinf (yaw);
					light->direction_cone[2] = sinf (pitch);
				}
				else if (kex && e.has_angle) Radiance_AngleDirection (e.angle, light->direction_cone);
				Radiance_CopyString (refs[result.written].name, sizeof (refs[result.written].name), e.target);
			}
		}
		++result.written;
	}
	if (valid)
	{
		/* Worldspawn may occur after a light in nonstandard entity lumps. */
		for (size_t i = 0; i < result.written; ++i)
		{
			radiance_light_t *light = &output[i];
			if (light->area_axis[2] != 1.0f) continue; /* exclude KEX lights */
			if (light->area_axis[0] < 0) light->area_axis[0] = global_anglescale;
			/* The compiler's default _range is .5; normalize to the existing
			 * runtime default and affect only compiler-authored sources. */
			light->color_intensity[3] = (float)fmax (-(double)FLT_MAX,
				fmin ((double)FLT_MAX, (double)light->color_intensity[3] * global_range * 2.0));
			if (light->metadata[0] != RADIANCE_LIGHT_DIRECTIONAL && !refs[i].explicit_falloff)
			{
				/* At zero distance scale the inverse formulas are singular;
				 * a small positive value keeps their finite GPU representation safe. */
				float dist = fmaxf (global_dist, 1e-6f), wait;
				uint32_t bits = light->metadata[3] & ~UINT32_C(7);
				memcpy (&wait, &bits, sizeof (wait));
				int formula = (int)(light->metadata[3] & 7u);
				/* Linear light with _dist 0 never fades. */
				if (global_dist == 0.0f && formula == 0) formula = 3;
				light->metadata[3] = Radiance_AttenuationDescriptor (formula,
					(float)fmax (1e-6, fmin ((double)FLT_MAX, (double)wait * dist)));
				light->position_radius[3] = (float)fmin ((double)FLT_MAX, (double)light->position_radius[3] / dist);
			}
		}
		result.sky_upper[3] *= global_range * 2.0f;
		result.sky_lower[3] *= global_range * 2.0f;
		if (target_count > 1) qsort (targets, target_count, sizeof (*targets), Radiance_CompareTargets);
		for (size_t i = 0; i < result.written; ++i)
		{
			if (refs[i].name[0])
			{
				const radiance_target_t *target = Radiance_FindTarget (targets, target_count, refs[i].name);
				if (target)
				{
					float direction[3];
					for (int a = 0; a < 3; ++a) direction[a] = target->origin[a] - output[i].position_radius[a];
					if (Radiance_Normalize (direction)) memcpy (output[i].direction_cone, direction, sizeof (direction));
					else ++result.unresolved_targets;
				}
				else ++result.unresolved_targets;
			}
			if (output[i].metadata[0] == RADIANCE_LIGHT_SPOT && output[i].direction_cone[0] == 0 &&
				output[i].direction_cone[1] == 0 && output[i].direction_cone[2] == 0)
			{
				/* Without a usable target or angle, an omni light is preferable to
				 * silently dropping the light or normalizing a zero vector. */
				output[i].metadata[0] = RADIANCE_LIGHT_POINT;
				output[i].direction_cone[3] = -1;
			}
		}
	}
	if (valid && templates)
	{
		size_t ordinary = 0, surface_count = 0;
		for (size_t i = 0; i < result.written; ++i)
		{
			if (!refs[i].surface[0]) { output[ordinary++] = output[i]; continue; }
			if (surface_count == template_capacity) { ++result.dropped; continue; }
			radiance_surface_template_t *t = &templates[surface_count++];
			memset (t, 0, sizeof (*t)); t->light = output[i];
			memcpy (t->texture, refs[i].surface, sizeof (t->texture));
			t->offset = refs[i].surface_offset; t->surface_spotlight = refs[i].surface_spotlight;
		}
		result.written = ordinary;
		if (template_count) *template_count = surface_count;
	}
	free (targets); free (refs);
	if (stats) *stats = result;
	return valid;
}

bool Radiance_ParseMapLights (const char *text, size_t text_size, radiance_light_t *output,
	size_t capacity, radiance_light_parse_stats_t *stats)
{
	return Radiance_ParseMapLightsWithSurfaceTemplates (text, text_size, output, capacity, NULL, 0, NULL, stats);
}

void Radiance_ApplyDefaultSpotSoftness (radiance_light_t *light, float softness)
{
	if (!light || !isfinite (softness) || softness <= 0.0f ||
		light->metadata[0] != RADIANCE_LIGHT_SPOT ||
		(light->metadata[2] & (RADIANCE_LIGHT_SOURCE_MAP | RADIANCE_LIGHT_SOURCE_KEX |
			RADIANCE_LIGHT_SOURCE_EXPLICIT_SOFTANGLE)) != RADIANCE_LIGHT_SOURCE_MAP ||
		light->area_axis[2] != 1.0f || !isfinite (light->direction_cone[3])) return;
	float outer = fminf (1.0f, fmaxf (-1.0f, light->direction_cone[3]));
	light->area_axis[1] = cosf (acosf (outer) * (1.0f - fminf (softness, 1.0f)));
}

radiance_light_t Radiance_ApplyLightstyle (const radiance_light_t *source, const int *styles, size_t style_count)
{
	radiance_light_t output = {0};
	if (!source) return output;
	output = *source;
	uint32_t style = source->metadata[1];
	if (styles && style < style_count) output.color_intensity[3] *= fmaxf (0.0f, styles[style] / 256.0f);
	return output;
}

static int Radiance_LightGridBounds (const radiance_light_grid_t *grid, const radiance_light_t *light,
	uint32_t lo[3], uint32_t hi[3])
{
	const uint32_t formula = light->metadata[3] & 7u;
	if (light->metadata[0] == RADIANCE_LIGHT_DIRECTIONAL ||
		((light->metadata[2] & RADIANCE_LIGHT_SOURCE_MAP) && (formula == 3 || formula == 4)))
	{
		for (int a = 0; a < 3; ++a) { lo[a] = 0; hi[a] = grid->dimensions[a] - 1; }
		return 1;
	}
	double radius = light->position_radius[3];
	if (!isfinite (radius) || radius <= 0) return -1;
	if (light->metadata[0] == RADIANCE_LIGHT_EMISSIVE)
	{
		const double width=light->area_axis[3], height=light->direction_cone[3];
		if (!isfinite(width) || !isfinite(height) || width<0 || height<0) return -1;
		/* The falloff radius is measured from the nearest point of the
		 * rectangle, so a center-based grid bound must include its extent. */
		radius += hypot(width,height);
		if (!isfinite(radius)) return -1;
	}
	for (int a = 0; a < 3; ++a)
	{
		const double position = light->position_radius[a];
		if (!isfinite (position)) return -1;
		/* The shader chooses cells with float arithmetic; a small conservative
		 * margin covers rounding near cell boundaries at large coordinates. */
		const double margin = 0.00001 * (fabs (position) + radius + fabs (grid->origin[a]) + grid->cell_size[a]);
		double first = floor ((position - radius - margin - grid->origin[a]) / grid->cell_size[a]);
		double last = floor ((position + radius + margin - grid->origin[a]) / grid->cell_size[a]);
		if (last < 0 || first >= grid->dimensions[a]) return 0;
		if (first < 0) first = 0;
		if (last >= grid->dimensions[a]) last = grid->dimensions[a] - 1;
		lo[a] = (uint32_t)first; hi[a] = (uint32_t)last;
	}
	return 1;
}

bool Radiance_LightGridBoundsCover (const radiance_light_grid_t *grid,
	const radiance_light_t *old_light, const radiance_light_t *new_light)
{
	if (!grid || !old_light || !new_light || old_light->metadata[0] == RADIANCE_LIGHT_SPOT ||
		new_light->metadata[0] == RADIANCE_LIGHT_SPOT) return false;
	for (int a = 0; a < 3; ++a)
		if (!grid->dimensions[a] || !isfinite (grid->origin[a]) ||
			!isfinite (grid->cell_size[a]) || grid->cell_size[a] <= 0) return false;
	uint32_t old_lo[3], old_hi[3], new_lo[3], new_hi[3];
	int old_bounds = Radiance_LightGridBounds (grid, old_light, old_lo, old_hi);
	int new_bounds = Radiance_LightGridBounds (grid, new_light, new_lo, new_hi);
	if (old_bounds < 0 || new_bounds < 0) return false;
	if (!new_bounds) return true;
	if (!old_bounds) return false;
	for (int a = 0; a < 3; ++a)
		if (old_lo[a] > new_lo[a] || old_hi[a] < new_hi[a]) return false;
	return true;
}

/* Each plane bounds a pyramid enclosing the spotlight cone. A cell outside
 * any one plane is provably outside the cone. Coefficients are prepared once
 * per light: dynamic-light updates must not make rebuilding the grid costly. */
typedef struct { double base[4], step[4][3]; unsigned count; } radiance_spot_grid_t;

static radiance_spot_grid_t Radiance_SpotGrid (const radiance_light_grid_t *grid, const radiance_light_t *light)
{
	radiance_spot_grid_t result = {0};
	if (light->metadata[0] != RADIANCE_LIGHT_SPOT ||
		((light->metadata[2] & (RADIANCE_LIGHT_SOURCE_MAP | RADIANCE_LIGHT_SOURCE_KEX)) == RADIANCE_LIGHT_SOURCE_MAP &&
		 (light->metadata[3] & 7u) == 4u)) return result;
	double axis[3], length2 = 0, edge = light->direction_cone[3];
	bool compiler_light = (light->metadata[2] & (RADIANCE_LIGHT_SOURCE_MAP | RADIANCE_LIGHT_SOURCE_KEX)) == RADIANCE_LIGHT_SOURCE_MAP && light->area_axis[2] == 1.0f;
	if (!isfinite (edge)) return result;
	edge = fmax (-1.0, fmin (edge, compiler_light ? 1.0 : 0.9999));
	for (int a = 0; a < 3; ++a)
	{
		axis[a] = light->direction_cone[a];
		if (!isfinite (axis[a]) || !isfinite (light->position_radius[a])) return result;
		length2 += axis[a] * axis[a];
	}
	/* Wide or degenerate cones are retained conservatively. */
	if (edge <= 0 || length2 <= edge * edge || !isfinite (length2)) return result;
	double len = sqrt (length2), tangent[3], bitangent[3], inv_tangent, slope = sqrt (length2 - edge * edge) / edge;
	for (int a = 0; a < 3; ++a) axis[a] /= len;
	/* Pick an axis least parallel to the cone axis for a stable basis. */
	int pivot = fabs (axis[0]) < fabs (axis[1]) ? 0 : 1;
	if (fabs (axis[2]) < fabs (axis[pivot])) pivot = 2;
	for (int a = 0; a < 3; ++a) tangent[a] = (a == pivot ? 1.0 : 0.0) - axis[pivot] * axis[a];
	inv_tangent = 1.0 / sqrt (1.0 - axis[pivot] * axis[pivot]);
	for (int a = 0; a < 3; ++a) tangent[a] *= inv_tangent;
	bitangent[0] = axis[1]*tangent[2] - axis[2]*tangent[1];
	bitangent[1] = axis[2]*tangent[0] - axis[0]*tangent[2];
	bitangent[2] = axis[0]*tangent[1] - axis[1]*tangent[0];
	for (int p = 0; p < 4; ++p)
	{
		const double *basis = p < 2 ? tangent : bitangent;
		double sign = (p & 1) ? 1.0 : -1.0, base = 0, margin = 1;
		for (int a = 0; a < 3; ++a)
		{
			double normal = slope * axis[a] + sign * basis[a];
			result.step[p][a] = normal * grid->cell_size[a];
			base += normal * ((double)grid->origin[a] - light->position_radius[a] + grid->cell_size[a] * 0.5);
			base += fabs (normal) * grid->cell_size[a] * 0.5;
			margin += fabs (normal) * (fabs (grid->origin[a]) + fabs (light->position_radius[a]) + (double)grid->dimensions[a] * grid->cell_size[a]);
		}
		result.base[p] = base + 0.0001 * margin;
		if (!isfinite (result.base[p])) { result.count = 0; return result; }
	}
	result.count = 4;
	return result;
}

static bool Radiance_SpotGridXRange (const radiance_spot_grid_t *spot, uint32_t y, uint32_t z, uint32_t *lo, uint32_t *hi)
{
	for (unsigned p = 0; p < spot->count; ++p)
	{
		double origin = spot->base[p] + spot->step[p][1] * y + spot->step[p][2] * z;
		double slope = spot->step[p][0];
		if (slope == 0) { if (origin < 0) return false; continue; }
		double bound = -origin / slope;
		if (!isfinite (bound)) continue;
		/* Round outwards by up to one extra cell to cover arithmetic error. */
		if (slope > 0)
		{
			bound = floor (bound);
			if (bound > *hi) return false;
			if (bound > *lo) *lo = (uint32_t)bound;
		}
		else
		{
			bound = ceil (bound);
			if (bound < *lo) return false;
			if (bound < *hi) *hi = (uint32_t)bound;
		}
	}
	return true;
}

bool Radiance_BuildLightGrid (const radiance_light_grid_t *grid, const radiance_light_t *lights,
	size_t light_count, uint32_t *offsets, size_t offsets_capacity, uint32_t *indices,
	size_t indices_capacity, radiance_light_grid_stats_t *stats)
{
	radiance_light_grid_stats_t result = {0};
	uint32_t *counts = NULL;
	bool success = false;
	if (!grid || (!lights && light_count) || !offsets || (!indices && indices_capacity) || light_count > UINT32_MAX) goto done;
	result.cell_count = 1;
	for (int a = 0; a < 3; ++a)
	{
		if (!grid->dimensions[a] || !isfinite (grid->origin[a]) || !isfinite (grid->cell_size[a]) || grid->cell_size[a] <= 0 ||
			result.cell_count > (SIZE_MAX - 1) / grid->dimensions[a]) goto done;
		result.cell_count *= grid->dimensions[a];
	}
	if (result.cell_count > UINT32_MAX || offsets_capacity <= result.cell_count || result.cell_count > SIZE_MAX / sizeof (*counts)) goto done;
	counts = calloc (result.cell_count, sizeof (*counts));
	if (!counts) goto done;
	for (size_t i = 0; i < light_count; ++i)
	{
		uint32_t lo[3], hi[3];
		int valid = Radiance_LightGridBounds (grid, &lights[i], lo, hi);
		radiance_spot_grid_t spot = Radiance_SpotGrid (grid, &lights[i]);
		if (valid < 0) goto done;
		if (!valid) continue;
		++result.indexed_lights;
		for (uint32_t z = lo[2]; z <= hi[2]; ++z)
			for (uint32_t y = lo[1]; y <= hi[1]; ++y)
			{
				uint32_t first = lo[0], last = hi[0];
				if (!Radiance_SpotGridXRange (&spot, y, z, &first, &last)) continue;
				size_t row = ((size_t)z * grid->dimensions[1] + y) * grid->dimensions[0];
				++counts[row + first];
				if (last < grid->dimensions[0] - 1) --counts[row + last + 1];
			}
	}
	offsets[0] = 0;
	for (size_t row = 0; row < result.cell_count; row += grid->dimensions[0])
	{
		/* Unsigned differences wrap for end markers. The running sum is exact:
		 * each of at most UINT32_MAX lights contributes at most once per cell. */
		uint32_t active = 0;
		for (size_t cell = row; cell < row + grid->dimensions[0]; ++cell)
		{
			active += counts[cell];
			if (result.required_indices > UINT32_MAX - active) goto done;
			result.required_indices += active;
			offsets[cell + 1] = (uint32_t)result.required_indices;
			counts[cell] = offsets[cell]; /* reuse as write cursor */
		}
	}
	if (indices_capacity < result.required_indices || (!indices && result.required_indices)) goto done;
	for (size_t i = 0; i < light_count; ++i)
	{
		uint32_t lo[3], hi[3];
		if (Radiance_LightGridBounds (grid, &lights[i], lo, hi) != 1) continue;
		radiance_spot_grid_t spot = Radiance_SpotGrid (grid, &lights[i]);
		for (uint32_t z = lo[2]; z <= hi[2]; ++z)
			for (uint32_t y = lo[1]; y <= hi[1]; ++y)
			{
				uint32_t first = lo[0], last = hi[0];
				if (!Radiance_SpotGridXRange (&spot, y, z, &first, &last)) continue;
				for (uint32_t x = first; x <= last; ++x)
				{
					size_t cell = ((size_t)z * grid->dimensions[1] + y) * grid->dimensions[0] + x;
					indices[counts[cell]++] = (uint32_t)i;
				}
			}
	}
	success = true;
done:
	free (counts);
	if (stats) *stats = result;
	return success;
}

bool Radiance_MakeDynamicLight (radiance_light_t *output, const float position[3], float radius,
	const float color[3], float intensity, const float direction[3], float cone_cos, double die, double now)
{
	if (!output || !position || !color || !isfinite (radius) || radius <= 0 || !isfinite (intensity) || intensity <= 0 ||
		!isfinite (die) || !isfinite (now) || die < now || !isfinite (cone_cos)) return false;
	for (int i = 0; i < 3; ++i) if (!isfinite (position[i]) || !isfinite (color[i])) return false;
	memset (output, 0, sizeof (*output));
	memcpy (output->position_radius, position, sizeof (float) * 3); output->position_radius[3] = radius;
	for (int i = 0; i < 3; ++i) output->color_intensity[i] = fmaxf (color[i], 0);
	output->color_intensity[3] = intensity;
	output->direction_cone[3] = -1;
	output->metadata[2] = RADIANCE_LIGHT_SOURCE_DYNAMIC;
	if (direction && cone_cos > -1)
	{
		float dir[3] = {direction[0], direction[1], direction[2]};
		if (!Radiance_Normalize (dir)) return false;
		memcpy (output->direction_cone, dir, sizeof (dir));
		output->direction_cone[3] = fminf (cone_cos, 1);
		output->metadata[0] = RADIANCE_LIGHT_SPOT;
	}
	return true;
}

void Radiance_ConfigureLegacyDynamicLight (radiance_light_t *light, float minlight)
{
	if (!light || !(light->metadata[2] & RADIANCE_LIGHT_SOURCE_DYNAMIC)) return;
	if (!isfinite (minlight) || minlight < 0) minlight = 0;
	light->metadata[2] |= RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC;
	memcpy (&light->metadata[1], &minlight, sizeof (minlight));
	light->color_intensity[3] = light->position_radius[3] * (3.14159265358979323846f / 128.0f);
}

static float Radiance_SRGBToLinear (uint8_t value)
{
	float s = value / 255.0f;
	return s <= 0.04045f ? s / 12.92f : powf ((s + 0.055f) / 1.055f, 2.4f);
}

bool Radiance_AnalyzeIndexedEmission (const uint8_t *pixels, size_t count, const uint8_t palette_rgba[1024],
	bool transparent_255, radiance_emissive_t *output)
{
	if ((!pixels && count) || !palette_rgba || !output) return false;
	memset (output, 0, sizeof (*output));
	if (!count) return true;
	double sum[3] = {0}, coverage = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const unsigned index = pixels[i];
		if (index < 224 || (index == 255 && transparent_255)) continue;
		const uint8_t *rgba = &palette_rgba[index * 4];
		float alpha = rgba[3] / 255.0f;
		if (alpha <= 0) continue;
		++output->emitting_texels;
		coverage += alpha;
		for (int c = 0; c < 3; ++c) sum[c] += Radiance_SRGBToLinear (rgba[c]) * alpha;
	}
	output->coverage = (float)(coverage / count);
	for (int c = 0; c < 3; ++c) output->radiance[c] = (float)(sum[c] / count);
	return true;
}

bool Radiance_AnalyzeMaskEmission (const uint8_t *rgba, size_t count, radiance_emissive_t *output)
{
	if ((!rgba && count) || !output || count > SIZE_MAX / 4) return false;
	memset (output, 0, sizeof (*output));
	if (!count) return true;
	double sum[3] = {0}, coverage = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const uint8_t *pixel = rgba + i * 4;
		float alpha = pixel[3] / 255.0f;
		if (alpha <= 0) continue;
		++output->emitting_texels;
		coverage += alpha;
		for (int c = 0; c < 3; ++c) sum[c] += Radiance_SRGBToLinear (pixel[c]) * alpha;
	}
	output->coverage = (float)(coverage / count);
	for (int c = 0; c < 3; ++c) output->radiance[c] = (float)(sum[c] / count);
	return true;
}

/* Used only for maps lacking an authored sky. A missing/black texture, zero
 * gain, or absent portal leaves the caller's authored parameters untouched. */
bool Radiance_ApplySkyFallback (const radiance_light_parse_stats_t *stats, bool has_portal,
    const float color[3], float gain, float upper[4])
{
    if (!stats || !color || !upper || stats->sky_authored || !has_portal ||
        !isfinite (gain) || gain <= 0) return false;
    for (int i = 0; i < 3; ++i)
        if (!isfinite (color[i]) || color[i] < 0 || color[i] > 1) return false;
    if (color[0] == 0 && color[1] == 0 && color[2] == 0) return false;
    memcpy (upper, color, 3 * sizeof (float));
    upper[3] = fminf (gain, 1.0f);
    return true;
}

bool Radiance_SkyLightingChanged (const float previous[8], bool previous_portals_only,
    const radiance_lighting_parameters_t *current)
{
    return !previous || !current || memcmp (previous, current->sky_radiance, 4 * sizeof (float)) ||
        memcmp (previous + 4, current->ground_radiance, 4 * sizeof (float)) ||
        previous_portals_only != ((current->counts[3] & 16u) != 0);
}
