/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bsp_meshlet.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Check (int condition, const char *message)
{
	if (!condition)
	{
		fprintf (stderr, "meshlet: %s\n", message);
		exit (1);
	}
}

enum
{
	FACES = 85,
	MAX_POLY = 180,
	STRIDE = 7
};
static float positions[FACES][MAX_POLY][STRIDE]; /* identical stride to world VBO */

static void CheckGeometry (const bsp_meshlet_bake_t *b, const bsp_meshlet_face_t faces[FACES])
{
	unsigned char seen[FACES][MAX_POLY] = {{0}};
	uint32_t	  next_index = 0, next_run = 0;
	for (uint32_t i = 0; i < b->num_meshlets; ++i)
	{
		const bsp_meshlet_t *m = &b->meshlets[i];
		Check (!i || m->material >= b->meshlets[i - 1].material, "material batches are contiguous");
		Check (m->first_index == next_index && m->first_run == next_run, "meshlet ranges partition output");
		Check (m->index_count && m->index_count <= BSP_MESHLET_MAX_TRIANGLES * 3 && m->num_runs, "nonempty, bounded meshlet");
		uint32_t unique_vertices[BSP_MESHLET_MAX_VERTICES], num_unique = 0;
		float	 low[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, high[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
		for (uint32_t j = 0; j < m->num_runs; ++j)
		{
			const bsp_meshlet_run_t *r = &b->runs[next_run++];
			Check (r->surface < FACES, "source surface id");
			const bsp_meshlet_face_t *face = &faces[r->surface];
			Check (m->material == face->material, "no incompatible materials/lightmaps");
			Check (r->first_index == next_index && r->index_count && !(r->index_count % 3), "contiguous source runs");
			for (uint32_t k = 0; k < r->index_count; k += 3)
			{
				const uint32_t *triangle = b->indices + next_index + k;
				uint32_t		rim = triangle[2] - face->first_vertex;
				Check (triangle[0] == face->first_vertex && rim >= 2 && rim < face->num_vertices && triangle[1] == triangle[2] - 1, "matches oriented BSP fan");
				Check (!seen[r->surface][rim]++, "no triangle submitted twice");
				for (int corner = 0; corner < 3; ++corner)
				{
					uint32_t v;
					for (v = 0; v < num_unique && unique_vertices[v] != triangle[corner]; ++v)
						;
					if (v == num_unique)
					{
						Check (num_unique < BSP_MESHLET_MAX_VERTICES, "unique vertex limit");
						unique_vertices[num_unique++] = triangle[corner];
					}
					const float *p = positions[r->surface][triangle[corner] - face->first_vertex];
					for (int axis = 0; axis < 3; ++axis)
					{
						if (p[axis] < low[axis])
							low[axis] = p[axis];
						if (p[axis] > high[axis])
							high[axis] = p[axis];
					}
				}
			}
			next_index += r->index_count;
		}
		Check (next_index == m->first_index + m->index_count, "run lengths equal meshlet range");
		Check (num_unique == m->num_vertices, "exact unique vertex count");
		for (int axis = 0; axis < 3; ++axis)
			{
			Check (m->mins[axis] == low[axis] && m->maxs[axis] == high[axis], "tight bounds include actual triangle vertices");
			Check (m->num_runs == 1 || m->maxs[axis] - m->mins[axis] <= 256.0f, "distant rooms cannot share a cluster");
		}
	}
	Check (b->num_indices == next_index && b->num_runs == next_run, "no trailing unused indexes or runs");
	for (size_t f = 0; f < FACES; ++f)
		for (uint32_t k = 2; k < faces[f].num_vertices; ++k)
			Check (seen[f][k] == 1, "no dropped triangles");
}

int main (void)
{
	bsp_meshlet_face_t faces[FACES], reversed[FACES];
	uint32_t		   vertex = 0;
	for (uint32_t f = 0; f < FACES; ++f)
	{
		uint32_t n = f == FACES - 1 ? MAX_POLY : 3 + f % 5;
		faces[f] = (bsp_meshlet_face_t){.surface = f,
										.material = f % 3,
										.first_vertex = vertex,
										.num_vertices = n,
										.vertices = &positions[f][0][0],
										.vertex_stride = sizeof (positions[f][0])};
		/* Alternating disjoint areas exercises spatial sorting. Constant Z
		 * covers maps with a flat quantization axis. */
		for (uint32_t k = 0; k < n; ++k)
		{
			positions[f][k][0] = (float)((int)(f % 2) * 8000 - 4000 + (int)(f % 7) * 8 + (int)k);
			positions[f][k][1] = (float)(k * k + f);
			positions[f][k][2] = -32.0f;
			positions[f][k][3] = -1e7f; /* not an XYZ coordinate */
		}
		vertex += n;
		reversed[FACES - f - 1] = faces[f];
	}

	bsp_meshlet_bake_t a, b;
	Check (BSPMeshlet_Build (faces, FACES, &a), "bake static world fixture");
	CheckGeometry (&a, faces);
	bsp_meshlet_mesh_data_t mesh;
	Check (BSPMeshlet_BuildMeshData (&a, &mesh), "build mesh shader streams");
	for (uint32_t i = 0; i < a.num_meshlets; ++i)
	{
		const bsp_meshlet_t *m = &a.meshlets[i];
		const uint32_t *header = mesh.words + i * 12;
		Check (header[3] == m->material && header[9] == m->num_vertices && header[11] == m->index_count / 3, "mesh stream header");
		uint32_t triangle = 0;
		for (uint32_t j = 0; j < m->num_runs; ++j)
		{
			const bsp_meshlet_run_t *run = &a.runs[m->first_run + j];
			for (uint32_t k = 0; k < run->index_count; k += 3, ++triangle)
			{
				uint32_t packed = mesh.words[header[10] + triangle * 2];
				Check (mesh.words[header[10] + triangle * 2 + 1] == run->surface, "mesh triangle source surface");
				for (uint32_t c = 0; c < 3; ++c)
				{
					uint32_t local = (packed >> (c * 6)) & 63u;
					Check (local < header[9] && mesh.words[header[8] + local] == a.indices[run->first_index + k + c], "mesh triangle matches BSP winding");
				}
			}
		}
	}
	BSPMeshlet_FreeMeshData (&mesh);
	bsp_meshlet_t saved = a.meshlets[0];
	a.meshlets[0].first_index = 1;
	Check (!BSPMeshlet_BuildMeshData (&a, &mesh) && !mesh.words, "reject noncontiguous mesh input");
	a.meshlets[0] = saved;
	a.meshlets[0].num_vertices = BSP_MESHLET_MAX_VERTICES + 1;
	Check (!BSPMeshlet_BuildMeshData (&a, &mesh) && !mesh.words, "reject oversized mesh input");
	a.meshlets[0] = saved;
	Check (a.num_meshlets < FACES && a.num_runs > FACES, "merges small faces and splits the oversized fan");
	Check (BSPMeshlet_Build (reversed, FACES, &b), "bake reordered input");
	Check (a.num_meshlets == b.num_meshlets && a.num_runs == b.num_runs && a.num_indices == b.num_indices, "deterministic counts");
	Check (
		!memcmp (a.meshlets, b.meshlets, a.num_meshlets * sizeof (*a.meshlets)) && !memcmp (a.indices, b.indices, a.num_indices * sizeof (*a.indices)) &&
			!memcmp (a.runs, b.runs, a.num_runs * sizeof (*a.runs)),
		"deterministic geometry and order");
	BSPMeshlet_Free (&a);
	BSPMeshlet_Free (&b);
	Check (BSPMeshlet_Build (NULL, 0, &a) && !a.meshlets && !a.runs && !a.indices, "empty world");
	BSPMeshlet_Free (&a);

	bsp_meshlet_face_t bad = faces[0];
	bad.first_vertex = UINT32_MAX - 1;
	Check (!BSPMeshlet_Build (&bad, 1, &a), "reject VBO index overflow");
	bad = faces[0];
	bad.vertex_stride = SIZE_MAX;
	Check (!BSPMeshlet_Build (&bad, 1, &a), "reject stride overflow");
	positions[0][0][1] = NAN;
	Check (!BSPMeshlet_Build (faces, FACES, &a) && !a.indices && !a.runs && !a.meshlets, "invalid bounds fall back without partial output");
	BSPMeshlet_Free (&a);
	puts ("BSP meshlet baker checks passed");
	return 0;
}
