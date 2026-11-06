/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_EDITOR_SECTOR_H
#define QUIVER_EDITOR_SECTOR_H

#include "editor_map.h"
#include <stdint.h>

/* A sector is a closed, oriented polyhedral region. Face loops wind outward;
 * portals pair geometrically identical faces with opposite winding. Faces may
 * have any orientation, so sectors may be stacked or have sloped surfaces.
 * Subdivide a face into coplanar polygons for an opening smaller than it. */
#define EDITOR_SECTOR_NO_PORTAL SIZE_MAX
#define EDITOR_SECTOR_MATERIAL_MAX 64
/* The viewport stores positions in floats; this preserves unit precision.
 * Embedded QBSP uses a bounded winding range; leave room for walls and hulls. */
#define EDITOR_SECTOR_COORD_LIMIT 16777216.0
#define EDITOR_SECTOR_QBSP_COORD_LIMIT 1047552.0
typedef struct { double position[3]; } editor_sector_vertex_t;
/* Room sectors bound empty world space; prop sectors describe solid objects. */
typedef enum
{
	EDITOR_SECTOR_WORLD = 0,
	EDITOR_SECTOR_PROP_STATIC = 1,
	EDITOR_SECTOR_PROP_DYNAMIC = 2,
	EDITOR_SECTOR_PROP_MOVABLE = 3
} editor_sector_prop_type_t;
typedef struct
{
	size_t *vertices;
	size_t num_vertices;
	char material[EDITOR_SECTOR_MATERIAL_MAX];
	size_t portal_sector, portal_face;
	/* Nonzero identifiers associate fragments of one authored face, scoped to
	 * the owning sector. Zero denotes a standalone face. */
	size_t source_group;
} editor_sector_face_t;
typedef struct
{
	editor_sector_face_t *faces;
	size_t num_faces;
	editor_sector_prop_type_t prop_type;
	/* Inverted so zero-initialized and legacy static props remain solid. */
	bool prop_no_collision;
	double prop_velocity[3];
} editor_sector_t;
typedef struct
{
	editor_sector_vertex_t *vertices;
	size_t num_vertices;
	editor_sector_t *sectors;
	size_t num_sectors;
	/* Worldspawn properties and entities, preserved in the source document. */
	editor_map_t entities;
} editor_sector_map_t;

/* All destinations must initially be zeroed. Parse/clone/compile leave the
 * destination untouched on failure. Serialize allocates a NUL-terminated
 * result owned by the caller, with length excluding its terminator. */
bool EditorSector_Init (editor_sector_map_t *map);
void EditorSector_Free (editor_sector_map_t *map);
bool EditorSector_Clone (editor_sector_map_t *dest, const editor_sector_map_t *src);
bool EditorSector_Parse (editor_sector_map_t *map, const char *text, size_t length, char *error, size_t error_size);
/* Sector sources start with the exact ASCII magic "QSECTOR 1\n". */
bool EditorSector_IsFormat (const char *text, size_t length);
bool EditorSector_Serialize (const editor_sector_map_t *map, char **out, size_t *length);
bool EditorSector_Validate (const editor_sector_map_t *map, char *error, size_t error_size);
/* Classify a point for light placement. A successful query reports whether
 * at least one world room exists and whether the point is strictly inside a
 * room and outside (including the boundary of) every solid prop. Invalid
 * documents, nonfinite/out-of-range points, and allocation failures return
 * false and report an error. */
bool EditorSector_QueryPointInOpenWorld (const editor_sector_map_t *map, const double point[3],
	bool *is_open, bool *has_world, char *error, size_t error_size);
/* Batch version: validates and builds sector shells once; up to 1024 points. */
bool EditorSector_QueryPointsInOpenWorld (const editor_sector_map_t *map, const double (*points)[3],
	size_t count, bool *is_open, bool *has_world, char *error, size_t error_size);
/* Triangles contain global vertex indices for a specified source face. Pass
 * NULL with capacity zero to query the count; invalid faces or insufficient
 * output capacity return zero. */
size_t EditorSector_TriangulateFace (const editor_sector_map_t *map, size_t sector, size_t face,
	size_t (*triangles)[3], size_t capacity);

/* Add a closed box sector, sharing existing exact-position vertices. */
bool EditorSector_AddBox (editor_sector_map_t *map, const double mins[3], const double maxs[3],
	const char *material, size_t *out_sector, char *error, size_t error_size);
bool EditorSector_AddBoxOfType (editor_sector_map_t *map, const double mins[3], const double maxs[3],
	const char *material, editor_sector_prop_type_t type, size_t *out_sector, char *error, size_t error_size);
bool EditorSector_SetPropType (editor_sector_map_t *map, size_t sector, editor_sector_prop_type_t type,
	char *error, size_t error_size);
bool EditorSector_SetPropCollisionEnabled (editor_sector_map_t *map, size_t sector, bool enabled,
	char *error, size_t error_size);
bool EditorSector_SetPropVelocity (editor_sector_map_t *map, size_t sector, const double velocity[3],
	char *error, size_t error_size);
/* Duplicate a prop as an independent sector with an offset and all its face
 * materials and entity settings. World rooms and linked portals are excluded. */
bool EditorSector_DuplicateProp (editor_sector_map_t *map, size_t sector, const double offset[3],
	size_t *out_sector, char *error, size_t error_size);
/* Vertex handles in different props (or a prop and a room) remain independent,
 * even when they occupy the same position. */
bool EditorSector_VerticesEditTogether (const editor_sector_map_t *map, size_t sector_a, size_t vertex_a,
	size_t sector_b, size_t vertex_b);
bool EditorSector_Remove (editor_sector_map_t *map, size_t sector, char *error, size_t error_size);
/* Linking is symmetric and requires identical, oppositely wound face loops. */
bool EditorSector_LinkPortal (editor_sector_map_t *map, size_t sector, size_t face, size_t other_sector, size_t other_face,
	char *error, size_t error_size);
bool EditorSector_UnlinkPortal (editor_sector_map_t *map, size_t sector, size_t face, char *error, size_t error_size);
/* These operations affect coincident vertices and reject invalid resulting solids.
 * Moving a vertex triangulates incident faces, including both sides of any
 * portal, when needed to preserve planarity; face indices may then change. */
bool EditorSector_SetVertex (editor_sector_map_t *map, size_t vertex, const double position[3], char *error, size_t error_size);
bool EditorSector_MergeVertices (editor_sector_map_t *map, size_t source, size_t target, char *error, size_t error_size);
/* Cleanup-only variant: merge source and linked portal aliases, without
 * changing the topology of unlinked sectors at the same position. */
bool EditorSector_MergePortalVertices (editor_sector_map_t *map, size_t source, size_t target, char *error, size_t error_size);
/* Split one planar face along a segment joining two boundary points. Any
 * affected shell edges and the opposite side of a portal are split too.
 * The two resulting faces are independent authored faces. */
bool EditorSector_CutFace (editor_sector_map_t *map, size_t sector, size_t face, const double start[3], const double end[3], char *error, size_t error_size);
bool EditorSector_MoveFace (editor_sector_map_t *map, size_t sector, size_t face, const double delta[3], char *error, size_t error_size);
bool EditorSector_Translate (editor_sector_map_t *map, size_t sector, const double delta[3], char *error, size_t error_size);

/* Lower sector boundary surfaces to ordinary MAP brushes. Source sectors are
 * not flattened on save. On success, out owns a complete Quake MAP document. */
bool EditorSector_Compile (const editor_sector_map_t *map, editor_map_t *out, char *error, size_t error_size);

#endif
