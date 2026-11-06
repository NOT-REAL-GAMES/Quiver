/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bsp_meshlet.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	const bsp_meshlet_face_t *face;
	uint32_t				  morton;
	float					  mins[3], maxs[3];
} ordered_face_t;

static void IncludePoint (float mins[3], float maxs[3], const float point[3])
{
	for (int axis = 0; axis < 3; ++axis)
	{
		if (point[axis] < mins[axis])
			mins[axis] = point[axis];
		if (point[axis] > maxs[axis])
			maxs[axis] = point[axis];
	}
}

static const float *VertexXYZ (const bsp_meshlet_face_t *face, uint32_t index)
{
	return (const float *)((const unsigned char *)face->vertices + index * face->vertex_stride);
}

/* Interleave ten bits per axis to sort faces spatially within a material. */
static uint32_t SpreadBits (uint32_t x)
{
	x = (x | (x << 16)) & 0x030000ffu;
	x = (x | (x << 8)) & 0x0300f00fu;
	x = (x | (x << 4)) & 0x030c30c3u;
	return (x | (x << 2)) & 0x09249249u;
}

static int CompareFaces (const void *lhs, const void *rhs)
{
	const ordered_face_t *a = lhs, *b = rhs;
	if (a->face->material != b->face->material)
		return a->face->material < b->face->material ? -1 : 1;
	if (a->morton != b->morton)
		return a->morton < b->morton ? -1 : 1;
	if (a->face->surface != b->face->surface)
		return a->face->surface < b->face->surface ? -1 : 1;
	return a->face->first_vertex < b->face->first_vertex ? -1 : a->face->first_vertex > b->face->first_vertex;
}

// Faces of one material can occur in entirely different rooms. A loose
// cluster box spanning both rooms cannot be hidden by either room's walls.
// A large original face may exceed the limit on its own.
static bool FitsClusterBounds (const bsp_meshlet_t *m, const ordered_face_t *face)
{
	for (int axis = 0; axis < 3; ++axis)
		if (fmaxf (m->maxs[axis], face->maxs[axis]) - fminf (m->mins[axis], face->mins[axis]) > 256.0f)
			return false;
	return true;
}

/* First count, then emit into exactly sized arrays. Keeping a whole face in
 * one run avoids needless per-frame visibility work. Only oversized fans
 * split: each continuation reuses the fan origin and previous rim vertex. */
static void Pack (const ordered_face_t *order, size_t count, bsp_meshlet_bake_t *bake, bool emit)
{
	uint32_t	  meshes = 0, runs = 0, indices = 0;
	bsp_meshlet_t scratch, *m = NULL;
	for (size_t i = 0; i < count; ++i)
	{
		const bsp_meshlet_face_t *face = order[i].face;
		for (uint32_t k = 2; k < face->num_vertices;)
		{
			uint32_t tris = face->num_vertices - k;
			if (tris > BSP_MESHLET_MAX_VERTICES - 2)
				tris = BSP_MESHLET_MAX_VERTICES - 2;
			if (tris > BSP_MESHLET_MAX_TRIANGLES)
				tris = BSP_MESHLET_MAX_TRIANGLES;
			const uint32_t verts = tris + 2;

			if (!m || face->material != m->material || verts + m->num_vertices > BSP_MESHLET_MAX_VERTICES ||
				tris + m->index_count / 3 > BSP_MESHLET_MAX_TRIANGLES || !FitsClusterBounds (m, &order[i]))
			{
				m = emit ? &bake->meshlets[meshes] : &scratch;
				*m = (bsp_meshlet_t){.material = face->material,
									 .first_index = indices,
									 .first_run = runs,
									 .mins = {FLT_MAX, FLT_MAX, FLT_MAX},
									 .maxs = {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
				++meshes;
			}
			if (verts == face->num_vertices)
			{
				IncludePoint (m->mins, m->maxs, order[i].mins);
				IncludePoint (m->mins, m->maxs, order[i].maxs);
			}
			else
			{
				IncludePoint (m->mins, m->maxs, VertexXYZ (face, 0));
				for (uint32_t v = k - 1; v < k + tris; ++v)
					IncludePoint (m->mins, m->maxs, VertexXYZ (face, v));
			}
			if (emit)
			{
				bake->runs[runs] = (bsp_meshlet_run_t){face->surface, indices, tris * 3};
				uint32_t *dst = bake->indices + indices;
				for (uint32_t v = k; v < k + tris; ++v)
				{
					*dst++ = face->first_vertex;
					*dst++ = face->first_vertex + v - 1;
					*dst++ = face->first_vertex + v;
				}
			}
			++runs;
			++m->num_runs;
			m->num_vertices += verts;
			m->index_count += tris * 3;
			indices += tris * 3;
			k += tris;
		}
	}
	bake->num_meshlets = meshes;
	bake->num_indices = indices;
	bake->num_runs = runs;
}

static void *ArrayAlloc (size_t count, size_t element)
{
	return count <= SIZE_MAX / element ? malloc (count * element) : NULL;
}

bool BSPMeshlet_Build (const bsp_meshlet_face_t *faces, size_t count, bsp_meshlet_bake_t *out)
{
	if (!out)
		return false;
	memset (out, 0, sizeof (*out));
	if (!count)
		return true;
	if (!faces || count > UINT32_MAX / 3)
		return false;
	ordered_face_t *order = ArrayAlloc (count, sizeof (*order));
	if (!order)
		return false;

	float	 world_mins[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
	float	 world_maxs[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	uint32_t index_count = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const bsp_meshlet_face_t *f = &faces[i];
		if (!f->vertices || f->num_vertices < 3 || f->vertex_stride < 3 * sizeof (float) || f->vertex_stride % sizeof (float) ||
			f->vertex_stride > (SIZE_MAX - 3 * sizeof (float)) / (f->num_vertices - 1) || f->first_vertex > UINT32_MAX - (f->num_vertices - 1) ||
			f->num_vertices - 2 > (UINT32_MAX - index_count) / 3)
			goto fail;
		index_count += (f->num_vertices - 2) * 3;
		order[i].face = f;
		memcpy (order[i].mins, f->vertices, 3 * sizeof (float));
		memcpy (order[i].maxs, f->vertices, 3 * sizeof (float));
		for (uint32_t v = 0; v < f->num_vertices; ++v)
		{
			const float *p = VertexXYZ (f, v);
			if (!isfinite (p[0]) || !isfinite (p[1]) || !isfinite (p[2]))
				goto fail;
			IncludePoint (order[i].mins, order[i].maxs, p);
		}
		IncludePoint (world_mins, world_maxs, order[i].mins);
		IncludePoint (world_mins, world_maxs, order[i].maxs);
	}
	for (size_t i = 0; i < count; ++i)
	{
		uint32_t key = 0;
		for (int axis = 0; axis < 3; ++axis)
		{
			/* Double intermediates also handle negative, flat and very large BSP bounds. */
			double	 span = (double)world_maxs[axis] - world_mins[axis];
			double	 center = ((double)order[i].mins[axis] + order[i].maxs[axis]) * 0.5;
			uint32_t x = span ? (uint32_t)(1023.0 * ((center - world_mins[axis]) / span)) : 0;
			key |= SpreadBits (x) << axis;
		}
		order[i].morton = key;
	}
	qsort (order, count, sizeof (*order), CompareFaces);
	Pack (order, count, out, false);
	out->meshlets = ArrayAlloc (out->num_meshlets, sizeof (*out->meshlets));
	out->runs = ArrayAlloc (out->num_runs, sizeof (*out->runs));
	out->indices = ArrayAlloc (out->num_indices, sizeof (*out->indices));
	if (!out->meshlets || !out->runs || !out->indices)
		goto fail;
	Pack (order, count, out, true);
	free (order);
	return true;

fail:
	free (order);
	BSPMeshlet_Free (out);
	return false;
}

void BSPMeshlet_Free (bsp_meshlet_bake_t *bake)
{
	free (bake->indices);
	free (bake->runs);
	free (bake->meshlets);
	memset (bake, 0, sizeof (*bake));
}




bool BSPMeshlet_BuildMeshData (const bsp_meshlet_bake_t *bake, bsp_meshlet_mesh_data_t *out)
{
	if (!out)
		return false;
	memset (out, 0, sizeof (*out));
	if (!bake || (!bake->meshlets && bake->num_meshlets) || (!bake->indices && bake->num_indices) || (!bake->runs && bake->num_runs))
		return false;
	if (!bake->num_meshlets)
		return bake->num_indices == 0 && bake->num_runs == 0;
	if (bake->num_indices % 3 || bake->num_meshlets > SIZE_MAX / 12)
		return false;
	size_t vertex_count = 0;
	uint32_t next_index = 0, next_run = 0;
	for (uint32_t i = 0; i < bake->num_meshlets; ++i)
	{
		const bsp_meshlet_t *m = &bake->meshlets[i];
		if (m->first_index != next_index || m->first_run != next_run || !m->num_vertices || m->num_vertices > BSP_MESHLET_MAX_VERTICES || !m->index_count || m->index_count % 3 ||
			m->index_count / 3 > BSP_MESHLET_MAX_TRIANGLES || m->first_index > bake->num_indices ||
			m->index_count > bake->num_indices - m->first_index || m->first_run > bake->num_runs ||
			m->num_runs > bake->num_runs - m->first_run)
			return false;
		if (vertex_count > UINT32_MAX - m->num_vertices)
			return false;
		vertex_count += m->num_vertices;
		next_index += m->index_count;
		next_run += m->num_runs;
	}
	if (next_index != bake->num_indices || next_run != bake->num_runs)
		return false;
	const size_t header_words = (size_t)bake->num_meshlets * 12;
	const size_t triangle_words = (size_t)(bake->num_indices / 3) * 2;
	if (header_words > UINT32_MAX || vertex_count > UINT32_MAX - header_words || triangle_words > UINT32_MAX - header_words - vertex_count)
		return false;
	const size_t words = header_words + vertex_count + triangle_words;
	if (words > SIZE_MAX / sizeof (uint32_t))
		return false;
	uint32_t *data = calloc (words, sizeof (*data));
	if (!data)
		return false;
	uint32_t vertex_cursor = (uint32_t)header_words;
	uint32_t triangle_cursor = (uint32_t)(header_words + vertex_count);
	for (uint32_t i = 0; i < bake->num_meshlets; ++i)
	{
		const bsp_meshlet_t *m = &bake->meshlets[i];
		uint32_t *h = data + i * 12;
		memcpy (h, m->mins, 3 * sizeof (float));
		h[3] = m->material;
		memcpy (h + 4, m->maxs, 3 * sizeof (float));
		h[8] = vertex_cursor;
		h[9] = m->num_vertices;
		h[10] = triangle_cursor;
		h[11] = m->index_count / 3;
		uint32_t unique = 0, expected_index = m->first_index;
		for (uint32_t j = 0; j < m->num_runs; ++j)
		{
			const bsp_meshlet_run_t *run = &bake->runs[m->first_run + j];
			if (run->first_index != expected_index || run->index_count % 3 || run->index_count > m->first_index + m->index_count - expected_index)
				goto fail;
			for (uint32_t k = 0; k < run->index_count; k += 3)
			{
				uint32_t packed = 0;
				for (uint32_t c = 0; c < 3; ++c)
				{
					uint32_t index = bake->indices[run->first_index + k + c];
					uint32_t local = 0;
					while (local < unique && data[vertex_cursor + local] != index)
						++local;
					if (local == unique)
					{
						if (unique >= m->num_vertices)
							goto fail;
						data[vertex_cursor + unique++] = index;
					}
					packed |= local << (c * 6);
				}
				data[triangle_cursor++] = packed;
				data[triangle_cursor++] = run->surface;
			}
			expected_index += run->index_count;
		}
		if (unique != m->num_vertices || expected_index != m->first_index + m->index_count)
			goto fail;
		vertex_cursor += unique;
	}
	out->words = data;
	out->num_words = words;
	return true;
fail:
	free (data);
	return false;
}

void BSPMeshlet_FreeMeshData (bsp_meshlet_mesh_data_t *data)
{
	if (data)
	{
		free (data->words);
		memset (data, 0, sizeof (*data));
	}
}
