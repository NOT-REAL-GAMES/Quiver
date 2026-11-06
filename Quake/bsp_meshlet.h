/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BSP_MESHLET_H
#define BSP_MESHLET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BSP_MESHLET_MAX_VERTICES  64u
#define BSP_MESHLET_MAX_TRIANGLES 126u

/* Input faces have disjoint vertex ranges in the existing brush VBO; XYZ
 * occupies the first three floats of each vertex. The caller supplies only
 * eligible static world faces, with identical material IDs for compatible
 * draw state (including texture/animation, lightmap and surface pass). */
typedef struct
{
	uint32_t	 surface, material, first_vertex, num_vertices;
	const float *vertices;
	size_t		 vertex_stride; /* bytes */
} bsp_meshlet_face_t;

typedef struct
{
	uint32_t material, first_index, index_count, first_run, num_runs, num_vertices;
	float	 mins[3], maxs[3];
} bsp_meshlet_t;

/* Source surface membership and exact index interval. Faces spanning a
 * meshlet boundary have one run in each meshlet. */
typedef struct
{
	uint32_t surface, first_index, index_count;
} bsp_meshlet_run_t;

typedef struct
{
	bsp_meshlet_t	  *meshlets;
	uint32_t		   num_meshlets;
	uint32_t		  *indices;
	uint32_t		   num_indices;
	bsp_meshlet_run_t *runs;
	uint32_t		   num_runs;
} bsp_meshlet_bake_t;

/* On success 'out' owns its arrays; no pointers to input data are retained.
 * Pass an empty/uninitialized output (free any previous bake first). Failure
 * returns false with an empty output, so callers may fall back to BSP. */
bool BSPMeshlet_Build (const bsp_meshlet_face_t *faces, size_t count, bsp_meshlet_bake_t *out);
void BSPMeshlet_Free (bsp_meshlet_bake_t *bake);

/* Mesh-shader stream. Each 12-word header stores bounds, material and
 * absolute word offsets/counts for its unique VBO indices and triangles.
 * Each triangle uses two words: packed three 6-bit local indices, then its
 * original surface ID for the facing test. */
typedef struct
{
	uint32_t *words;
	size_t num_words;
} bsp_meshlet_mesh_data_t;
bool BSPMeshlet_BuildMeshData (const bsp_meshlet_bake_t *bake, bsp_meshlet_mesh_data_t *out);
void BSPMeshlet_FreeMeshData (bsp_meshlet_mesh_data_t *data);

#endif
