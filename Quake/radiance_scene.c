#include "radiance_scene.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert (sizeof (rc_scene_triangle_t) == 96, "radiance triangle must match std430");
_Static_assert (sizeof (rc_scene_node_t) == 48, "radiance BVH node must match std430");

void RCScene_Init (rc_scene_t *scene) { memset (scene, 0, sizeof (*scene)); }
void RCScene_Clear (rc_scene_t *scene)
{
	free (scene->triangles); free (scene->indices); free (scene->nodes);
	memset (scene, 0, sizeof (*scene));
}

bool RCScene_AppendTriangle (rc_scene_t *scene, const rc_scene_triangle_t *triangle)
{
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			if (!isfinite (triangle->position[i][j])) return false;
	if (scene->triangle_count == UINT32_MAX) return false;
	if (scene->triangle_count == scene->triangle_capacity)
	{
		uint32_t capacity = scene->triangle_capacity ? scene->triangle_capacity : 256;
		if (scene->triangle_capacity)
			capacity = capacity > UINT32_MAX / 2 ? UINT32_MAX : capacity * 2;
		if ((uint64_t)capacity * sizeof (*scene->triangles) > SIZE_MAX) return false;
		void *allocation = realloc (scene->triangles, (size_t)capacity * sizeof (*scene->triangles));
		if (!allocation) return false;
		scene->triangles = allocation; scene->triangle_capacity = capacity;
	}
	free (scene->nodes); free (scene->indices);
	scene->nodes = NULL; scene->indices = NULL; scene->node_count = scene->index_count = 0;
	scene->triangles[scene->triangle_count++] = *triangle;
	return true;
}

static float RCScene_Centroid (const rc_scene_t *scene, uint32_t item, unsigned axis)
{
	const rc_scene_triangle_t *t = &scene->triangles[item];
	return t->position[0][axis] / 3.0f + t->position[1][axis] / 3.0f + t->position[2][axis] / 3.0f;
}

/* In-place median selection: deterministic, bounded recursion depth even for
 * identical centroids, and no global comparator state shared by map workers. */
static void RCScene_Select (rc_scene_t *scene, uint32_t begin, uint32_t end, uint32_t middle, unsigned axis)
{
	while (end - begin > 1)
	{
		float pivot = RCScene_Centroid (scene, scene->indices[begin + (end - begin) / 2], axis);
		uint32_t low = begin, i = begin, high = end;
		while (i < high)
		{
			float value = RCScene_Centroid (scene, scene->indices[i], axis);
			if (value < pivot)
			{
				uint32_t tmp = scene->indices[low]; scene->indices[low++] = scene->indices[i]; scene->indices[i++] = tmp;
			}
			else if (value > pivot)
			{
				uint32_t tmp = scene->indices[--high]; scene->indices[high] = scene->indices[i]; scene->indices[i] = tmp;
			}
			else ++i;
		}
		if (middle < low) end = low;
		else if (middle >= high) begin = high;
		else return;
	}
}

static uint32_t RCScene_BuildNode (rc_scene_t *scene, uint32_t begin, uint32_t end)
{
	uint32_t id = scene->node_count++;
	rc_scene_node_t *node = &scene->nodes[id];
	float cmin[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, cmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	for (int a = 0; a < 3; ++a) { node->mins[a] = FLT_MAX; node->maxs[a] = -FLT_MAX; }
	for (uint32_t i = begin; i < end; ++i)
	{
		const rc_scene_triangle_t *t = &scene->triangles[scene->indices[i]];
		for (int a = 0; a < 3; ++a)
		{
			for (int v = 0; v < 3; ++v)
			{
				float p = t->position[v][a];
				if (p < node->mins[a]) node->mins[a] = p;
				if (p > node->maxs[a]) node->maxs[a] = p;
			}
			float c = RCScene_Centroid (scene, scene->indices[i], a);
			if (c < cmin[a]) cmin[a] = c;
			if (c > cmax[a]) cmax[a] = c;
		}
	}
	if (end - begin <= 4)
	{
		node->metadata[0] = begin; node->metadata[1] = end - begin;
	}
	else
	{
		unsigned axis = 0;
		for (unsigned a = 1; a < 3; ++a)
			if ((double)cmax[a] - cmin[a] > (double)cmax[axis] - cmin[axis]) axis = a;
		uint32_t middle = begin + (end - begin) / 2;
		RCScene_Select (scene, begin, end, middle, axis);
		RCScene_BuildNode (scene, begin, middle);
		RCScene_BuildNode (scene, middle, end);
	}
	node->metadata[2] = scene->node_count; /* next node after this subtree */
	return id;
}

bool RCScene_Build (rc_scene_t *scene)
{
	free (scene->nodes); free (scene->indices);
	scene->nodes = NULL; scene->indices = NULL; scene->node_count = scene->index_count = 0;
	uint32_t count = scene->triangle_count;
	if (!count) return true;
	if (count > UINT32_MAX / 2 || (uint64_t)count * 2 * sizeof (*scene->nodes) > SIZE_MAX ||
		(uint64_t)count * sizeof (*scene->indices) > SIZE_MAX) return false;
	scene->nodes = calloc ((size_t)count * 2, sizeof (*scene->nodes));
	scene->indices = malloc ((size_t)count * sizeof (*scene->indices));
	if (!scene->nodes || !scene->indices)
	{
		free (scene->nodes); free (scene->indices); scene->nodes = NULL; scene->indices = NULL; return false;
	}
	for (uint32_t i = 0; i < count; ++i) scene->indices[i] = i;
	scene->index_count = count;
	RCScene_BuildNode (scene, 0, count);
	return true;
}

static bool RCScene_Composable (const rc_scene_t *scene)
{
	if (!scene) return false;
	if (!scene->triangle_count) return !scene->node_count && !scene->index_count;
	return scene->triangles && scene->indices && scene->nodes && scene->node_count &&
		scene->index_count == scene->triangle_count && scene->nodes[0].metadata[2] == scene->node_count;
}

bool RCScene_Compose (rc_scene_t *scene, const rc_scene_t *first, const rc_scene_t *second)
{
	if (!scene || !RCScene_Composable (first) || !RCScene_Composable (second) ||
		second->triangle_count > UINT32_MAX - first->triangle_count) return false;
	bool join = first->triangle_count && second->triangle_count;
	uint64_t nodes = (uint64_t)first->node_count + second->node_count + join;
	uint32_t triangles = first->triangle_count + second->triangle_count;
	if (nodes > UINT32_MAX || nodes * sizeof (rc_scene_node_t) > SIZE_MAX ||
		(uint64_t)triangles * sizeof (rc_scene_triangle_t) > SIZE_MAX ||
		(uint64_t)triangles * sizeof (uint32_t) > SIZE_MAX) return false;
	rc_scene_t pending = {0};
	if (triangles)
	{
		pending.triangles = malloc ((size_t)triangles * sizeof (*pending.triangles));
		pending.indices = malloc ((size_t)triangles * sizeof (*pending.indices));
		pending.nodes = malloc ((size_t)nodes * sizeof (*pending.nodes));
		if (!pending.triangles || !pending.indices || !pending.nodes) { RCScene_Clear (&pending); return false; }
	}
	pending.triangle_count = pending.triangle_capacity = pending.index_count = triangles;
	pending.node_count = (uint32_t)nodes;
	uint32_t triangle_offset = 0, node_offset = join ? 1 : 0;
	const rc_scene_t *parts[2] = {first, second};
	for (unsigned part = 0; part < 2; ++part)
	{
		const rc_scene_t *source = parts[part];
		if (!source->triangle_count) continue;
		memcpy (pending.triangles + triangle_offset, source->triangles,
			(size_t)source->triangle_count * sizeof (*pending.triangles));
		for (uint32_t i = 0; i < source->index_count; ++i)
			pending.indices[triangle_offset+i] = source->indices[i] + triangle_offset;
		for (uint32_t i = 0; i < source->node_count; ++i)
		{
			rc_scene_node_t node = source->nodes[i];
			if (node.metadata[1]) node.metadata[0] += triangle_offset;
			node.metadata[2] += node_offset;
			pending.nodes[node_offset+i] = node;
		}
		triangle_offset += source->triangle_count;
		node_offset += source->node_count;
	}
	if (join)
	{
		rc_scene_node_t *root = &pending.nodes[0];
		memset (root, 0, sizeof (*root)); root->metadata[2] = pending.node_count;
		for (unsigned axis = 0; axis < 3; ++axis)
		{
			root->mins[axis] = fminf (first->nodes[0].mins[axis], second->nodes[0].mins[axis]);
			root->maxs[axis] = fmaxf (first->nodes[0].maxs[axis], second->nodes[0].maxs[axis]);
		}
	}
	RCScene_Clear (scene); *scene = pending;
	return true;
}

bool RCScene_ReplaceSuffix (rc_scene_t *scene, const rc_scene_t *first, const rc_scene_t *second)
{
	if (!scene || scene == first || scene == second || !RCScene_Composable (scene) ||
		!RCScene_Composable (first) || !RCScene_Composable (second)) return false;
	bool join = first->triangle_count && second->triangle_count;
	if ((uint64_t)first->triangle_count + second->triangle_count != scene->triangle_count ||
		(uint64_t)first->node_count + second->node_count + join != scene->node_count) return false;
	/* All compatibility checks precede the first write. No allocation or
	 * fallible operation follows, preserving transactional caller behavior. */
	uint32_t triangle_offset = first->triangle_count, node_offset = first->node_count + join;
	if (second->triangle_count)
	{
		memcpy (scene->triangles + triangle_offset, second->triangles, (size_t)second->triangle_count * sizeof (*scene->triangles));
		for (uint32_t i = 0; i < second->index_count; ++i) scene->indices[triangle_offset+i] = second->indices[i] + triangle_offset;
		for (uint32_t i = 0; i < second->node_count; ++i)
		{
			rc_scene_node_t node = second->nodes[i];
			if (node.metadata[1]) node.metadata[0] += triangle_offset;
			node.metadata[2] += node_offset; scene->nodes[node_offset+i] = node;
		}
	}
	if (join)
		for (unsigned axis = 0; axis < 3; ++axis)
		{
			scene->nodes[0].mins[axis] = fminf (first->nodes[0].mins[axis], second->nodes[0].mins[axis]);
			scene->nodes[0].maxs[axis] = fmaxf (first->nodes[0].maxs[axis], second->nodes[0].maxs[axis]);
		}
	return true;
}

bool RCScene_Refit (rc_scene_t *scene)
{
	if (!scene) return false;
	if (!scene->triangle_count) return scene->node_count == 0 && scene->index_count == 0;
	if (!scene->nodes || !scene->indices || scene->index_count != scene->triangle_count || !scene->node_count) return false;
	/* Validate the entire topology before touching bounds so a failed refit
	 * cannot leave a partially updated BVH. */
	for (uint32_t i = 0; i < scene->node_count; ++i)
	{
		rc_scene_node_t *node = &scene->nodes[i];
		uint32_t escape = node->metadata[2], first = node->metadata[0], count = node->metadata[1];
		if (escape <= i || escape > scene->node_count) return false;
		if (count)
		{
			if (first >= scene->index_count || count > scene->index_count - first) return false;
			for (uint32_t j = 0; j < count; ++j)
			{
				uint32_t index = scene->indices[first+j];
				if (index >= scene->triangle_count) return false;
				for (int v = 0; v < 3; ++v)
					for (int a = 0; a < 3; ++a)
						if (!isfinite (scene->triangles[index].position[v][a])) return false;
			}
		}
		else
		{
			uint32_t left = i + 1;
			if (left >= escape) return false;
			uint32_t right = scene->nodes[left].metadata[2];
			if (right <= left || right >= escape || scene->nodes[right].metadata[2] != escape) return false;
		}
	}
	for (uint32_t i = scene->node_count; i-- > 0;)
	{
		rc_scene_node_t *node = &scene->nodes[i];
		if (node->metadata[1])
		{
			for (int a = 0; a < 3; ++a) { node->mins[a] = FLT_MAX; node->maxs[a] = -FLT_MAX; }
			for (uint32_t j = 0; j < node->metadata[1]; ++j)
			{
				const rc_scene_triangle_t *t = &scene->triangles[scene->indices[node->metadata[0]+j]];
				for (int v = 0; v < 3; ++v)
					for (int a = 0; a < 3; ++a)
					{
						if (t->position[v][a] < node->mins[a]) node->mins[a] = t->position[v][a];
						if (t->position[v][a] > node->maxs[a]) node->maxs[a] = t->position[v][a];
					}
			}
		}
		else
		{
			const rc_scene_node_t *left = &scene->nodes[i+1], *right = &scene->nodes[left->metadata[2]];
			for (int a = 0; a < 3; ++a)
			{
				node->mins[a] = fminf (left->mins[a], right->mins[a]);
				node->maxs[a] = fmaxf (left->maxs[a], right->maxs[a]);
			}
		}
	}
	return true;
}

bool RCScene_AnyGeometryAABB (const rc_scene_t *scene, const float mins[3], const float maxs[3])
{
	if (!scene || !scene->nodes || !mins || !maxs) return false;
	for (int a = 0; a < 3; ++a)
		if (!isfinite (mins[a]) || !isfinite (maxs[a]) || mins[a] > maxs[a]) return false;
	for (uint32_t i = 0; i < scene->node_count;)
	{
		const rc_scene_node_t *node = &scene->nodes[i];
		bool overlap = true;
		for (int a = 0; a < 3; ++a)
			if (node->maxs[a] < mins[a] || node->mins[a] > maxs[a]) { overlap = false; break; }
		if (!overlap)
		{
			if (node->metadata[2] <= i) return false; /* malformed escape */
			i = node->metadata[2];
			continue;
		}
		if (node->metadata[1]) return true;
		++i;
	}
	return false;
}

static bool RCScene_BoxHit (const rc_scene_node_t *n, const float o[3], const float d[3], float tmin, float tmax)
{
	for (int a = 0; a < 3; ++a)
	{
		if (d[a] == 0.0f)
		{
			if (o[a] < n->mins[a] || o[a] > n->maxs[a]) return false;
			continue;
		}
		float x = (n->mins[a] - o[a]) / d[a], y = (n->maxs[a] - o[a]) / d[a];
		if (x > y) { float swap = x; x = y; y = swap; }
		if (x > tmin) tmin = x;
		if (y < tmax) tmax = y;
		if (tmin > tmax) return false;
	}
	return true;
}

static void cross3 (const float a[3], const float b[3], float c[3])
{
	c[0] = a[1] * b[2] - a[2] * b[1]; c[1] = a[2] * b[0] - a[0] * b[2]; c[2] = a[0] * b[1] - a[1] * b[0];
}
static float dot3 (const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static bool RCScene_TriangleHit (const rc_scene_triangle_t *tri, const float o[3], const float d[3], float tmin, float tmax, rc_scene_hit_t *hit)
{
	float e1[3], e2[3], p[3], s[3], q[3], n[3];
	for (int a = 0; a < 3; ++a) { e1[a] = tri->position[1][a]-tri->position[0][a]; e2[a] = tri->position[2][a]-tri->position[0][a]; s[a] = o[a]-tri->position[0][a]; }
	cross3 (d, e2, p); float det = dot3 (e1, p);
	if (fabsf (det) < 1e-8f) return false;
	float inv = 1.0f / det, u = dot3 (s, p) * inv;
	if (u < 0 || u > 1) return false;
	cross3 (s, e1, q); float v = dot3 (d, q) * inv;
	if (v < 0 || u + v > 1) return false;
	float t = dot3 (e2, q) * inv;
	if (!(t >= tmin && t <= tmax)) return false;
	cross3 (e1, e2, n); float length = sqrtf (dot3 (n, n));
	if (!(length > 0)) return false;
	hit->distance = t; hit->barycentric[0] = 1-u-v; hit->barycentric[1] = u; hit->barycentric[2] = v;
	for (int a = 0; a < 3; ++a) { hit->position[a] = o[a] + t*d[a]; hit->normal[a] = n[a]/length; }
	for (int a = 0; a < 2; ++a) hit->uv[a] = hit->barycentric[0]*tri->uv01[a] + u*tri->uv01[a+2] + v*tri->uv2[a];
	hit->material=tri->metadata[0]; hit->flags=tri->metadata[1]; hit->surface=tri->metadata[2]; hit->instance=tri->metadata[3];
	return true;
}

bool RCScene_TraceReference (const rc_scene_t *scene, const float origin[3], const float direction[3],
	float tmin, float tmax, rc_scene_accept_fn accept, void *user, rc_scene_hit_t *hit)
{
	bool found = false;
	if (!scene->nodes || !scene->indices || !hit || !(tmax >= tmin)) return false;
	for (int a = 0; a < 3; ++a) if (!isfinite (origin[a]) || !isfinite (direction[a])) return false;
	for (uint32_t i = 0; i < scene->node_count;)
	{
		const rc_scene_node_t *node = &scene->nodes[i];
		if (!RCScene_BoxHit (node, origin, direction, tmin, tmax)) { i = node->metadata[2]; continue; }
		if (node->metadata[1])
		{
			for (uint32_t j = 0; j < node->metadata[1]; ++j)
			{
				uint32_t index = scene->indices[node->metadata[0] + j]; rc_scene_hit_t candidate;
				if (RCScene_TriangleHit (&scene->triangles[index], origin, direction, tmin, tmax, &candidate))
				{
					candidate.triangle = index;
					if (!accept || accept (&candidate, user)) { *hit = candidate; tmax = candidate.distance; found = true; }
				}
			}
		}
		++i;
	}
	return found;
}
