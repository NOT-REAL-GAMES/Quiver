/*
Copyright (C) 2024 Quiver Contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// kd_model.h -- In-memory kd-tree map structures

#ifndef KD_MODEL_H
#define KD_MODEL_H

#include "quakedef.h"
#include "kd_format.h"

// Forward declarations
struct qmodel_s;
struct texture_s;
struct gltexture_s;

//=============================================================================
// In-Memory Node Structures
//=============================================================================

typedef struct mkdnode_s
{
	uint8_t axis;             // Split axis (KD_AXIS_X/Y/Z) or KD_AXIS_LEAF
	float   split_dist;       // Split plane distance
	int32_t children[2];      // Child indices: [0]=front, [1]=back
	                          // negative = leaf index (~leaf_idx)
	vec3_t  mins;             // Bounding box minimum
	vec3_t  maxs;             // Bounding box maximum
} mkdnode_t;

typedef struct mkdleaf_s
{
	int     contents;         // Content flags (CONTENTS_EMPTY, CONTENTS_SOLID, etc.)
	int     zone;             // Visibility zone index
	int     first_surface;    // First surface index in this leaf
	int     num_surfaces;     // Number of surfaces in this leaf
	vec3_t  mins;             // Bounding box minimum
	vec3_t  maxs;             // Bounding box maximum
	int     visframe;         // For visibility culling
	int     *surfaces;        // Array of surface indices in this leaf
} mkdleaf_t;

//=============================================================================
// In-Memory Surface Structures
//=============================================================================

typedef struct mkdsurface_s
{
	mplane_t          *plane;          // Surface plane
	uint32_t           flags;          // KD_SURF_* flags
	struct texture_s  *texture;        // Texture pointer
	struct gltexture_s *gltexture;     // GPU texture for rendering

	// Vertex data
	int                first_vertex;   // First vertex in VBO
	int                num_vertices;   // Number of vertices (convex polygon)

	// Texture coordinates
	float              texinfo_vecs[2][4]; // Texture coordinate transform

	// Lightmap info
	int                light_s, light_t;   // Lightmap atlas coordinates
	int                light_w, light_h;   // Lightmap dimensions
	int                lightmap_num;       // Lightmap texture index
	byte              *samples;            // Lightmap sample data

	// Rendering state
	int                visframe;           // For visibility culling
	struct mkdsurface_s *texturechain;     // For texture chain rendering
	int                vbo_firstvert;      // Index into VBO
	int                indirect_idx;       // Indirect draw index
} mkdsurface_t;

//=============================================================================
// In-Memory Visibility Structures
//=============================================================================

typedef struct mkdzone_s
{
	vec3_t  mins;             // Zone bounding box minimum
	vec3_t  maxs;             // Zone bounding box maximum
	byte   *vis;              // Decompressed visibility data for this zone
	int     vis_size;         // Size of visibility data
	int     first_leaf;       // First leaf in this zone
	int     num_leaves;       // Number of leaves in this zone
} mkdzone_t;

//=============================================================================
// In-Memory Brush Structures (for editor and collision)
//=============================================================================

typedef struct mkdbrushside_s
{
	mplane_t          *plane;         // Plane (pointing outward)
	struct texture_s  *texture;       // Texture
	float              texinfo_vecs[2][4]; // Texture coordinate vectors
} mkdbrushside_t;

typedef struct mkdbrush_s
{
	int                num_sides;     // Number of sides
	mkdbrushside_t    *sides;         // Array of sides
	int                contents;      // Content flags
	vec3_t             mins;          // Computed bounding box
	vec3_t             maxs;
} mkdbrush_t;

//=============================================================================
// In-Memory Submodel Structure
//=============================================================================

typedef struct mkdsubmodel_s
{
	vec3_t     mins;          // Bounding box minimum
	vec3_t     maxs;          // Bounding box maximum
	vec3_t     origin;        // Origin offset
	mkdnode_t *headnode;      // Root node of submodel's kd-tree
	int        first_surface; // First surface
	int        num_surfaces;  // Number of surfaces
} mkdsubmodel_t;

//=============================================================================
// Main Kd-Tree Model Structure
//=============================================================================

typedef struct kdmodel_s
{
	// Identification
	char            name[MAX_QPATH];
	int             version;

	// Kd-tree structure
	mkdnode_t      *nodes;
	int             num_nodes;
	mkdleaf_t      *leaves;
	int             num_leaves;
	mkdnode_t      *headnode;     // Root of the tree

	// Geometry
	mplane_t       *planes;
	int             num_planes;
	vec3_t         *vertices;
	int             num_vertices;

	// Surfaces
	mkdsurface_t   *surfaces;
	int             num_surfaces;
	int            *surfedges;    // Surface edge indices
	int             num_surfedges;

	// Textures
	struct texture_s **textures;
	int             num_textures;

	// Visibility
	mkdzone_t      *zones;
	int             num_zones;
	byte           *visdata;      // Raw visibility data
	int             visdata_size;

	// Lighting
	byte           *lightdata;
	int             lightdata_size;

	// Entities
	char           *entities;

	// Brushes (for editor/collision)
	mkdbrush_t     *brushes;
	int             num_brushes;

	// Submodels
	mkdsubmodel_t  *submodels;
	int             num_submodels;

	// Bounds
	vec3_t          mins;
	vec3_t          maxs;

	// GPU data
	VkBuffer        vertex_buffer;
	struct glheapallocation_s *vertex_allocation;
	VkBuffer        index_buffer;
	struct glheapallocation_s *index_allocation;

} kdmodel_t;

//=============================================================================
// Function Declarations
//=============================================================================

// Loading
kdmodel_t *KD_LoadModel (const char *name);
void       KD_FreeModel (kdmodel_t *model);

// Tree traversal
mkdleaf_t *KD_PointInLeaf (kdmodel_t *model, vec3_t point);
int        KD_BoxLeaves (kdmodel_t *model, vec3_t mins, vec3_t maxs, mkdleaf_t **list, int listsize);

// Visibility
byte      *KD_LeafPVS (kdmodel_t *model, mkdleaf_t *leaf);
qboolean   KD_LeafVisible (kdmodel_t *model, mkdleaf_t *from, mkdleaf_t *to);

// Collision (kd_world.c)
int        KD_PointContents (kdmodel_t *model, vec3_t point);
qboolean   KD_TraceLine (kdmodel_t *model, vec3_t start, vec3_t end, trace_t *trace);
qboolean   KD_TraceBox (kdmodel_t *model, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace);
qboolean   KD_ClipMoveToWorld (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace);
void       KD_WorldInit (void);

// Rendering
void       KD_BuildVertexBuffer (kdmodel_t *model);
void       KD_MarkSurfaces (kdmodel_t *model, mkdleaf_t *viewleaf);

// Initialization
void       KD_Init (void);

// Rendering (kd_render.c)
void       KD_RenderInit (void);
void       KD_DrawDebugSurfaces (cb_context_t *cbx);
kdmodel_t *KD_GetDebugModel (void);

// Compiler (kd_compile.c)
void       KD_CompileInit (void);

#endif // KD_MODEL_H
