/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_EDITOR_MAP_H
#define QUIVER_EDITOR_MAP_H

#include <stdbool.h>
#include <stddef.h>

/* Point positions use floats in the editor and engine. */
#define EDITOR_MAP_POINT_COORD_LIMIT 16777216.0

/* Plane points and complete texture mapping tail of one Quake MAP face. */
typedef struct
{
	double points[3][3];
	char *mapping;
} editor_map_face_t;

typedef struct
{
	editor_map_face_t *faces;
	size_t num_faces;
} editor_map_brush_t;

typedef struct
{
	char *key, *value;
} editor_map_property_t;

typedef struct
{
	editor_map_property_t *properties;
	size_t num_properties;
	editor_map_brush_t *brushes;
	size_t num_brushes;
} editor_map_entity_t;

typedef struct
{
	editor_map_entity_t *entities;
	size_t num_entities;
} editor_map_t;

/* Initialize outputs to zero before first use. Parse and clone leave the
 * destination unchanged on failure. Serialize allocates a NUL-terminated
 * buffer owned by the caller; out_length excludes the terminator. */
void EditorMap_Free (editor_map_t *map);
bool EditorMap_Parse (editor_map_t *map, const char *text, size_t length, char *error, size_t error_size);
bool EditorMap_Clone (editor_map_t *dest, const editor_map_t *src);
bool EditorMap_Serialize (const editor_map_t *map, char **out, size_t *out_length);
bool EditorMap_Init (editor_map_t *map);
bool EditorMap_AddEntity (editor_map_t *map, const char *classname, size_t *out_index);
const char *EditorMap_GetProperty (const editor_map_entity_t *entity, const char *key);
bool EditorMap_SetProperty (editor_map_entity_t *entity, const char *key, const char *value);
bool EditorMap_RemoveEntity (editor_map_t *map, size_t index);
/* Point entities have no brushes; the worldspawn is never a point entity.
 * Missing origins use Quake's default (0,0,0). Duplication preserves all
 * properties, including target names, and offsets only the origin. */
bool EditorMap_GetPointOrigin (const editor_map_t *map, size_t index, double origin[3]);
bool EditorMap_AddLight (editor_map_t *map, const double origin[3], size_t *out_index);
/* Color edits use normalized RGB [0,1] and write `_color`. Read accepts both
 * normalized and legacy byte RGB (any channel >1 selects /255), default white.
 * Explicit edits replace the legacy `color` alias with canonical `_color`.
 * Invalid authored colors are displayed as white; reading never edits source. */
bool EditorMap_GetLightColor (const editor_map_t *map, size_t index, double rgb[3]);
bool EditorMap_SetLightColor (editor_map_t *map, size_t index, const double rgb[3]);
bool EditorMap_SetPointOrigin (editor_map_t *map, size_t index, const double origin[3]);
bool EditorMap_DuplicatePoint (editor_map_t *map, size_t index, const double offset[3], size_t *out_index);
#define EDITOR_MAP_MODEL_PATH_MAX 256
typedef struct
{
	char model[EDITOR_MAP_MODEL_PATH_MAX];
	double origin[3], angles[3], modelscale;
	int skin;
	bool collision;
} editor_map_model_t;
/* Static model entities remain ordinary source point entities. These helpers
 * validate document values only; compile validates asset existence/static poses. */
bool EditorMap_ValidModelPath (const char *path);
bool EditorMap_IsModel (const editor_map_t *map, size_t index);
bool EditorMap_GetModel (const editor_map_t *map, size_t index, editor_map_model_t *model);
bool EditorMap_AddModel (editor_map_t *map, const char *path, const double origin[3], size_t *out_index);
bool EditorMap_SetModel (editor_map_t *map, size_t index, const editor_map_model_t *model);
/* Renderer convention: Rz(yaw) Ry(-pitch) Rx(roll), then uniform scale and
 * origin. Bounds are asset-local; no asset loading occurs in this helper. */
bool EditorMap_ModelCorners (const editor_map_model_t *model, const double mins[3], const double maxs[3], double corners[8][3]);
bool EditorMap_AddBox (editor_map_t *map, size_t entity, const double mins[3], const double maxs[3], const char *texture, size_t *out_index);
bool EditorMap_RemoveBrush (editor_map_entity_t *entity, size_t index);
bool EditorMap_BrushBounds (const editor_map_brush_t *brush, double mins[3], double maxs[3]);
/* Reconstruct a clipped convex face from its plane and the other brush planes.
 * Returns zero for invalid geometry or insufficient capacity. */
size_t EditorMap_BrushFaceVertices (const editor_map_brush_t *brush, size_t face, double (*vertices)[3], size_t capacity);
bool EditorMap_BrushTranslate (editor_map_brush_t *brush, const double delta[3]);
/* Resize only a six-sided axis-aligned box; all face mappings survive. */
bool EditorMap_BrushResizeBox (editor_map_brush_t *brush, const double mins[3], const double maxs[3]);

#endif
