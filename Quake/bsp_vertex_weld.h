/* Load-time BSP vertex welding. The original BSP topology and indices stay
 * intact; only positions of close vertices in the same group are unified. */
#ifndef BSP_VERTEX_WELD_H
#define BSP_VERTEX_WELD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Large cell coordinates cannot be represented accurately enough for an
 * integer grid. At this threshold distinct float coordinates are necessarily
 * farther apart than epsilon, so use their exact bit pattern for that axis. */
static inline int64_t BSPVertexWeldAxis (float value, float epsilon, bool *exact)
{
	double cell = epsilon > 0.0f ? (double)value / (2.0 * (double)epsilon) : 0.0;
	if (epsilon > 0.0f && cell > -1099511627776.0 && cell < 1099511627776.0)
	{
		*exact = false;
		return (int64_t)floor (cell);
	}
	uint32_t bits;
	if (value == 0.0f) value = 0.0f; /* +0 and -0 represent the same point. */
	memcpy (&bits, &value, sizeof bits);
	*exact = true;
	return INT64_MIN + (int64_t)bits;
}

static inline size_t BSPVertexWeldHash (const int64_t key[3], uint32_t group, size_t mask)
{
	uint64_t h = UINT64_C (14695981039346656037);
	for (int i = 0; i < 3; ++i)
	{
		uint64_t x = (uint64_t)key[i];
		x ^= x >> 30; x *= UINT64_C (0xbf58476d1ce4e5b9);
		x ^= x >> 27; x *= UINT64_C (0x94d049bb133111eb);
		x ^= x >> 31;
		h = (h ^ x) * UINT64_C (1099511628211);
	}
	h = (h ^ group) * UINT64_C (1099511628211);
	return (size_t)h & mask;
}

typedef struct bsp_vertex_weld_entry_s
{
	int64_t key[3];
	size_t next; /* index + 1; zero terminates a chain */
	uint32_t group;
} bsp_vertex_weld_entry_t;

/* Finds the nearest input representative in the same group within epsilon's
 * Euclidean radius, breaking ties by earliest input index. Only representatives
 * enter the hash: this prevents a
 * chain of individually close vertices from dragging distant points together.
 * On failure input and welded_count remain untouched. Nonfinite vertices and
 * the UINT32_MAX group are excluded. Passing NULL groups gives one group. */
static inline bool BSPVertexWeld (float (*vertices)[3], size_t count, float epsilon,
	const uint32_t *groups, size_t *welded_count)
{
	if (!isfinite (epsilon) || epsilon < 0.0f || (count && !vertices))
		return false;
	if (!count)
	{
		if (welded_count) *welded_count = 0;
		return true;
	}
	if (count > SIZE_MAX / sizeof (*vertices) || count > SIZE_MAX / sizeof (bsp_vertex_weld_entry_t) || count > SIZE_MAX / 2)
		return false;
	size_t capacity = 2;
	while (capacity < count * 2)
	{
		if (capacity > SIZE_MAX / 2) return false;
		capacity *= 2;
	}
	if (capacity > SIZE_MAX / sizeof (size_t)) return false;
	float (*result)[3] = malloc (count * sizeof (*result));
	bsp_vertex_weld_entry_t *entries = malloc (count * sizeof (*entries));
	size_t *buckets = calloc (capacity, sizeof (*buckets));
	if (!result || !entries || !buckets)
	{
		free (result); free (entries); free (buckets);
		return false;
	}
	memcpy (result, vertices, count * sizeof (*result));
	size_t welded = 0;
	const double eps2 = (double)epsilon * (double)epsilon;
	for (size_t i = 0; i < count; ++i)
	{
		uint32_t group = groups ? groups[i] : 0;
		if (group == UINT32_MAX || !isfinite (vertices[i][0]) || !isfinite (vertices[i][1]) || !isfinite (vertices[i][2]))
			continue;
		int64_t key[3]; bool exact[3];
		for (int a = 0; a < 3; ++a) key[a] = BSPVertexWeldAxis (vertices[i][a], epsilon, &exact[a]);
		size_t best = SIZE_MAX;
		double best_dist2 = eps2;
		for (int x = exact[0] ? 0 : -1; x <= (exact[0] ? 0 : 1); ++x)
		for (int y = exact[1] ? 0 : -1; y <= (exact[1] ? 0 : 1); ++y)
		for (int z = exact[2] ? 0 : -1; z <= (exact[2] ? 0 : 1); ++z)
		{
			const int64_t neighbor[3] = {key[0] + x, key[1] + y, key[2] + z};
			for (size_t node = buckets[BSPVertexWeldHash (neighbor, group, capacity - 1)]; node; node = entries[node - 1].next)
			{
				size_t j = node - 1;
				if (entries[j].group != group || entries[j].key[0] != neighbor[0] || entries[j].key[1] != neighbor[1] || entries[j].key[2] != neighbor[2]) continue;
				double dx = (double)vertices[i][0] - (double)vertices[j][0];
				double dy = (double)vertices[i][1] - (double)vertices[j][1];
				double dz = (double)vertices[i][2] - (double)vertices[j][2];
				double dist2 = dx * dx + dy * dy + dz * dz;
				if (dist2 <= eps2 && (best == SIZE_MAX || dist2 < best_dist2 || (dist2 == best_dist2 && j < best)))
				{ best = j; best_dist2 = dist2; }
			}
		}
		if (best != SIZE_MAX)
		{
			memcpy (result[i], vertices[best], sizeof result[i]);
			++welded;
		}
		else
		{
			memcpy (entries[i].key, key, sizeof key);
			entries[i].group = group;
			size_t bucket = BSPVertexWeldHash (key, group, capacity - 1);
			entries[i].next = buckets[bucket];
			buckets[bucket] = i + 1;
		}
	}
	memcpy (vertices, result, count * sizeof (*result));
	free (result); free (entries); free (buckets);
	if (welded_count) *welded_count = welded;
	return true;
}
#endif
