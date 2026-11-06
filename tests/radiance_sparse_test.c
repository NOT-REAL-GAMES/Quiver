#include "radiance_sparse.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t table_index (const rc_level_t *level, const float point[3])
{
	uint32_t cell[3], side = level->layout[0], brick = level->layout[3];
	for (int a = 0; a < 3; ++a)
	{
		double coordinate = floor (((double)point[a] - level->origin_spacing[a]) / level->origin_spacing[3]);
		assert (coordinate >= 0 && coordinate < side);
		cell[a] = (uint32_t)coordinate / brick;
	}
	uint32_t table_side = side / brick;
	return level->layout[2] + (cell[2] * table_side + cell[1]) * table_side + cell[0];
}

static void assert_halo (const rc_sparse_t *s, const float point[3])
{
	const rc_layout_t *layout = RC_SparseLayout (s);
	uint32_t total;
	const uint32_t *table = RC_SparseTable (s, &total);
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
	{
		const rc_level_t *c = &layout->levels[l];
		uint32_t side = c->layout[0], brick = c->layout[3], table_side = side / brick;
		int lower[3];
		for (int a = 0; a < 3; ++a)
			lower[a] = (int)floor (((double)point[a] - c->origin_spacing[a]) / c->origin_spacing[3] - 0.5);
		for (uint32_t corner = 0; corner < 8; ++corner)
		{
			uint32_t b[3];
			for (int a = 0; a < 3; ++a)
			{
				int cell = lower[a] + (int)((corner >> a) & 1u);
				if (cell < 0) cell = 0;
				if (cell >= (int)side) cell = (int)side - 1;
				b[a] = (uint32_t)cell / brick;
			}
			uint32_t index = c->layout[2] + (b[2] * table_side + b[1]) * table_side + b[0];
			assert (index < total && table[index] != RC_SPARSE_ABSENT);
		}
	}
}

static void assert_parent_coverage (const rc_sparse_t *s)
{
	const rc_layout_t *layout = RC_SparseLayout (s);
	uint32_t table_count, total_active;
	const uint32_t *table = RC_SparseTable (s, &table_count);
	const rc_sparse_brick_t *active = RC_SparseActive (s, &total_active);
	for (uint32_t l = 0; l + 1 < RC_CASCADE_COUNT; ++l)
	{
		uint32_t base, count, records, overflow, far_base, far_count, far_records, far_overflow;
		assert (RC_SparseLevelWork (s, l, &base, &count, &records, &overflow));
		assert (RC_SparseLevelWork (s, l + 1, &far_base, &far_count, &far_records, &far_overflow));
		assert (!far_overflow && base + count <= total_active);
		const rc_level_t *near = &layout->levels[l], *far = &layout->levels[l + 1];
		const uint32_t brick = near->layout[3], far_brick = far->layout[3], far_table_side = far->layout[0] / far_brick;
		for (uint32_t b = 0; b < count; ++b)
			for (uint32_t z = 0; z < brick; ++z)
				for (uint32_t y = 0; y < brick; ++y)
					for (uint32_t x = 0; x < brick; ++x)
					{
						const rc_sparse_brick_t *p = &active[base + b];
						uint32_t coords[3] = {p->x * brick + x, p->y * brick + y, p->z * brick + z};
						int lower[3];
						for (int axis = 0; axis < 3; ++axis)
						{
							double world = near->origin_spacing[axis] + (coords[axis] + 0.5) * near->origin_spacing[3];
							lower[axis] = (int)floor ((world - far->origin_spacing[axis]) / far->origin_spacing[3] - 0.5);
						}
						for (uint32_t corner = 0; corner < 8; ++corner)
						{
							uint32_t cell[3];
							for (int axis = 0; axis < 3; ++axis)
							{
								int q = lower[axis] + (int)((corner >> axis) & 1u);
								if (q < 0) q = 0;
								if (q >= (int)far->layout[0]) q = (int)far->layout[0] - 1;
								cell[axis] = (uint32_t)q / far_brick;
							}
							uint32_t idx = far->layout[2] + (cell[2] * far_table_side + cell[1]) * far_table_side + cell[0];
							assert (idx < table_count && table[idx] != RC_SPARSE_ABSENT);
						}
					}
	}
}

/* Crossing the origin shifts every camera-centered cascade at once.  A
 * resident world-space brick must retain its physical records in both
 * directions, and must never be queued for clearing as a newly allocated
 * brick.  Exercise each signed axis separately. */
static void assert_signed_origin_scrolling (void)
{
	const uint32_t caps[5] = {512, 64, 8, 8, 8};
	const float point[3] = {8, 8, 8};
	const float mins[3] = {-100000, -100000, -100000};
	const float maxs[3] = {100000, 100000, 100000};
	for (int axis = 0; axis < 3; ++axis)
	{
		rc_sparse_t *s = RC_SparseCreate (32, 4, 32, 64, caps);
		float camera[3] = {0, 0, 0};
		uint32_t bases[RC_CASCADE_COUNT];
		assert (s);
		camera[axis] = -1;
		assert (RC_SparseBeginFrame (s, camera));
		assert (RC_SparseMarkAABB (s, mins, maxs, 0));
		assert (RC_SparseResolve (s));
		const uint64_t revision = RC_SparseTopologyRevision (s);
		uint32_t count;
		const uint32_t *table = RC_SparseTable (s, &count);
		for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
		{
			bases[l] = table[table_index (&RC_SparseLayout (s)->levels[l], point)];
			assert (bases[l] != RC_SPARSE_ABSENT);
		}
		for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
		{
			uint32_t base, active, records, overflow;
			assert (RC_SparseLevelWork (s, l, &base, &active, &records, &overflow));
			assert (active == caps[l] && overflow == 0);
		}
		for (int step = 0; step < 20; ++step)
		{
			camera[axis] = (step & 1) ? -1 : 1;
			assert (RC_SparseBeginFrame (s, camera));
			assert (RC_SparseMarkAABB (s, mins, maxs, 0));
			assert (RC_SparseResolve (s));
			assert (RC_SparseTopologyRevision (s) == revision);
			table = RC_SparseTable (s, &count);
			uint32_t clear_count;
			const rc_sparse_range_t *clears = RC_SparseClears (s, &clear_count);
			assert (clear_count == 0);
			for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
			{
				assert (table[table_index (&RC_SparseLayout (s)->levels[l], point)] == bases[l]);
				for (uint32_t i = 0; i < clear_count; ++i)
					assert (bases[l] < clears[i].first || bases[l] >= clears[i].first + clears[i].count);
			}
		}
		RC_SparseDestroy (s);
	}
}

/* At actual scroll boundaries every level, including the coarsest, must
 * retain the records in its overlapping world-space region. */
static void assert_all_cascade_scroll_overlap (void)
{
	const uint32_t caps[5] = {512, 64, 8, 8, 8};
	const float point[3] = {8, 8, 8};
	const float mins[3] = {-100000, -100000, -100000};
	const float maxs[3] = {100000, 100000, 100000};
	for (uint32_t l = 0; l < RC_CASCADE_COUNT; ++l)
		for (int axis = 0; axis < 3; ++axis)
		{
			rc_sparse_t *s = RC_SparseCreate (32, 4, 32, 64, caps);
			float camera[3] = {0, 0, 0};
			assert (s && RC_SparseBeginFrame (s, camera));
			assert (RC_SparseMarkAABB (s, mins, maxs, 0));
			assert (RC_SparseResolve (s));
			const rc_level_t *level = &RC_SparseLayout (s)->levels[l];
			float brick_world = level->layout[3] * level->origin_spacing[3];
			uint32_t count;
			const uint32_t *table = RC_SparseTable (s, &count);
			const uint32_t old_base = table[table_index (level, point)];
			assert (old_base != RC_SPARSE_ABSENT);
			uint32_t active_base, active_count, records_per_brick, overflow, total_active;
			assert (RC_SparseLevelWork (s, l, &active_base, &active_count, &records_per_brick, &overflow));
			const rc_sparse_brick_t *active = RC_SparseActive (s, &total_active);
			assert (active && active_count == caps[l] && active_base + active_count <= total_active);
			const uint32_t level_first = active[active_base].record_base;
			const uint32_t level_end = active[active_base + active_count - 1].record_base + records_per_brick;
			const uint64_t revision = RC_SparseTopologyRevision (s);
			camera[axis] = brick_world;
			assert (RC_SparseBeginFrame (s, camera));
			assert (RC_SparseMarkAABB (s, mins, maxs, 0));
			assert (RC_SparseResolve (s));
			assert (RC_SparseTopologyRevision (s) != revision);
			table = RC_SparseTable (s, &count);
			level = &RC_SparseLayout (s)->levels[l];
			assert (table[table_index (level, point)] == old_base);
			uint32_t clear_count;
			const rc_sparse_range_t *clears = RC_SparseClears (s, &clear_count);
			bool cleared_new_slot = false;
			for (uint32_t i = 0; i < clear_count; ++i)
			{
				assert (old_base < clears[i].first || old_base >= clears[i].first + clears[i].count);
				if (clears[i].first >= level_first && clears[i].first < level_end)
					cleared_new_slot = true;
			}
			assert (cleared_new_slot);
			RC_SparseDestroy (s);
		}
}

int main (void)
{
	assert_signed_origin_scrolling ();
	assert_all_cascade_scroll_overlap ();
	const uint32_t caps[5] = {8, 8, 8, 8, 8};
	rc_sparse_t *s = RC_SparseCreate (32, 4, 32, 64, caps);
	assert (s);
	float camera[3] = {0, 0, 0}, point[3] = {0, 0, 0};
	assert (RC_SparseBeginFrame (s, camera));
	assert (RC_SparseMarkAABB (s, point, point, 0));
	assert (RC_SparseResolve (s));
	assert_halo (s, point);
	assert_parent_coverage (s);
	uint32_t table_count, active_count, clear_count;
	const uint32_t *table = RC_SparseTable (s, &table_count);
	const rc_sparse_brick_t *active = RC_SparseActive (s, &active_count);
	const rc_sparse_range_t *clears = RC_SparseClears (s, &clear_count);
	assert (table_count == 600 && active_count == clear_count && clear_count > 0);
	/* Sparse dispatch bounds are physical active records, not logical probes. */
	rc_scheduler_t scheduler;
	RC_SchedulerReset (&scheduler);
	uint32_t scheduled_level, first, scheduled_count, work_base, work_count, work_per_brick, work_overflow;
	assert (RC_SparseSchedulerNext (&scheduler, s, 31, &scheduled_level, &first, &scheduled_count));
	assert (scheduled_level == 4 && first == 0 && scheduled_count == 31);
	assert (RC_SparseLevelWork (s, scheduled_level, &work_base, &work_count, &work_per_brick, &work_overflow));
	assert ((uint64_t)first + scheduled_count <= (uint64_t)work_count * work_per_brick);
	scheduler.cursor[scheduled_level] = UINT32_MAX;
	assert (RC_SparseSchedulerNext (&scheduler, s, 31, &scheduled_level, &first, &scheduled_count) && first == 0);
	assert (sizeof (rc_sparse_brick_t) == 16);
	for (uint32_t i = 0; i < clear_count; ++i)
	{
		assert (clears[i].count && clears[i].first + clears[i].count <= RC_SparseLayout (s)->record_count);
		for (uint32_t j = i + 1; j < clear_count; ++j)
			assert (clears[i].first + clears[i].count <= clears[j].first || clears[j].first + clears[j].count <= clears[i].first);
	}
	assert (active != NULL);
	uint32_t old_base = table[table_index (&RC_SparseLayout (s)->levels[0], point)];
	assert (old_base != RC_SPARSE_ABSENT);
	uint64_t topology = RC_SparseTopologyRevision (s);
	/* Under one brick of movement, no world-space slots change. */
	camera[0] = 32;
	assert (RC_SparseBeginFrame (s, camera));
	assert (RC_SparseMarkAABB (s, point, point, 0));
	assert (RC_SparseResolve (s));
	assert (RC_SparseTopologyRevision (s) == topology);
	assert (RC_SparseClears (s, &clear_count) && clear_count == 0);
	/* On a brick boundary, logical indices change but overlapping world-space
	 * bricks keep their physical values and no clearing is needed. */
	camera[0] = 128;
	assert (RC_SparseBeginFrame (s, camera));
	assert (RC_SparseMarkAABB (s, point, point, 0));
	assert (RC_SparseResolve (s));
	assert (RC_SparseTopologyRevision (s) != topology);
	topology = RC_SparseTopologyRevision (s);
	assert_halo (s, point);
	assert_parent_coverage (s);
	table = RC_SparseTable (s, &table_count);
	assert (table[table_index (&RC_SparseLayout (s)->levels[0], point)] == old_base);
	assert (RC_SparseClears (s, &clear_count) && clear_count == 0);
	/* A teleport reuses old physical slots only after requesting validity clear. */
	camera[0] = point[0] = 1000000;
	assert (RC_SparseBeginFrame (s, camera));
	assert (RC_SparseMarkAABB (s, point, point, 0));
	assert (RC_SparseResolve (s));
	assert (RC_SparseTopologyRevision (s) != topology);
	assert_halo (s, point);
	assert_parent_coverage (s);
	assert (RC_SparseClears (s, &clear_count) && clear_count > 0);
	RC_SparseDestroy (s);

	/* Over-capacity selection remains deterministic regardless of mark order. */
	const uint32_t tight[5] = {1, 1, 1, 1, 1};
	rc_sparse_t *a = RC_SparseCreate (32, 4, 32, 64, tight);
	rc_sparse_t *b = RC_SparseCreate (32, 4, 32, 64, tight);
	assert (a && b);
	float zero[3] = {0, 0, 0}, other[3] = {300, 100, -50};
	assert (RC_SparseBeginFrame (a, zero) && RC_SparseBeginFrame (b, zero));
	assert (RC_SparseMarkAABB (a, zero, zero, 0) && RC_SparseMarkAABB (a, other, other, 5));
	assert (RC_SparseMarkAABB (b, other, other, 5) && RC_SparseMarkAABB (b, zero, zero, 0));
	assert (RC_SparseResolve (a) && RC_SparseResolve (b));
	uint32_t n1, n2;
	const uint32_t *t1 = RC_SparseTable (a, &n1), *t2 = RC_SparseTable (b, &n2);
	assert (n1 == n2 && !memcmp (t1, t2, (size_t)n1 * sizeof (*t1)));
	uint32_t active_base, count, per_brick, overflow;
	assert (RC_SparseLevelWork (a, 0, &active_base, &count, &per_brick, &overflow));
	assert (count == 1 && per_brick == 1024 && overflow > 0);
	assert (RC_SparseTable (a, &n1)[table_index (&RC_SparseLayout (a)->levels[0], other)] == RC_SPARSE_ABSENT);
	uint32_t active_total;
	const rc_sparse_brick_t *before_reuse = RC_SparseActive (a, &active_total);
	assert (active_total > 0);
	uint32_t reused_base = before_reuse[active_base].record_base;
	assert (RC_SparseBeginFrame (a, zero));
	assert (RC_SparseMarkAABB (a, other, other, 0) && RC_SparseMarkAABB (a, zero, zero, 5));
	assert (RC_SparseResolve (a));
	uint32_t reuse_clears;
	const rc_sparse_range_t *reuse = RC_SparseClears (a, &reuse_clears);
	bool found_reuse = false;
	for (uint32_t i = 0; i < reuse_clears; ++i)
		if (reuse[i].first == reused_base && reuse[i].count == per_brick) found_reuse = true;
	assert (found_reuse);
	RC_SparseDestroy (a); RC_SparseDestroy (b);
	puts ("PASS: bounded sparse cascades, halos, stable scrolling, teleports, and deterministic overflow");
	return 0;
}
