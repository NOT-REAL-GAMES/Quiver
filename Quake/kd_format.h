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
// kd_format.h -- Kd-tree map file format definitions (.qkd)

#ifndef KD_FORMAT_H
#define KD_FORMAT_H

#include "q_stdinc.h"

//=============================================================================
// File Format Constants
//=============================================================================

#define KDFORMAT_MAGIC   (('K' << 0) | ('D' << 8) | ('M' << 16) | ('P' << 24))
#define KDFORMAT_VERSION 1

//=============================================================================
// Lump IDs
//=============================================================================

typedef enum
{
	KD_LUMP_ENTITIES,   // Entity definitions (same format as BSP)
	KD_LUMP_TEXTURES,   // Texture data
	KD_LUMP_PLANES,     // Plane equations for surfaces
	KD_LUMP_VERTICES,   // Vertex positions
	KD_LUMP_TEXCOORDS,  // Texture coordinates
	KD_LUMP_KDNODES,    // Kd-tree internal nodes
	KD_LUMP_KDLEAVES,   // Kd-tree leaf nodes
	KD_LUMP_SURFACES,   // Surface/face definitions
	KD_LUMP_SURFEDGES,  // Surface edge indices
	KD_LUMP_VISIBILITY, // Zone visibility data
	KD_LUMP_LIGHTMAPS,  // Lightmap atlas data
	KD_LUMP_BRUSHES,    // Source brushes (for editor/collision)
	KD_LUMP_BRUSHSIDES, // Brush side planes
	KD_LUMP_ZONES,      // Visibility zones
	KD_LUMP_MODELS,     // Submodels (doors, platforms, etc.)
	KD_LUMP_NUM
} kd_lump_id_t;

//=============================================================================
// File Header
//=============================================================================

typedef struct
{
	int32_t offset;
	int32_t length;
} dkdlump_t;

typedef struct
{
	uint32_t   magic;   // KDFORMAT_MAGIC
	uint32_t   version; // KDFORMAT_VERSION
	dkdlump_t  lumps[KD_LUMP_NUM];
} dkdheader_t;

//=============================================================================
// Node and Leaf Structures
//=============================================================================

// Axis identifiers for kd-tree splits
#define KD_AXIS_X    0
#define KD_AXIS_Y    1
#define KD_AXIS_Z    2
#define KD_AXIS_LEAF 255

// Kd-tree internal node (on-disk format)
typedef struct
{
	uint8_t axis;        // Split axis (KD_AXIS_X/Y/Z) or KD_AXIS_LEAF
	uint8_t pad[3];      // Alignment padding
	float   split_dist;  // Split plane distance along axis
	int32_t children[2]; // Child indices: [0]=front (>split), [1]=back (<=split)
	                     // Negative value = ~index into leaf array
	float   mins[3];     // Bounding box minimum
	float   maxs[3];     // Bounding box maximum
} dkdnode_t;

// Kd-tree leaf node (on-disk format)
typedef struct
{
	int32_t contents;      // Content flags (CONTENTS_EMPTY, CONTENTS_SOLID, etc.)
	int32_t zone;          // Visibility zone index
	int32_t first_surface; // First surface index in this leaf
	int32_t num_surfaces;  // Number of surfaces in this leaf
	float   mins[3];       // Bounding box minimum
	float   maxs[3];       // Bounding box maximum
} dkdleaf_t;

//=============================================================================
// Surface Structures
//=============================================================================

// Surface flags
#define KD_SURF_PLANEBACK  (1 << 0) // Surface faces opposite of plane normal
#define KD_SURF_SKY        (1 << 1) // Sky surface
#define KD_SURF_TURBULENT  (1 << 2) // Water/slime/lava
#define KD_SURF_NODRAW     (1 << 3) // Don't render (trigger brushes, etc.)
#define KD_SURF_NOLIGHT    (1 << 4) // Don't lightmap
#define KD_SURF_ALPHATEST  (1 << 5) // Alpha-tested transparency

// Surface definition (on-disk format)
typedef struct
{
	int32_t  plane_num;     // Plane index
	uint32_t flags;         // KD_SURF_* flags
	int32_t  texture_num;   // Texture index
	int32_t  first_vertex;  // First vertex index
	int32_t  num_vertices;  // Number of vertices (forms a convex polygon)
	int32_t  lightmap_s;    // Lightmap atlas S coordinate
	int32_t  lightmap_t;    // Lightmap atlas T coordinate
	int32_t  lightmap_w;    // Lightmap width
	int32_t  lightmap_h;    // Lightmap height
	float    texinfo_vecs[2][4]; // Texture coordinate transform vectors
} dkdsurface_t;

//=============================================================================
// Geometry Structures
//=============================================================================

// Vertex (on-disk format)
typedef struct
{
	float position[3];
} dkdvertex_t;

// Texture coordinates (on-disk format, parallel array to vertices)
typedef struct
{
	float s, t;       // Texture coordinates
	float ls, lt;     // Lightmap coordinates
} dkdtexcoord_t;

// Plane equation (on-disk format)
typedef struct
{
	float normal[3];
	float dist;
	int32_t type; // PLANE_X/Y/Z/ANYX/ANYY/ANYZ for fast tests
} dkdplane_t;

//=============================================================================
// Brush Structures (for editor and precise collision)
//=============================================================================

// Brush definition (on-disk format)
typedef struct
{
	int32_t first_side;  // First brush side index
	int32_t num_sides;   // Number of sides
	int32_t contents;    // Content flags
} dkdbrush_t;

// Brush side (on-disk format)
typedef struct
{
	int32_t plane_num;   // Plane index (pointing outward)
	int32_t texture_num; // Texture index
	float   texinfo_vecs[2][4]; // Texture coordinate vectors
} dkdbrushside_t;

//=============================================================================
// Visibility Structures
//=============================================================================

// Visibility zone (on-disk format)
typedef struct
{
	float   mins[3];     // Zone bounding box minimum
	float   maxs[3];     // Zone bounding box maximum
	int32_t vis_offset;  // Offset into visibility data
	int32_t vis_size;    // Size of compressed visibility data
	int32_t first_leaf;  // First leaf in this zone
	int32_t num_leaves;  // Number of leaves in this zone
} dkdzone_t;

//=============================================================================
// Submodel Structure (doors, platforms, etc.)
//=============================================================================

typedef struct
{
	float   mins[3];       // Bounding box minimum
	float   maxs[3];       // Bounding box maximum
	float   origin[3];     // Origin offset
	int32_t headnode;      // Root node of submodel's kd-tree
	int32_t first_surface; // First surface
	int32_t num_surfaces;  // Number of surfaces
} dkdmodel_t;

//=============================================================================
// Texture Structure
//=============================================================================

#define KD_TEXTURE_NAME_LEN 64

typedef struct
{
	char    name[KD_TEXTURE_NAME_LEN]; // Texture name
	int32_t width;                     // Texture width
	int32_t height;                    // Texture height
	int32_t flags;                     // Texture flags
} dkdtexture_t;

#endif // KD_FORMAT_H
