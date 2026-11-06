#include "radiance_sparse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct rc_sparse_level_s
{
	uint32_t brick_side, table_side, table_offset, table_count;
	uint32_t capacity, slot_offset, record_offset, records_per_brick;
	uint32_t active_base, active_count, overflow;
} rc_sparse_level_t;

typedef struct rc_sparse_request_s
{
	uint32_t index, priority;
	double distance_squared;
} rc_sparse_request_t;

typedef struct rc_sparse_refresh_s
{
	uint32_t progress, generation;
	uint64_t last_priority;
	bool pending, again, priority, cleanup;
} rc_sparse_refresh_t;

struct rc_sparse_s
{
	rc_layout_t layout;
	rc_sparse_level_t levels[RC_CASCADE_COUNT];
	uint32_t *table, *scratch_table, *requests;
	unsigned char *selected, *used_slots;
	rc_sparse_request_t *sort_items;
	rc_sparse_brick_t *active;
	rc_sparse_range_t *clears;
	rc_sparse_refresh_t *refresh;
	rc_sparse_refresh_work_t reservation;
	uint32_t table_count, total_slots, active_count, clear_count;
	uint32_t refresh_next;
	uint32_t priority_streak;
	uint64_t priority_serial;
	uint64_t topology_revision;
	float camera[3];
	bool frame_open;
	bool refresh_inflight;
};

static int RC_SparseCompare (const void *a, const void *b)
{
	const rc_sparse_request_t *x = a, *y = b;
	if (x->priority != y->priority) return x->priority < y->priority ? -1 : 1;
	if (x->distance_squared != y->distance_squared) return x->distance_squared < y->distance_squared ? -1 : 1;
	return x->index == y->index ? 0 : (x->index < y->index ? -1 : 1);
}

static int RC_SparseBrickCompare (const void *a, const void *b)
{
	const rc_sparse_brick_t *x = a, *y = b;
	return x->record_base == y->record_base ? 0 : (x->record_base < y->record_base ? -1 : 1);
}

rc_sparse_t *RC_SparseCreate (uint32_t grid_side, uint32_t direction_side, float spacing, float first_interval,
	const uint32_t capacities[RC_CASCADE_COUNT])
{
	rc_layout_t layout;
	uint64_t table_total = 0, slot_total = 0, record_total = 0, probe_total = 0;
	if (!capacities || !RC_LayoutInit (&layout, grid_side, direction_side, spacing, first_interval)) return NULL;
	rc_sparse_t *s = calloc (1, sizeof (*s));
	if (!s) return NULL;
	s->layout = layout;
	for (uint32_t i = 0; i < RC_CASCADE_COUNT; ++i)
	{
		rc_sparse_level_t *level = &s->levels[i];
		rc_level_t *descriptor = &s->layout.levels[i];
		uint64_t side = descriptor->layout[0], directions = descriptor->layout[1];
		/* The two coarsest grids used to be a single brick each. Any
		 * scroll therefore discarded their entire history. Split them into
		 * eight bricks so a one-brick scroll retains the overlapping half. */
		uint64_t brick = side <= 4 && side > 1 ? side / 2 : (side < 4 ? side : 4);
		if (!brick || side % brick) goto failed;
		uint64_t table_side = side / brick;
		uint64_t table_count = table_side * table_side * table_side;
		uint64_t probes_per_brick = brick * brick * brick;
		uint64_t records_per_brick = probes_per_brick * directions * directions;
		if (!capacities[i] || capacities[i] > table_count || records_per_brick > UINT32_MAX ||
			table_total + table_count > UINT32_MAX || slot_total + capacities[i] > UINT32_MAX ||
			record_total + (uint64_t)capacities[i] * records_per_brick > UINT32_MAX ||
			probe_total + (uint64_t)capacities[i] * probes_per_brick > UINT32_MAX) goto failed;
		level->brick_side = (uint32_t)brick;
		level->table_side = (uint32_t)table_side;
		level->table_offset = (uint32_t)table_total;
		level->table_count = (uint32_t)table_count;
		level->capacity = capacities[i];
		level->slot_offset = (uint32_t)slot_total;
		level->record_offset = (uint32_t)record_total;
		level->records_per_brick = (uint32_t)records_per_brick;
		descriptor->layout[2] = (uint32_t)table_total;
		descriptor->layout[3] = (uint32_t)brick;
		table_total += table_count;
		slot_total += capacities[i];
		record_total += (uint64_t)capacities[i] * records_per_brick;
		probe_total += (uint64_t)capacities[i] * probes_per_brick;
	}
	if (table_total > SIZE_MAX / sizeof (rc_sparse_request_t) || slot_total > SIZE_MAX / sizeof (rc_sparse_brick_t) ||
		slot_total > SIZE_MAX / sizeof (rc_sparse_range_t) || slot_total > SIZE_MAX / sizeof (rc_sparse_refresh_t)) goto failed;
	s->table_count = (uint32_t)table_total;
	s->total_slots = (uint32_t)slot_total;
	s->layout.record_count = (uint32_t)record_total;
	s->layout.probe_count = (uint32_t)probe_total;
	s->table = malloc ((size_t)table_total * sizeof (*s->table));
	s->scratch_table = malloc ((size_t)table_total * sizeof (*s->scratch_table));
	s->requests = malloc ((size_t)table_total * sizeof (*s->requests));
	s->selected = calloc ((size_t)table_total, sizeof (*s->selected));
	s->used_slots = calloc ((size_t)slot_total, sizeof (*s->used_slots));
	s->sort_items = malloc ((size_t)table_total * sizeof (*s->sort_items));
	s->active = malloc ((size_t)slot_total * sizeof (*s->active));
	s->clears = malloc ((size_t)slot_total * sizeof (*s->clears));
	s->refresh = calloc ((size_t)slot_total, sizeof (*s->refresh));
	if (!s->table || !s->scratch_table || !s->requests || !s->selected || !s->used_slots || !s->sort_items || !s->active || !s->clears)
		goto failed;
	if (!s->refresh) goto failed;
	/* Low-index fine slots are allocated in receiver-priority order. */
	s->refresh_next = 0;
	for (uint32_t i = 0; i < s->table_count; ++i) s->table[i] = RC_SPARSE_ABSENT;
	return s;
failed:
	RC_SparseDestroy (s);
	return NULL;
}

void RC_SparseDestroy (rc_sparse_t *s)
{
	if (!s) return;
	free (s->table); free (s->scratch_table); free (s->requests); free (s->selected);
	free (s->used_slots); free (s->sort_items); free (s->active); free (s->clears); free (s->refresh);
	free (s);
}

bool RC_SparseBeginFrame (rc_sparse_t *s, const float camera[3])
{
	if (!s || !camera || s->refresh_inflight || !isfinite (camera[0]) || !isfinite (camera[1]) || !isfinite (camera[2])) return false;
	/* Validate all origins before changing any state. */
	float origins[RC_CASCADE_COUNT][3];
	bool topology_changed = false;
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
		for (int axis = 0; axis < 3; ++axis)
		{
			const double spacing = s->layout.levels[l].origin_spacing[3];
			const double brick_world = spacing * s->levels[l].brick_side;
			const double half_extent = (double)s->layout.levels[l].layout[0] * spacing * 0.5;
			const double previous = s->layout.levels[l].origin_spacing[axis];
			const double center = previous + half_extent;
			/* Keep the existing grid while the camera is near its center. In
			 * particular, floor(camera / brick_world) used to move all five
			 * levels at zero; the one-brick coarse levels then lost all their
			 * history for a movement of only a fraction of a unit. Recenter on
			 * a brick-aligned position only after crossing a hysteresis band. */
			const double threshold = s->levels[l].table_side <= 2 ? 0.5 : 0.75;
			const double value = isfinite (previous) && fabs ((double)camera[axis] - center) <= brick_world * threshold ?
				previous : floor ((double)camera[axis] / brick_world + 0.5) * brick_world - half_extent;
			if (!isfinite (value) || fabs (value) > 3.402823466e38) return false;
			origins[l][axis] = (float)value;
		}
	for (uint32_t i = 0; i < s->table_count; ++i)
	{
		s->scratch_table[i] = RC_SPARSE_ABSENT;
		s->requests[i] = UINT32_MAX;
	}
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
	{
		rc_sparse_level_t *level = &s->levels[l];
		rc_level_t *descriptor = &s->layout.levels[l];
		int64_t delta[3] = {0, 0, 0};
		bool overlap = true;
		for (int axis = 0; axis < 3; ++axis)
		{
			const float previous = descriptor->origin_spacing[axis];
			if (previous != origins[l][axis]) topology_changed = true;
			const double brick_world = (double)descriptor->origin_spacing[3] * level->brick_side;
			if (!isfinite (previous)) overlap = false;
			else
			{
				const double shift = ((double)origins[l][axis] - previous) / brick_world;
				const double rounded = round (shift);
				if (!isfinite (shift) || fabs (shift - rounded) > 0.001 || fabs (rounded) >= level->table_side) overlap = false;
				else delta[axis] = (int64_t)rounded;
			}
			descriptor->origin_spacing[axis] = origins[l][axis];
		}
		if (!overlap) continue;
		const uint32_t side = level->table_side;
		for (uint32_t z = 0; z < side; ++z)
			for (uint32_t y = 0; y < side; ++y)
				for (uint32_t x = 0; x < side; ++x)
				{
					const int64_t ox = (int64_t)x + delta[0], oy = (int64_t)y + delta[1], oz = (int64_t)z + delta[2];
					if (ox >= 0 && oy >= 0 && oz >= 0 && ox < side && oy < side && oz < side)
					{
						uint32_t next = level->table_offset + (z * side + y) * side + x;
						uint32_t old = level->table_offset + ((uint32_t)oz * side + (uint32_t)oy) * side + (uint32_t)ox;
						s->scratch_table[next] = s->table[old];
					}
				}
	}
	memcpy (s->table, s->scratch_table, (size_t)s->table_count * sizeof (*s->table));
	memcpy (s->camera, camera, sizeof (s->camera));
	s->active_count = s->clear_count = 0;
	s->frame_open = true;
	if (topology_changed) ++s->topology_revision;
	return true;
}

static void RC_SparseMarkLevelAABB (rc_sparse_t *s, uint32_t l, const float mins[3], const float maxs[3], uint32_t priority, bool clamp_outside)
{
	const rc_sparse_level_t *level = &s->levels[l];
	const rc_level_t *descriptor = &s->layout.levels[l];
	uint32_t begin[3], end[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		const double spacing = descriptor->origin_spacing[3], origin = descriptor->origin_spacing[axis];
		double lo = floor (((double)mins[axis] - origin) / spacing - 0.5) - 1.0;
		double hi = ceil (((double)maxs[axis] - origin) / spacing - 0.5) + 1.0;
		if (hi < 0.0)
		{
			if (!clamp_outside) return;
			lo = hi = 0.0;
		}
		else if (lo >= descriptor->layout[0])
		{
			if (!clamp_outside) return;
			lo = hi = descriptor->layout[0] - 1u;
		}
		if (lo < 0.0) lo = 0.0;
		if (hi >= descriptor->layout[0]) hi = descriptor->layout[0] - 1u;
		begin[axis] = (uint32_t)lo / level->brick_side;
		end[axis] = (uint32_t)hi / level->brick_side;
	}
	for (uint32_t z = begin[2]; z <= end[2]; ++z)
		for (uint32_t y = begin[1]; y <= end[1]; ++y)
			for (uint32_t x = begin[0]; x <= end[0]; ++x)
			{
				const uint32_t index = level->table_offset + (z * level->table_side + y) * level->table_side + x;
				if (priority < s->requests[index]) s->requests[index] = priority;
			}
}

bool RC_SparseMarkAABB (rc_sparse_t *s, const float mins[3], const float maxs[3], uint32_t priority)
{
	if (!s || !s->frame_open || !mins || !maxs || priority == UINT32_MAX) return false;
	for (int axis = 0; axis < 3; ++axis)
		if (!isfinite (mins[axis]) || !isfinite (maxs[axis]) || mins[axis] > maxs[axis]) return false;
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
		RC_SparseMarkLevelAABB (s, l, mins, maxs, priority, false);
	return true;
}

bool RC_SparseResolve (rc_sparse_t *s)
{
	if (!s || !s->frame_open) return false;
	bool topology_changed = false;
	memset (s->selected, 0, s->table_count);
	memset (s->used_slots, 0, s->total_slots);
	s->active_count = s->clear_count = 0;
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
	{
		rc_sparse_level_t *level = &s->levels[l];
		const rc_level_t *descriptor = &s->layout.levels[l];
		/* Every selected finer brick contains many probes beyond the original
		 * request AABB. Request interpolation support for its whole extent;
		 * capacity shortages remain explicit in this level's overflow. */
		if (l > 0)
		{
			const rc_sparse_level_t *child = &s->levels[l - 1u];
			const rc_level_t *child_descriptor = &s->layout.levels[l - 1u];
			for (uint32_t i = 0; i < child->active_count; ++i)
			{
				const rc_sparse_brick_t *brick = &s->active[child->active_base + i];
				const uint32_t xyz[3] = {brick->x, brick->y, brick->z};
				float mins[3], maxs[3];
				for (int axis = 0; axis < 3; ++axis)
				{
					mins[axis] = child_descriptor->origin_spacing[axis] +
						((float)(xyz[axis] * child->brick_side) + 0.5f) * child_descriptor->origin_spacing[3];
					maxs[axis] = child_descriptor->origin_spacing[axis] +
						((float)((xyz[axis] + 1u) * child->brick_side) - 0.5f) * child_descriptor->origin_spacing[3];
				}
				const uint32_t child_index = child->table_offset + (brick->z * child->table_side + brick->y) * child->table_side + brick->x;
				RC_SparseMarkLevelAABB (s, l, mins, maxs, s->requests[child_index], true);
			}
		}
		uint32_t requested = 0;
		for (uint32_t local = 0; local < level->table_count; ++local)
		{
			const uint32_t index = level->table_offset + local;
			if (s->requests[index] == UINT32_MAX) continue;
			uint32_t remainder = local, xyz[3];
			for (int axis = 0; axis < 3; ++axis) { xyz[axis] = remainder % level->table_side; remainder /= level->table_side; }
			double distance_squared = 0.0;
			for (int axis = 0; axis < 3; ++axis)
			{
				const double center = descriptor->origin_spacing[axis] +
					((double)xyz[axis] + 0.5) * level->brick_side * descriptor->origin_spacing[3];
				const double difference = center - s->camera[axis];
				distance_squared += difference * difference;
			}
			s->sort_items[requested++] = (rc_sparse_request_t){index, s->requests[index], distance_squared};
		}
		qsort (s->sort_items, requested, sizeof (*s->sort_items), RC_SparseCompare);
		const uint32_t selected_count = requested < level->capacity ? requested : level->capacity;
		level->overflow = requested - selected_count;
		for (uint32_t i = 0; i < selected_count; ++i) s->selected[s->sort_items[i].index] = 1;
		for (uint32_t local = 0; local < level->table_count; ++local)
		{
			const uint32_t index = level->table_offset + local, base = s->table[index];
			if (base == RC_SPARSE_ABSENT) continue;
			if (!s->selected[index]) { s->table[index] = RC_SPARSE_ABSENT; topology_changed = true; continue; }
			if (base < level->record_offset || (base - level->record_offset) % level->records_per_brick) return false;
			const uint32_t slot = (base - level->record_offset) / level->records_per_brick;
			if (slot >= level->capacity || s->used_slots[level->slot_offset + slot]) return false;
			s->used_slots[level->slot_offset + slot] = 1;
		}
		level->active_base = s->active_count;
		level->active_count = selected_count;
		for (uint32_t i = 0; i < selected_count; ++i)
		{
			const uint32_t index = s->sort_items[i].index;
			if (s->table[index] == RC_SPARSE_ABSENT)
			{
				topology_changed = true;
				uint32_t slot = 0;
				while (slot < level->capacity && s->used_slots[level->slot_offset + slot]) ++slot;
				if (slot == level->capacity) return false;
				s->used_slots[level->slot_offset + slot] = 1;
				rc_sparse_refresh_t *refresh = &s->refresh[level->slot_offset + slot];
				uint32_t generation = refresh->generation + 1u;
				/* New slots are invalid on the GPU, so schedule nearby fine
				 * probes promptly while the regular far-to-near sweep runs. */
				*refresh = (rc_sparse_refresh_t){.generation = generation, .pending = true};
				s->table[index] = level->record_offset + slot * level->records_per_brick;
				s->clears[s->clear_count++] = (rc_sparse_range_t){s->table[index], level->records_per_brick};
			}
			uint32_t local = index - level->table_offset;
			rc_sparse_brick_t brick;
			brick.x = local % level->table_side; local /= level->table_side;
			brick.y = local % level->table_side; brick.z = local / level->table_side;
			brick.record_base = s->table[index];
			s->active[s->active_count++] = brick;
		}
		/* Stable physical ordering makes the list insensitive to priority or
		 * distance changes that do not actually replace allocated bricks. */
		qsort (s->active + level->active_base, selected_count, sizeof (*s->active), RC_SparseBrickCompare);
	}
	/* Retired slots have no data to refresh. A future allocation resets the
	 * slot's generation and begins with invalid records. */
	for (uint32_t slot = 0; slot < s->total_slots; ++slot)
		if (!s->used_slots[slot])
		{
			s->refresh[slot].pending = s->refresh[slot].again = s->refresh[slot].priority = s->refresh[slot].cleanup = false;
			s->refresh[slot].progress = 0;
		}
	s->frame_open = false;
	if (topology_changed) ++s->topology_revision;
	return true;
}

uint64_t RC_SparseTopologyRevision (const rc_sparse_t *s) { return s ? s->topology_revision : 0; }
const rc_layout_t *RC_SparseLayout (const rc_sparse_t *s) { return s ? &s->layout : NULL; }
const uint32_t *RC_SparseTable (const rc_sparse_t *s, uint32_t *count)
{ if (count) *count = s ? s->table_count : 0; return s ? s->table : NULL; }
const rc_sparse_brick_t *RC_SparseActive (const rc_sparse_t *s, uint32_t *count)
{ if (count) *count = s ? s->active_count : 0; return s ? s->active : NULL; }
const rc_sparse_range_t *RC_SparseClears (const rc_sparse_t *s, uint32_t *count)
{ if (count) *count = s ? s->clear_count : 0; return s ? s->clears : NULL; }
bool RC_SparseLevelWork (const rc_sparse_t *s, uint32_t level, uint32_t *active_base,
	uint32_t *active_count, uint32_t *records_per_brick, uint32_t *overflow)
{
	if (!s || level >= RC_CASCADE_COUNT || !active_base || !active_count || !records_per_brick || !overflow) return false;
	*active_base = s->levels[level].active_base;
	*active_count = s->levels[level].active_count;
	*records_per_brick = s->levels[level].records_per_brick;
	*overflow = s->levels[level].overflow;
	return true;
}

bool RC_SparseSchedulerNext (rc_scheduler_t *scheduler, const rc_sparse_t *s, uint32_t budget,
	uint32_t *level_out, uint32_t *first_out, uint32_t *count_out)
{
	if (!scheduler || !s || !budget || !level_out || !first_out || !count_out || scheduler->level >= RC_CASCADE_COUNT)
		return false;
	for (uint32_t visited = 0; visited < RC_CASCADE_COUNT; ++visited)
	{
		const uint32_t level = scheduler->level;
		const uint64_t wide = (uint64_t)s->levels[level].active_count * s->levels[level].records_per_brick;
		if (wide > UINT32_MAX) return false;
		const uint32_t total = (uint32_t)wide;
		if (scheduler->cursor[level] >= total) scheduler->cursor[level] = 0;
		if (total)
		{
			const uint32_t remaining = total - scheduler->cursor[level];
			const uint32_t count = remaining < budget ? remaining : budget;
			*level_out = level;
			*first_out = scheduler->cursor[level];
			*count_out = count;
			scheduler->cursor[level] += count;
			if (scheduler->cursor[level] < total) return true;
		}
		scheduler->cursor[level] = 0;
		if (!level) scheduler->initial_sweep = false;
		scheduler->level = level ? level - 1u : RC_CASCADE_COUNT - 1u;
		if (total) return true;
	}
	return false;
}

bool RC_SparseRequestRefresh (rc_sparse_t *s, const float mins[3], const float maxs[3])
{
	if (!s || s->frame_open || !mins || !maxs) return false;
	for (int axis = 0; axis < 3; ++axis)
		if (!isfinite (mins[axis]) || !isfinite (maxs[axis]) || mins[axis] > maxs[axis]) return false;
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
	{
		const rc_sparse_level_t *level = &s->levels[l];
		const rc_level_t *descriptor = &s->layout.levels[l];
		const double max_distance = descriptor->interval[1];
		for (uint32_t i = 0; i < level->active_count; ++i)
		{
			const rc_sparse_brick_t *brick = &s->active[level->active_base + i];
			const uint32_t xyz[3] = {brick->x, brick->y, brick->z};
			double distance_squared = 0;
			for (int axis = 0; axis < 3; ++axis)
			{
				const double spacing = descriptor->origin_spacing[3];
				const double lo = descriptor->origin_spacing[axis] + ((double)xyz[axis] * level->brick_side + 0.5) * spacing;
				const double hi = descriptor->origin_spacing[axis] + ((double)(xyz[axis] + 1u) * level->brick_side - 0.5) * spacing;
				double gap = 0;
				if (hi < mins[axis]) gap = mins[axis] - hi;
				else if (lo > maxs[axis]) gap = lo - maxs[axis];
				distance_squared += gap * gap;
			}
			if (distance_squared > max_distance * max_distance) continue;
			const uint32_t local_slot = (brick->record_base - level->record_offset) / level->records_per_brick;
			const uint32_t slot = level->slot_offset + local_slot;
			rc_sparse_refresh_t *refresh = &s->refresh[slot];
			if (!refresh->pending)
			{
				refresh->pending = true;
				refresh->progress = 0;
				refresh->again = false;
			}
			else if (refresh->progress || (s->refresh_inflight && s->reservation.slot == slot))
				refresh->again = true;
		}
	}
	return true;
}

bool RC_SparsePrioritizeInfluence (rc_sparse_t *s, const float source[3], float max_distance, const float receiver[3])
{
	if (!s || s->frame_open || !source || !receiver || !isfinite (max_distance) || max_distance < 0) return false;
	for (int axis = 0; axis < 3; ++axis) if (!isfinite (source[axis]) || !isfinite (receiver[axis])) return false;
	const rc_sparse_level_t *level = &s->levels[0];
	const rc_level_t *descriptor = &s->layout.levels[0];
	/* The camera is usually well outside the visibly lit surface region. Aim
	 * toward the source in the horizontal plane while retaining the receiver's
	 * height so ground and walls visible from the camera receive prompt updates. */
	const float focus[3] = { source[0], source[1], receiver[2] };
	double best_distance[2] = {HUGE_VAL, HUGE_VAL};
	uint32_t best_slot[2] = {UINT32_MAX, UINT32_MAX};
	uint32_t priorities = 0;
	for (uint32_t i = 0; i < level->active_count; ++i)
	{
		const rc_sparse_brick_t *brick = &s->active[level->active_base + i];
		const uint32_t slot = level->slot_offset + (brick->record_base - level->record_offset) / level->records_per_brick;
		if (!s->refresh[slot].pending) continue;
		priorities += s->refresh[slot].priority;
		const uint32_t xyz[3] = {brick->x, brick->y, brick->z};
		double source_distance = 0, receiver_distance = 0;
		for (int axis = 0; axis < 3; ++axis)
		{
			const double spacing = descriptor->origin_spacing[3];
			const double lo = descriptor->origin_spacing[axis] + ((double)xyz[axis] * level->brick_side + 0.5) * spacing;
			const double hi = descriptor->origin_spacing[axis] + ((double)(xyz[axis] + 1u) * level->brick_side - 0.5) * spacing;
			double gap = source[axis] < lo ? lo - source[axis] : source[axis] > hi ? source[axis] - hi : 0;
			source_distance += gap * gap;
			gap = focus[axis] < lo ? lo - focus[axis] : focus[axis] > hi ? focus[axis] - hi : 0;
			receiver_distance += gap * gap;
		}
		/* A queued brick near this source already serves the same transient,
		 * but a different, distant source must be able to enqueue its own. */
		if (s->refresh[slot].priority)
		{
			if (receiver_distance <= (double)descriptor->origin_spacing[3] * descriptor->origin_spacing[3]) return true;
			continue;
		}
		/* Rotate among nearby affected bricks. A brief cooldown avoids tracing
		 * the exact same brick on every decay tick without sending work to the
		 * far end of the light's volume just because it is globally older. */
		const uint64_t last = s->refresh[slot].last_priority;
		const unsigned recent = last && s->priority_serial - last < 8 ? 1u : 0u;
		if (source_distance <= (double)max_distance * max_distance && receiver_distance < best_distance[recent])
		{ best_distance[recent] = receiver_distance; best_slot[recent] = slot; }
	}
	/* Animated lights can change every frame. Bound their queued urgent work;
	 * the ordinary refresh queue remains available for the rest. */
	const uint32_t chosen = priorities < RC_SPARSE_URGENT_MAX ? (best_slot[0] != UINT32_MAX ? best_slot[0] : best_slot[1]) : UINT32_MAX;
	if (chosen != UINT32_MAX) s->refresh[chosen].priority = true;
	return true;
}

bool RC_SparsePrioritizeExpiredInfluence (rc_sparse_t *s, const float source[3], float max_distance)
{
	if (!s || s->frame_open || !source || !isfinite (max_distance) || max_distance < 0) return false;
	for (int axis = 0; axis < 3; ++axis) if (!isfinite (source[axis])) return false;
	const rc_sparse_level_t *level = &s->levels[0];
	const rc_level_t *descriptor = &s->layout.levels[0];
	for (uint32_t i = 0; i < level->active_count; ++i)
	{
		const rc_sparse_brick_t *brick = &s->active[level->active_base + i];
		const uint32_t slot = level->slot_offset + (brick->record_base - level->record_offset) / level->records_per_brick;
		rc_sparse_refresh_t *refresh = &s->refresh[slot];
		if (!refresh->pending || !refresh->last_priority) continue;
		const uint32_t xyz[3] = {brick->x, brick->y, brick->z};
		double distance_squared = 0;
		for (int axis = 0; axis < 3; ++axis)
		{
			const double spacing = descriptor->origin_spacing[3];
			const double lo = descriptor->origin_spacing[axis] + ((double)xyz[axis] * level->brick_side + 0.5) * spacing;
			const double hi = descriptor->origin_spacing[axis] + ((double)(xyz[axis] + 1u) * level->brick_side - 0.5) * spacing;
			const double gap = source[axis] < lo ? lo - source[axis] : source[axis] > hi ? source[axis] - hi : 0;
			distance_squared += gap * gap;
		}
		if (distance_squared <= (double)max_distance * max_distance) refresh->cleanup = true;
	}
	return true;
}

bool RC_SparseRefreshNext (rc_sparse_t *s, uint32_t budget, rc_sparse_refresh_work_t *work)
{
	if (!s || s->frame_open || s->refresh_inflight || !budget || !work) return false;
	/* Cleanup eligibility is bounded by the allocated slots, but only a
	 * handful are promoted at once. Refill as those slots finish so a large
	 * or multi-light event cannot leave the remaining former influence stale. */
	uint32_t priority_count = RC_SparseUrgentPending (s);
	for (uint32_t slot = 0; slot < s->total_slots && priority_count < RC_SPARSE_URGENT_MAX; ++slot)
	{
		rc_sparse_refresh_t *refresh = &s->refresh[slot];
		if (refresh->pending && refresh->cleanup && !refresh->priority)
		{ refresh->priority = true; ++priority_count; }
	}
	uint32_t best_distance[2] = {UINT32_MAX, UINT32_MAX};
	rc_sparse_refresh_work_t candidates[2] = {{0}, {0}};
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
	{
		const rc_sparse_level_t *level = &s->levels[l];
		for (uint32_t i = 0; i < level->active_count; ++i)
		{
			const rc_sparse_brick_t *brick = &s->active[level->active_base + i];
			const uint32_t slot = level->slot_offset + (brick->record_base - level->record_offset) / level->records_per_brick;
			const rc_sparse_refresh_t *refresh = &s->refresh[slot];
			if (!refresh->pending) continue;
			const unsigned priority = refresh->priority ? 1u : 0u;
			const uint32_t distance = slot >= s->refresh_next ? slot - s->refresh_next : s->total_slots - (s->refresh_next - slot);
			if (distance >= best_distance[priority]) continue;
			const uint32_t remaining = level->records_per_brick - refresh->progress;
			candidates[priority] = (rc_sparse_refresh_work_t){l, i * level->records_per_brick + refresh->progress,
				remaining < budget ? remaining : budget, slot, refresh->generation};
			best_distance[priority] = distance;
		}
	}
	if (best_distance[0] == UINT32_MAX && best_distance[1] == UINT32_MAX) return false;
	/* At least one ordinary completed brick in four, when one is waiting. */
	const unsigned selected = best_distance[1] != UINT32_MAX && (s->priority_streak < 3 || best_distance[0] == UINT32_MAX) ? 1u : 0u;
	rc_sparse_refresh_work_t candidate = candidates[selected];
	s->reservation = candidate;
	s->refresh_inflight = true;
	*work = candidate;
	return true;
}

bool RC_SparseRefreshCommit (rc_sparse_t *s, const rc_sparse_refresh_work_t *work, bool submitted)
{
	if (!s || !work || !s->refresh_inflight || work->level != s->reservation.level || work->first != s->reservation.first ||
		work->count != s->reservation.count || work->slot != s->reservation.slot || work->generation != s->reservation.generation ||
		work->slot >= s->total_slots || work->level >= RC_CASCADE_COUNT) return false;
	rc_sparse_refresh_t *refresh = &s->refresh[work->slot];
	const rc_sparse_level_t *level = &s->levels[work->level];
	if (!refresh->pending || refresh->generation != work->generation || refresh->progress > level->records_per_brick ||
		work->count > level->records_per_brick - refresh->progress) return false;
	if (submitted)
	{
		refresh->progress += work->count;
		if (refresh->progress == level->records_per_brick)
		{
			if (refresh->priority && ++s->priority_serial == 0)
			{
				for (uint32_t i = 0; i < s->total_slots; ++i) s->refresh[i].last_priority = 0;
				s->priority_serial = 1;
			}
			if (refresh->priority) refresh->last_priority = s->priority_serial;
			if (refresh->priority) { if (s->priority_streak < 3) ++s->priority_streak; }
			else s->priority_streak = 0;
			refresh->priority = false;
			refresh->progress = 0;
			refresh->cleanup = refresh->cleanup && refresh->again;
			refresh->pending = refresh->again;
			refresh->again = false;
			s->refresh_next = work->slot + 1u < s->total_slots ? work->slot + 1u : 0u;
		}
	}
	s->refresh_inflight = false;
	return true;
}

uint32_t RC_SparseRefreshPending (const rc_sparse_t *s)
{
	uint32_t count = 0;
	if (s) for (uint32_t slot = 0; slot < s->total_slots; ++slot) count += s->refresh[slot].pending;
	return count;
}

uint32_t RC_SparseUrgentPending (const rc_sparse_t *s)
{
	uint32_t count = 0;
	if (s) for (uint32_t slot = 0; slot < s->total_slots; ++slot) count += s->refresh[slot].pending && s->refresh[slot].priority;
	return count;
}
