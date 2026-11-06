/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "editor_map.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The map format stores editor-side brush planes, not the clipped polygons
 * in a BSP. Preserve each complete texture mapping tail when editing planes. */
static char *CopyString (const char *s, size_t n)
{
	if (n == SIZE_MAX)
		return NULL;
	char *copy = malloc (n + 1);
	if (copy)
	{
		memcpy (copy, s, n);
		copy[n] = 0;
	}
	return copy;
}

static bool Grow (void **array, size_t count, size_t element_size)
{
	if (count > SIZE_MAX / element_size)
		return false;
	void *p = realloc (*array, count * element_size);
	if (!p)
		return false;
	*array = p;
	return true;
}

void EditorMap_Free (editor_map_t *map)
{
	if (!map)
		return;
	for (size_t e = 0; e < map->num_entities; ++e)
	{
		editor_map_entity_t *entity = &map->entities[e];
		for (size_t i = 0; i < entity->num_properties; ++i)
		{
			free (entity->properties[i].key);
			free (entity->properties[i].value);
		}
		free (entity->properties);
		for (size_t i = 0; i < entity->num_brushes; ++i)
		{
			for (size_t j = 0; j < entity->brushes[i].num_faces; ++j)
				free (entity->brushes[i].faces[j].mapping);
			free (entity->brushes[i].faces);
		}
		free (entity->brushes);
	}
	free (map->entities);
	memset (map, 0, sizeof (*map));
}

const char *EditorMap_GetProperty (const editor_map_entity_t *entity, const char *key)
{
	if (!entity || !key)
		return NULL;
	for (size_t i = 0; i < entity->num_properties; ++i)
		if (strcmp (entity->properties[i].key, key) == 0)
			return entity->properties[i].value;
	return NULL;
}

bool EditorMap_SetProperty (editor_map_entity_t *entity, const char *key, const char *value)
{
	if (!entity || !key || !*key || !value || strpbrk (key, "\r\n") || strpbrk (value, "\r\n"))
		return false;
	char *new_value = CopyString (value, strlen (value));
	if (!new_value)
		return false;
	for (size_t i = 0; i < entity->num_properties; ++i)
		if (!strcmp (entity->properties[i].key, key))
		{
			free (entity->properties[i].value);
			entity->properties[i].value = new_value;
			return true;
		}
	char *new_key = CopyString (key, strlen (key));
	if (!new_key || entity->num_properties == SIZE_MAX ||
		!Grow ((void **)&entity->properties, entity->num_properties + 1, sizeof (*entity->properties)))
	{
		free (new_key);
		free (new_value);
		return false;
	}
	entity->properties[entity->num_properties++] = (editor_map_property_t){new_key, new_value};
	return true;
}

bool EditorMap_AddEntity (editor_map_t *map, const char *classname, size_t *out_index)
{
	if (!map || !classname || !*classname || map->num_entities == SIZE_MAX ||
		!Grow ((void **)&map->entities, map->num_entities + 1, sizeof (*map->entities)))
		return false;
	editor_map_entity_t *e = &map->entities[map->num_entities];
	memset (e, 0, sizeof (*e));
	if (!EditorMap_SetProperty (e, "classname", classname))
		return false;
	if (out_index)
		*out_index = map->num_entities;
	++map->num_entities;
	return true;
}

bool EditorMap_Init (editor_map_t *map)
{
	if (!map)
		return false;
	editor_map_t result = {0};
	if (!EditorMap_AddEntity (&result, "worldspawn", NULL))
	{
		EditorMap_Free (&result);
		return false;
	}
	EditorMap_Free (map);
	*map = result;
	return true;
}

bool EditorMap_RemoveBrush (editor_map_entity_t *entity, size_t index)
{
	if (!entity || index >= entity->num_brushes)
		return false;
	editor_map_brush_t *b = &entity->brushes[index];
	for (size_t i = 0; i < b->num_faces; ++i)
		free (b->faces[i].mapping);
	free (b->faces);
	memmove (b, b + 1, (entity->num_brushes - index - 1) * sizeof (*b));
	--entity->num_brushes;
	return true;
}

bool EditorMap_RemoveEntity (editor_map_t *map, size_t index)
{
	if (!map || index == 0 || index >= map->num_entities)
		return false;
	editor_map_entity_t *e = &map->entities[index];
	for (size_t i = 0; i < e->num_properties; ++i)
	{
		free (e->properties[i].key);
		free (e->properties[i].value);
	}
	free (e->properties);
	while (e->num_brushes)
		EditorMap_RemoveBrush (e, e->num_brushes - 1);
	free (e->brushes);
	memmove (e, e + 1, (map->num_entities - index - 1) * sizeof (*e));
	--map->num_entities;
	return true;
}

bool EditorMap_GetPointOrigin (const editor_map_t *map, size_t index, double origin[3])
{
	if (!map || !origin || index == 0 || index >= map->num_entities || map->entities[index].num_brushes)
		return false;
	const editor_map_entity_t *entity = &map->entities[index];
	const char *classname = EditorMap_GetProperty (entity, "classname");
	if (!classname || !*classname) return false;
	const char *p = EditorMap_GetProperty (entity, "origin");
	if (!p || !*p) { origin[0] = origin[1] = origin[2] = 0; return true; }
	for (int i = 0; i < 3; ++i)
	{
		while (isspace ((unsigned char)*p)) ++p;
		char *end; double value = strtod (p, &end);
		if (end == p || !isfinite (value)) return false;
		origin[i] = value; p = end;
	}
	while (isspace ((unsigned char)*p)) ++p;
	return !*p;
}

bool EditorMap_SetPointOrigin (editor_map_t *map, size_t index, const double origin[3])
{
	double old[3];
	if (!origin || !EditorMap_GetPointOrigin (map, index, old)) return false;
	for (int i = 0; i < 3; ++i) if (!isfinite (origin[i]) || fabs (origin[i]) > EDITOR_MAP_POINT_COORD_LIMIT) return false;
	char value[160];
	int n = snprintf (value, sizeof (value), "%.17g %.17g %.17g", origin[0], origin[1], origin[2]);
	return n > 0 && (size_t)n < sizeof (value) && EditorMap_SetProperty (&map->entities[index], "origin", value);
}

bool EditorMap_AddLight (editor_map_t *map, const double origin[3], size_t *out_index)
{
	if (!map || !origin) return false;
	for (int i = 0; i < 3; ++i) if (!isfinite (origin[i]) || fabs (origin[i]) > EDITOR_MAP_POINT_COORD_LIMIT) return false;
	editor_map_t copy = {0}; size_t index;
	if (!EditorMap_Clone (&copy, map)) return false;
	if (!EditorMap_AddEntity (&copy, "light", &index) ||
		!EditorMap_SetPointOrigin (&copy, index, origin) ||
		!EditorMap_SetProperty (&copy.entities[index], "light", "300"))
	{ EditorMap_Free (&copy); return false; }
	EditorMap_Free (map); *map = copy;
	if (out_index) *out_index = index;
	return true;
}

static bool EditorMap_IsLight (const editor_map_t *map, size_t index)
{
	if (!map || !index || index >= map->num_entities || map->entities[index].num_brushes) return false;
	const char *classname = EditorMap_GetProperty (&map->entities[index], "classname");
	return classname && !strcmp (classname, "light");
}

static bool EditorMap_ParseNumbers (const char *text, double *values, unsigned count);

bool EditorMap_GetLightColor (const editor_map_t *map, size_t index, double rgb[3])
{
	if (!rgb || !EditorMap_IsLight (map, index)) return false;
	double value[4] = {1, 1, 1, 300};
	const editor_map_entity_t *entity = &map->entities[index];
	/* LIGHT treats underscore-prefixed keys as aliases. Its sorted dictionary
	 * gives `color` priority if both are authored; an edit canonicalizes them. */
	const char *text = EditorMap_GetProperty (entity, "color");
	if (!text || !*text) text = EditorMap_GetProperty (entity, "_color");
	if (text && *text)
	{
		if (!EditorMap_ParseNumbers (text, value, 3)) goto white;
	}
	else
	{
		/* RGB-valued `light` is another accepted imported-map convention. */
		text = EditorMap_GetProperty (entity, "light");
		if (!text || !*text) text = EditorMap_GetProperty (entity, "_light");
		if (text && *text && !EditorMap_ParseNumbers (text, value, 4)) goto white;
	}
	double scale = value[0] > 1 || value[1] > 1 || value[2] > 1 ? 1.0 / 255 : 1;
	/* The inspector's editable range is SDR. Reading never rewrites an
	 * imported out-of-range/invalid source; only an explicit edit does. */
	for (unsigned i = 0; i < 3; ++i) rgb[i] = fmin (1, fmax (0, value[i] * scale));
	return true;
white:
	rgb[0] = rgb[1] = rgb[2] = 1;
	return true;
}

bool EditorMap_SetLightColor (editor_map_t *map, size_t index, const double rgb[3])
{
	if (!rgb || !EditorMap_IsLight (map, index)) return false;
	for (unsigned i = 0; i < 3; ++i)
		if (!isfinite (rgb[i]) || rgb[i] < 0 || rgb[i] > 1) return false;
	char value[192];
	int length = snprintf (value, sizeof (value), "%.17g %.17g %.17g", rgb[0], rgb[1], rgb[2]);
	if (length <= 0 || (size_t)length >= sizeof (value)) return false;
	/* Canonicalize the legacy alias transactionally. Otherwise `color` can
	 * override the inspector's `_color` in both LIGHT and Radiance. */
	editor_map_t copy = {0};
	if (!EditorMap_Clone (&copy, map)) return false;
	editor_map_entity_t *entity = &copy.entities[index];
	if (!EditorMap_SetProperty (entity, "_color", value)) { EditorMap_Free (&copy); return false; }
	for (size_t i = 0; i < entity->num_properties;)
	{
		if (strcmp (entity->properties[i].key, "color")) { ++i; continue; }
		free (entity->properties[i].key); free (entity->properties[i].value);
		memmove (&entity->properties[i], &entity->properties[i + 1], (--entity->num_properties - i) * sizeof (*entity->properties));
	}
	EditorMap_Free (map); *map = copy;
	return true;
}

bool EditorMap_DuplicatePoint (editor_map_t *map, size_t index, const double offset[3], size_t *out_index)
{
	double origin[3];
	if (!map || !offset || !EditorMap_GetPointOrigin (map, index, origin)) return false;
	for (int i = 0; i < 3; ++i)
	{
		if (!isfinite (offset[i]) || !isfinite (origin[i] + offset[i]) || fabs (origin[i] + offset[i]) > EDITOR_MAP_POINT_COORD_LIMIT) return false;
		origin[i] += offset[i];
	}
	const editor_map_entity_t *source = &map->entities[index];
	const char *classname = EditorMap_GetProperty (source, "classname");
	editor_map_t copy = {0}; size_t added;
	if (!EditorMap_Clone (&copy, map)) return false;
	if (!EditorMap_AddEntity (&copy, classname, &added)) { EditorMap_Free (&copy); return false; }
	for (size_t i = 0; i < source->num_properties; ++i)
	{
		const editor_map_property_t *property = &source->properties[i];
		if (!strcmp (property->key, "classname") || !strcmp (property->key, "origin")) continue;
		if (!EditorMap_SetProperty (&copy.entities[added], property->key, property->value))
		{ EditorMap_Free (&copy); return false; }
	}
	if (!EditorMap_SetPointOrigin (&copy, added, origin)) { EditorMap_Free (&copy); return false; }
	EditorMap_Free (map); *map = copy;
	if (out_index) *out_index = added;
	return true;
}

bool EditorMap_ValidModelPath (const char *path)
{
	if (!path || !*path || strlen (path) >= EDITOR_MAP_MODEL_PATH_MAX || *path == '/' ||
		strstr (path, "..") || strstr (path, "//")) return false;
	for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
		if (!isalnum (*p) && *p != '_' && *p != '-' && *p != '.' && *p != '/') return false;
	const char *extension = strrchr (path, '.');
	if (!extension) return false;
	char lower[10]; size_t n = strlen (extension);
	if (n >= sizeof (lower)) return false;
	for (size_t i = 0; i <= n; ++i) lower[i] = (char)tolower ((unsigned char)extension[i]);
	return !strcmp (lower, ".mdl") || !strcmp (lower, ".md3") || !strcmp (lower, ".md5mesh");
}

bool EditorMap_IsModel (const editor_map_t *map, size_t index)
{
	if (!map || !index || index >= map->num_entities || map->entities[index].num_brushes) return false;
	const char *classname = EditorMap_GetProperty (&map->entities[index], "classname");
	return classname && !strcmp (classname, "quiver_static_model");
}

static bool EditorMap_ParseNumbers (const char *text, double *values, unsigned count)
{
	if (!text || !*text) return false;
	for (unsigned i = 0; i < count; ++i)
	{
		while (isspace ((unsigned char)*text)) ++text;
		char *end; values[i] = strtod (text, &end);
		if (end == text || !isfinite (values[i])) return false;
		text = end;
	}
	while (isspace ((unsigned char)*text)) ++text;
	return !*text;
}

static bool EditorMap_ValidModel (const editor_map_model_t *model)
{
	if (!model || !memchr (model->model, 0, sizeof (model->model)) || !EditorMap_ValidModelPath (model->model) ||
		!isfinite (model->modelscale) || model->modelscale <= 0 || model->modelscale > FLT_MAX ||
		(float)model->modelscale <= 0 || model->skin < 0) return false;
	for (unsigned i = 0; i < 3; ++i)
		if (!isfinite (model->origin[i]) || fabs (model->origin[i]) > EDITOR_MAP_POINT_COORD_LIMIT ||
			!isfinite (model->angles[i]) || fabs (model->angles[i]) > EDITOR_MAP_POINT_COORD_LIMIT) return false;
	return true;
}

bool EditorMap_GetModel (const editor_map_t *map, size_t index, editor_map_model_t *model)
{
	if (!model || !EditorMap_IsModel (map, index)) return false;
	const editor_map_entity_t *entity = &map->entities[index];
	editor_map_model_t value = {.modelscale = 1, .collision = true};
	const char *path = EditorMap_GetProperty (entity, "model");
	if (!EditorMap_ValidModelPath (path)) return false;
	memcpy (value.model, path, strlen (path) + 1);
	if (!EditorMap_GetPointOrigin (map, index, value.origin)) return false;
	const char *text = EditorMap_GetProperty (entity, "angles");
	if (text && *text && !EditorMap_ParseNumbers (text, value.angles, 3)) return false;
	text = EditorMap_GetProperty (entity, "modelscale");
	if (text && *text && !EditorMap_ParseNumbers (text, &value.modelscale, 1)) return false;
	text = EditorMap_GetProperty (entity, "skin");
	if (text && *text)
	{
		double skin;
		if (!EditorMap_ParseNumbers (text, &skin, 1) || skin < 0 || skin > INT_MAX || floor (skin) != skin) return false;
		value.skin = (int)skin;
	}
	text = EditorMap_GetProperty (entity, "collision");
	if (text && *text)
	{
		if (strcmp (text, "0") && strcmp (text, "1")) return false;
		value.collision = !strcmp (text, "1");
	}
	if (!EditorMap_ValidModel (&value)) return false;
	*model = value;
	return true;
}

bool EditorMap_SetModel (editor_map_t *map, size_t index, const editor_map_model_t *model)
{
	if (!EditorMap_IsModel (map, index) || !EditorMap_ValidModel (model)) return false;
	editor_map_t copy = {0};
	if (!EditorMap_Clone (&copy, map)) return false;
	char angles[160], scale[64], skin[32];
	snprintf (angles, sizeof (angles), "%.17g %.17g %.17g", model->angles[0], model->angles[1], model->angles[2]);
	snprintf (scale, sizeof (scale), "%.17g", model->modelscale); snprintf (skin, sizeof (skin), "%d", model->skin);
	editor_map_entity_t *entity = &copy.entities[index];
	if (!EditorMap_SetProperty (entity, "model", model->model) || !EditorMap_SetPointOrigin (&copy, index, model->origin) ||
		!EditorMap_SetProperty (entity, "angles", angles) || !EditorMap_SetProperty (entity, "modelscale", scale) ||
		!EditorMap_SetProperty (entity, "skin", skin) || !EditorMap_SetProperty (entity, "collision", model->collision ? "1" : "0"))
	{ EditorMap_Free (&copy); return false; }
	EditorMap_Free (map); *map = copy; return true;
}

bool EditorMap_AddModel (editor_map_t *map, const char *path, const double origin[3], size_t *out_index)
{
	if (!map || !origin || !EditorMap_ValidModelPath (path)) return false;
	editor_map_model_t value = {.modelscale = 1, .collision = true};
	memcpy (value.model, path, strlen (path) + 1); memcpy (value.origin, origin, sizeof (value.origin));
	if (!EditorMap_ValidModel (&value)) return false;
	editor_map_t copy = {0}; size_t index;
	if (!EditorMap_Clone (&copy, map)) return false;
	if (!EditorMap_AddEntity (&copy, "quiver_static_model", &index) || !EditorMap_SetModel (&copy, index, &value))
	{ EditorMap_Free (&copy); return false; }
	EditorMap_Free (map); *map = copy;
	if (out_index) *out_index = index;
	return true;
}

bool EditorMap_ModelCorners (const editor_map_model_t *model, const double mins[3], const double maxs[3], double corners[8][3])
{
	if (!mins || !maxs || !corners || !EditorMap_ValidModel (model)) return false;
	for(unsigned a=0;a<3;++a)if(!isfinite(mins[a]) || !isfinite(maxs[a]) || mins[a]>maxs[a])return false;
	const double radians=3.14159265358979323846/180;
	double cp=cos(model->angles[0]*radians),sp=sin(model->angles[0]*radians);
	double cy=cos(model->angles[1]*radians),sy=sin(model->angles[1]*radians);
	double cr=cos(model->angles[2]*radians),sr=sin(model->angles[2]*radians);
	double result[8][3];
	for(unsigned i=0;i<8;++i)
	{
		double x=(i&1?maxs[0]:mins[0])*model->modelscale;
		double y=(i&2?maxs[1]:mins[1])*model->modelscale;
		double z=(i&4?maxs[2]:mins[2])*model->modelscale;
		double ry=cr*y-sr*z,rz=sr*y+cr*z;
		double px=cp*x-sp*rz,pz=sp*x+cp*rz;
		result[i][0]=model->origin[0]+cy*px-sy*ry;
		result[i][1]=model->origin[1]+sy*px+cy*ry;
		result[i][2]=model->origin[2]+pz;
		for(unsigned a=0;a<3;++a)if(!isfinite(result[i][a]) || fabs(result[i][a])>FLT_MAX)return false;
	}
	memcpy(corners,result,sizeof(result));return true;
}

typedef struct
{
	const char *begin, *cursor, *end;
	char *error;
	size_t error_size;
} parser_t;

static bool ParseError (parser_t *p, const char *message)
{
	if (p->error && p->error_size)
	{
		size_t line = 1;
		for (const char *c = p->begin; c < p->cursor && c < p->end; ++c)
			line += *c == '\n';
		snprintf (p->error, p->error_size, "line %zu: %s", line, message);
	}
	return false;
}

static bool SkipSpace (parser_t *p)
{
	for (;;)
	{
		while (p->cursor < p->end && isspace ((unsigned char)*p->cursor))
			++p->cursor;
		if (p->end - p->cursor < 2 || p->cursor[0] != '/')
			return true;
		if (p->cursor[1] == '/')
		{
			p->cursor += 2;
			while (p->cursor < p->end && *p->cursor != '\n')
				++p->cursor;
		}
		else if (p->cursor[1] == '*')
		{
			p->cursor += 2;
			while (p->end - p->cursor >= 2 && !(p->cursor[0] == '*' && p->cursor[1] == '/'))
				++p->cursor;
			if (p->end - p->cursor < 2)
				return ParseError (p, "unterminated comment");
			p->cursor += 2;
		}
		else
			return true;
	}
}

static bool Consume (parser_t *p, char c)
{
	if (!SkipSpace (p))
		return false;
	if (p->cursor == p->end || *p->cursor != c)
		return ParseError (p, "unexpected token");
	++p->cursor;
	return true;
}

static char *ParseQuoted (parser_t *p)
{
	if (!Consume (p, '"'))
		return NULL;
	const char *start = p->cursor;
	const char *end = start;
	while (end < p->end && *end != '"')
	{
		if (*end == '\n' || *end == '\r' || !*end)
		{
			p->cursor = end;
			ParseError (p, "unterminated quoted string");
			return NULL;
		}
		if (*end == '\\' && end + 1 < p->end && (end[1] == '"' || end[1] == '\\'))
			++end;
		++end;
	}
	if (end == p->end)
	{
		p->cursor = end;
		ParseError (p, "unterminated quoted string");
		return NULL;
	}
	char *result = malloc ((size_t)(end - start) + 1);
	if (!result)
	{
		ParseError (p, "out of memory");
		return NULL;
	}
	size_t n = 0;
	while (start < end)
	{
		char c = *start++;
		if (c == '\\' && start < end && (*start == '"' || *start == '\\'))
			c = *start++;
		result[n++] = c;
	}
	result[n] = 0;
	p->cursor = end + 1;
	return result;
}

static bool ParseNumber (parser_t *p, double *number)
{
	if (!SkipSpace (p) || p->cursor == p->end)
		return ParseError (p, "expected coordinate");
	const char *start = p->cursor;
	while (p->cursor < p->end && !isspace ((unsigned char)*p->cursor) && *p->cursor != ')')
		++p->cursor;
	char *token = CopyString (start, (size_t)(p->cursor - start));
	if (!token)
		return ParseError (p, "out of memory");
	char *end;
	double value = strtod (token, &end);
	bool ok = end != token && *end == 0 && isfinite (value);
	free (token);
	if (!ok)
		return ParseError (p, "invalid coordinate");
	*number = value;
	return true;
}

/* Validate the required mapping fields while retaining the original tail for
 * Valve 220 axes, compiler extensions and comments when writing the map. */
static bool MappingNumber (const char **cursor)
{
	while (isspace ((unsigned char)**cursor))
		++*cursor;
	char *end;
	double value = strtod (*cursor, &end);
	if (end == *cursor || !isfinite (value) || (*end && !isspace ((unsigned char)*end) && *end != ']'))
		return false;
	*cursor = end;
	return true;
}

static bool ValidMapping (const char *mapping)
{
	const char *p = mapping;
	if (*p == '"')
	{
		++p;
		while (*p && *p != '"')
			++p;
		if (*p++ != '"')
			return false;
	}
	else
	{
		while (*p && !isspace ((unsigned char)*p))
			++p;
		if (p == mapping)
			return false;
	}
	while (isspace ((unsigned char)*p))
		++p;
	if (*p == '[')
	{
		for (int axis = 0; axis < 2; ++axis)
		{
			while (isspace ((unsigned char)*p))
				++p;
			if (*p++ != '[')
				return false;
			for (int component = 0; component < 4; ++component)
				if (!MappingNumber (&p))
					return false;
			while (isspace ((unsigned char)*p))
				++p;
			if (*p++ != ']')
				return false;
		}
		for (int i = 0; i < 3; ++i)
			if (!MappingNumber (&p))
				return false;
	}
	else
		for (int i = 0; i < 5; ++i)
			if (!MappingNumber (&p))
				return false;
	return true;
}

static bool ParseFace (parser_t *p, editor_map_brush_t *brush)
{
	editor_map_face_t face = {0};
	for (int vertex = 0; vertex < 3; ++vertex)
	{
		if (!Consume (p, '('))
			return false;
		for (int axis = 0; axis < 3; ++axis)
			if (!ParseNumber (p, &face.points[vertex][axis]))
				return false;
		if (!Consume (p, ')'))
			return false;
	}
	while (p->cursor < p->end && (*p->cursor == ' ' || *p->cursor == '\t'))
		++p->cursor;
	const char *begin = p->cursor;
	bool quoted = false;
	while (p->cursor < p->end && *p->cursor != '\n' && *p->cursor != '\r')
	{
		if (*p->cursor == '"' && (p->cursor == begin || p->cursor[-1] != '\\'))
			quoted = !quoted;
		if (!quoted && p->end - p->cursor >= 2 && p->cursor[0] == '/' && p->cursor[1] == '/')
		{
			while (p->cursor < p->end && *p->cursor != '\n' && *p->cursor != '\r')
				++p->cursor;
			break;
		}
		if (!quoted && (*p->cursor == '}' || *p->cursor == '('))
			break;
		++p->cursor;
	}
	const char *end = p->cursor;
	while (end > begin && isspace ((unsigned char)end[-1]))
		--end;
	if (begin == end || quoted)
		return ParseError (p, "missing or unterminated texture mapping");
	face.mapping = CopyString (begin, (size_t)(end - begin));
	if (face.mapping && !ValidMapping (face.mapping))
	{
		free (face.mapping);
		return ParseError (p, "invalid texture mapping");
	}
	if (!face.mapping || brush->num_faces == SIZE_MAX || !Grow ((void **)&brush->faces, brush->num_faces + 1, sizeof (*brush->faces)))
	{
		free (face.mapping);
		return ParseError (p, "out of memory");
	}
	brush->faces[brush->num_faces++] = face;
	return true;
}

static bool ParseBrush (parser_t *p, editor_map_entity_t *entity)
{
	if (!Consume (p, '{'))
		return false;
	if (entity->num_brushes == SIZE_MAX || !Grow ((void **)&entity->brushes, entity->num_brushes + 1, sizeof (*entity->brushes)))
		return ParseError (p, "out of memory");
	editor_map_brush_t *brush = &entity->brushes[entity->num_brushes++];
	memset (brush, 0, sizeof (*brush));
	for (;;)
	{
		if (!SkipSpace (p) || p->cursor == p->end)
			return ParseError (p, "unterminated brush");
		if (*p->cursor == '}')
		{
			++p->cursor;
			return brush->num_faces >= 4 || ParseError (p, "brush has fewer than four faces");
		}
		if (*p->cursor != '(')
			return ParseError (p, "unsupported brush syntax");
		if (!ParseFace (p, brush))
			return false;
	}
}

bool EditorMap_Parse (editor_map_t *map, const char *text, size_t length, char *error, size_t error_size)
{
	if (error && error_size)
		error[0] = 0;
	if (!map || !text || memchr (text, 0, length))
		return false;
	parser_t p = {text, text, text + length, error, error_size};
	editor_map_t result = {0};
	for (;;)
	{
		if (!SkipSpace (&p))
			goto fail;
		if (p.cursor == p.end)
			break;
		if (!Consume (&p, '{'))
			goto fail;
		if (result.num_entities == SIZE_MAX ||
			!Grow ((void **)&result.entities, result.num_entities + 1, sizeof (*result.entities)))
		{
			ParseError (&p, "out of memory");
			goto fail;
		}
		editor_map_entity_t *entity = &result.entities[result.num_entities++];
		memset (entity, 0, sizeof (*entity));
		for (;;)
		{
			if (!SkipSpace (&p) || p.cursor == p.end)
			{
				ParseError (&p, "unterminated entity");
				goto fail;
			}
			if (*p.cursor == '}')
			{
				++p.cursor;
				break;
			}
			if (*p.cursor == '{')
			{
				if (!ParseBrush (&p, entity))
					goto fail;
			}
			else
			{
				char *key = ParseQuoted (&p), *value = key ? ParseQuoted (&p) : NULL;
				if (!key || !value || !*key || entity->num_properties == SIZE_MAX ||
					!Grow ((void **)&entity->properties, entity->num_properties + 1, sizeof (*entity->properties)))
				{
					if (key && value)
						ParseError (&p, *key ? "out of memory" : "empty property key");
					free (key);
					free (value);
					goto fail;
				}
				entity->properties[entity->num_properties++] = (editor_map_property_t){key, value};
			}
		}
	}
	if (p.cursor != p.end || !result.num_entities || !EditorMap_GetProperty (&result.entities[0], "classname") ||
		strcmp (EditorMap_GetProperty (&result.entities[0], "classname"), "worldspawn"))
	{
		ParseError (&p, "first entity must be worldspawn");
		goto fail;
	}
	EditorMap_Free (map);
	*map = result;
	return true;
fail:
	EditorMap_Free (&result);
	return false;
}

typedef struct
{
	char *data;
	size_t length, capacity;
} text_buffer_t;

static bool Append (text_buffer_t *b, const char *text, size_t length)
{
	if (length > SIZE_MAX - b->length - 1)
		return false;
	size_t required = b->length + length + 1;
	if (required > b->capacity)
	{
		size_t capacity = b->capacity ? b->capacity : 256;
		while (capacity < required)
		{
			if (capacity > SIZE_MAX / 2)
			{
				capacity = required;
				break;
			}
			capacity *= 2;
		}
		char *p = realloc (b->data, capacity);
		if (!p)
			return false;
		b->data = p;
		b->capacity = capacity;
	}
	memcpy (b->data + b->length, text, length);
	b->length += length;
	b->data[b->length] = 0;
	return true;
}

static bool AppendText (text_buffer_t *b, const char *text)
{
	return Append (b, text, strlen (text));
}

static bool AppendQuoted (text_buffer_t *b, const char *s)
{
	if (!AppendText (b, "\""))
		return false;
	for (; *s; ++s)
	{
		if ((*s == '"' || *s == '\\') && !AppendText (b, "\\"))
			return false;
		if (*s == '\r' || *s == '\n' || !Append (b, s, 1))
			return false;
	}
	return AppendText (b, "\"");
}

bool EditorMap_Serialize (const editor_map_t *map, char **out, size_t *out_length)
{
	if (!map || !out || !map->num_entities)
		return false;
	text_buffer_t b = {0};
	for (size_t e = 0; e < map->num_entities; ++e)
	{
		const editor_map_entity_t *entity = &map->entities[e];
		if (!AppendText (&b, "{\n"))
			goto fail;
		for (size_t i = 0; i < entity->num_properties; ++i)
			if (!AppendQuoted (&b, entity->properties[i].key) || !AppendText (&b, " ") ||
				!AppendQuoted (&b, entity->properties[i].value) || !AppendText (&b, "\n"))
				goto fail;
		for (size_t i = 0; i < entity->num_brushes; ++i)
		{
			const editor_map_brush_t *brush = &entity->brushes[i];
			if (!AppendText (&b, "{\n"))
				goto fail;
			for (size_t j = 0; j < brush->num_faces; ++j)
			{
				const editor_map_face_t *face = &brush->faces[j];
				for (int k = 0; k < 3; ++k)
				{
					char point[160];
					int n = snprintf (point, sizeof (point), "( %.17g %.17g %.17g ) ", face->points[k][0], face->points[k][1], face->points[k][2]);
					if (n < 0 || (size_t)n >= sizeof (point) || !Append (&b, point, (size_t)n))
						goto fail;
				}
				if (!face->mapping || !AppendText (&b, face->mapping) || !AppendText (&b, "\n"))
					goto fail;
			}
			if (!AppendText (&b, "}\n"))
				goto fail;
		}
		if (!AppendText (&b, "}\n"))
			goto fail;
	}
	*out = b.data;
	if (out_length)
		*out_length = b.length;
	return true;
fail:
	free (b.data);
	return false;
}

bool EditorMap_Clone (editor_map_t *dest, const editor_map_t *src)
{
	char *text;
	size_t length;
	if (!dest || !EditorMap_Serialize (src, &text, &length))
		return false;
	bool ok = EditorMap_Parse (dest, text, length, NULL, 0);
	free (text);
	return ok;
}

static void Cross (const double a[3], const double b[3], double out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static double Dot (const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

typedef struct { double normal[3], dist; } editor_map_plane_t;

static bool BrushPlanes (const editor_map_brush_t *brush, editor_map_plane_t *planes)
{
	for (size_t i = 0; i < brush->num_faces; ++i)
	{
		const editor_map_face_t *face = &brush->faces[i];
		double a[3], b[3];
		for (int k = 0; k < 3; ++k)
		{
			a[k] = face->points[1][k] - face->points[0][k];
			b[k] = face->points[2][k] - face->points[0][k];
		}
		Cross (a, b, planes[i].normal);
		double length = sqrt (Dot (planes[i].normal, planes[i].normal));
		if (!(length > 1e-12) || !isfinite (length))
			return false;
		for (int k = 0; k < 3; ++k)
			planes[i].normal[k] /= length;
		planes[i].dist = Dot (planes[i].normal, face->points[0]);
		if (!isfinite (planes[i].dist))
			return false;
	}
	return true;
}

static bool BrushInside (const editor_map_plane_t *planes, size_t count, const double point[3], double sign)
{
	for (size_t i = 0; i < count; ++i)
	{
		double distance = Dot (planes[i].normal, point) - planes[i].dist;
		double tolerance = 1e-7 * (1.0 + fabs (planes[i].dist) + fabs (point[0]) + fabs (point[1]) + fabs (point[2]));
		if (sign * distance < -tolerance)
			return false;
	}
	return true;
}

/* Detect recession directions, including rays formed by intersecting two
 * planes. Bounds are only useful for editing if the convex solid is closed. */
static bool IsRecessionDirection (const editor_map_plane_t *planes, size_t count, const double dir[3], double sign)
{
	double len = sqrt (Dot (dir, dir));
	if (!(len > 1e-12))
		return false;
	for (size_t i = 0; i < count; ++i)
		if (sign * Dot (planes[i].normal, dir) < -1e-10 * len)
			return false;
	return true;
}

bool EditorMap_BrushBounds (const editor_map_brush_t *brush, double mins[3], double maxs[3])
{
	if (!brush || !mins || !maxs || brush->num_faces < 4 || brush->num_faces > 64 || !brush->faces)
		return false;
	editor_map_plane_t *planes = malloc (brush->num_faces * sizeof (*planes));
	if (!planes)
		return false;
	bool ok = false;
	if (!BrushPlanes (brush, planes))
		goto done;
	for (int s = 0; s < 2; ++s)
	{
		double sign = s == 0 ? 1.0 : -1.0;
		double lo[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, hi[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
		size_t vertices = 0;
		for (size_t i = 0; i < brush->num_faces; ++i)
			for (size_t j = i + 1; j < brush->num_faces; ++j)
				for (size_t k = j + 1; k < brush->num_faces; ++k)
				{
					double a[3], b[3], c[3], point[3];
					Cross (planes[j].normal, planes[k].normal, a);
					double det = Dot (planes[i].normal, a);
					if (fabs (det) < 1e-10)
						continue;
					Cross (planes[k].normal, planes[i].normal, b);
					Cross (planes[i].normal, planes[j].normal, c);
					for (int axis = 0; axis < 3; ++axis)
						point[axis] = (planes[i].dist * a[axis] + planes[j].dist * b[axis] + planes[k].dist * c[axis]) / det;
					if (!isfinite (point[0]) || !isfinite (point[1]) || !isfinite (point[2]) || !BrushInside (planes, brush->num_faces, point, sign))
						continue;
					for (int axis = 0; axis < 3; ++axis)
					{
						lo[axis] = fmin (lo[axis], point[axis]);
						hi[axis] = fmax (hi[axis], point[axis]);
					}
					++vertices;
				}
		if (vertices < 4 || hi[0] - lo[0] <= 1e-8 || hi[1] - lo[1] <= 1e-8 || hi[2] - lo[2] <= 1e-8)
			continue;
		bool unbounded = false;
		for (int axis = 0; axis < 3 && !unbounded; ++axis)
			for (int direction = -1; direction <= 1; direction += 2)
			{
				double ray[3] = {0, 0, 0};
				ray[axis] = direction;
				unbounded |= IsRecessionDirection (planes, brush->num_faces, ray, sign);
			}
		for (size_t i = 0; i < brush->num_faces && !unbounded; ++i)
			for (size_t j = i + 1; j < brush->num_faces && !unbounded; ++j)
			{
				double ray[3];
				Cross (planes[i].normal, planes[j].normal, ray);
				unbounded |= IsRecessionDirection (planes, brush->num_faces, ray, sign);
				for (int axis = 0; axis < 3; ++axis)
					ray[axis] = -ray[axis];
				unbounded |= IsRecessionDirection (planes, brush->num_faces, ray, sign);
			}
		if (!unbounded)
		{
			memcpy (mins, lo, sizeof (lo));
			memcpy (maxs, hi, sizeof (hi));
			ok = true;
			break;
		}
	}
done:
	free (planes);
	return ok;
}

size_t EditorMap_BrushFaceVertices (const editor_map_brush_t *brush, size_t face, double (*vertices)[3], size_t capacity)
{
	if (!brush || !vertices || face >= brush->num_faces || brush->num_faces < 4 || brush->num_faces > 64 || !brush->faces)
		return 0;
	double lo[3], hi[3];
	if (!EditorMap_BrushBounds (brush, lo, hi))
		return 0;
	editor_map_plane_t planes[64];
	if (!BrushPlanes (brush, planes))
		return 0;
	double points[64][3];
	size_t count = 0;
	for (int side = 0; side < 2 && count < 3; ++side)
	{
		count = 0;
		const double sign = side ? -1.0 : 1.0;
		for (size_t j = 0; j < brush->num_faces; ++j)
			for (size_t k = j + 1; k < brush->num_faces; ++k)
			{
				if (j == face || k == face) continue;
				double a[3], b[3], c[3], p[3];
				Cross (planes[j].normal, planes[k].normal, a);
				double det = Dot (planes[face].normal, a);
				if (fabs (det) < 1e-10) continue;
				Cross (planes[k].normal, planes[face].normal, b);
				Cross (planes[face].normal, planes[j].normal, c);
				for (int axis = 0; axis < 3; ++axis)
					p[axis] = (planes[face].dist * a[axis] + planes[j].dist * b[axis] + planes[k].dist * c[axis]) / det;
				if (!isfinite (p[0]) || !isfinite (p[1]) || !isfinite (p[2]) || !BrushInside (planes, brush->num_faces, p, sign)) continue;
				bool duplicate = false;
				for (size_t v = 0; v < count; ++v)
				{
					double d[3];
					for (int axis = 0; axis < 3; ++axis) d[axis] = p[axis] - points[v][axis];
					if (Dot (d, d) < 1e-12) { duplicate = true; break; }
				}
				if (!duplicate)
				{
					if (count == 64) return 0;
					memcpy (points[count++], p, sizeof (p));
				}
			}
	}
	if (count < 3 || count > capacity) return 0;
	double center[3] = {0, 0, 0}, u[3], v[3];
	for (size_t i = 0; i < count; ++i)
		for (int axis = 0; axis < 3; ++axis) center[axis] += points[i][axis] / count;
	for (int axis = 0; axis < 3; ++axis) u[axis] = points[0][axis] - center[axis];
	Cross (planes[face].normal, u, v);
	double angles[64];
	for (size_t i = 0; i < count; ++i)
	{
		double d[3];
		for (int axis = 0; axis < 3; ++axis) d[axis] = points[i][axis] - center[axis];
		angles[i] = atan2 (Dot (d, v), Dot (d, u));
	}
	for (size_t i = 1; i < count; ++i)
	{
		double angle = angles[i], p[3]; memcpy (p, points[i], sizeof (p));
		size_t j = i;
		while (j && angles[j - 1] > angle)
		{
			angles[j] = angles[j - 1]; memcpy (points[j], points[j - 1], sizeof (p)); --j;
		}
		angles[j] = angle; memcpy (points[j], p, sizeof (p));
	}
	memcpy (vertices, points, count * sizeof (points[0]));
	return count;
}

static bool ValidBounds (const double mins[3], const double maxs[3])
{
	if (!mins || !maxs)
		return false;
	for (int axis = 0; axis < 3; ++axis)
		if (!isfinite (mins[axis]) || !isfinite (maxs[axis]) || !(maxs[axis] > mins[axis]))
			return false;
	return true;
}

bool EditorMap_AddBox (editor_map_t *map, size_t entity_index, const double mins[3], const double maxs[3], const char *texture, size_t *out_index)
{
	if (!map || entity_index >= map->num_entities || !ValidBounds (mins, maxs) || !texture || !*texture)
		return false;
	for (const unsigned char *p = (const unsigned char *)texture; *p; ++p)
		if (isspace (*p) || *p == '"' || *p == '{' || *p == '}' || *p == '(' || *p == ')')
			return false;
	const size_t texture_length = strlen (texture);
	if (texture_length > SIZE_MAX - sizeof (" 0 0 0 1 1"))
		return false;
	char *mapping = malloc (texture_length + sizeof (" 0 0 0 1 1"));
	if (!mapping)
		return false;
	memcpy (mapping, texture, texture_length);
	memcpy (mapping + texture_length, " 0 0 0 1 1", sizeof (" 0 0 0 1 1"));
	editor_map_brush_t brush = {0};
	brush.faces = calloc (6, sizeof (*brush.faces));
	if (!brush.faces)
	{
		free (mapping);
		return false;
	}
	brush.num_faces = 6;
	for (int axis = 0; axis < 3; ++axis)
		for (int side = 0; side < 2; ++side)
		{
			editor_map_face_t *face = &brush.faces[2 * axis + side];
			face->mapping = CopyString (mapping, strlen (mapping));
			if (!face->mapping)
				goto fail;
			for (int point = 0; point < 3; ++point)
				memcpy (face->points[point], mins, 3 * sizeof (double));
			const double coordinate = side ? maxs[axis] : mins[axis];
			for (int point = 0; point < 3; ++point)
				face->points[point][axis] = coordinate;
			int u = (axis + 1) % 3, v = (axis + 2) % 3;
			face->points[1][side ? v : u] = maxs[side ? v : u];
			face->points[2][side ? u : v] = maxs[side ? u : v];
		}
	free (mapping);
	editor_map_entity_t *entity = &map->entities[entity_index];
	if (entity->num_brushes == SIZE_MAX || !Grow ((void **)&entity->brushes, entity->num_brushes + 1, sizeof (*entity->brushes)))
		goto fail_without_mapping;
	if (out_index)
		*out_index = entity->num_brushes;
	entity->brushes[entity->num_brushes++] = brush;
	return true;
fail:
	free (mapping);
fail_without_mapping:
	for (size_t i = 0; i < brush.num_faces; ++i)
		free (brush.faces[i].mapping);
	free (brush.faces);
	return false;
}

bool EditorMap_BrushTranslate (editor_map_brush_t *brush, const double delta[3])
{
	if (!brush || !delta)
		return false;
	for (int axis = 0; axis < 3; ++axis)
		if (!isfinite (delta[axis]))
			return false;
	for (size_t face = 0; face < brush->num_faces; ++face)
		for (int point = 0; point < 3; ++point)
			for (int axis = 0; axis < 3; ++axis)
				if (!isfinite (brush->faces[face].points[point][axis] + delta[axis]))
					return false;
	for (size_t face = 0; face < brush->num_faces; ++face)
		for (int point = 0; point < 3; ++point)
			for (int axis = 0; axis < 3; ++axis)
				brush->faces[face].points[point][axis] += delta[axis];
	return true;
}

bool EditorMap_BrushResizeBox (editor_map_brush_t *brush, const double mins[3], const double maxs[3])
{
	if (!brush || brush->num_faces != 6 || !ValidBounds (mins, maxs))
		return false;
	double old_min[3], old_max[3];
	editor_map_plane_t planes[6];
	if (!EditorMap_BrushBounds (brush, old_min, old_max) || !BrushPlanes (brush, planes))
		return false;
	int face_axes[6], face_sides[6], seen = 0;
	for (int face = 0; face < 6; ++face)
	{
		int axis = 0;
		for (int i = 1; i < 3; ++i)
			if (fabs (planes[face].normal[i]) > fabs (planes[face].normal[axis]))
				axis = i;
		if (fabs (fabs (planes[face].normal[axis]) - 1.0) > 1e-10)
			return false;
		double coordinate = brush->faces[face].points[0][axis];
		double tolerance = 1e-8 * (1.0 + fabs (coordinate));
		int side = fabs (coordinate - old_min[axis]) <= tolerance ? 0 :
			fabs (coordinate - old_max[axis]) <= tolerance ? 1 : -1;
		if (side < 0 || (seen & (1 << (2 * axis + side))))
			return false;
		seen |= 1 << (2 * axis + side);
		face_axes[face] = axis;
		face_sides[face] = side;
	}
	for (int face = 0; face < 6; ++face)
		for (int point = 0; point < 3; ++point)
			brush->faces[face].points[point][face_axes[face]] = face_sides[face] ? maxs[face_axes[face]] : mins[face_axes[face]];
	return true;
}
