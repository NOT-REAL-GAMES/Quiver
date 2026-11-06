#include "radiance_sparse.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static rc_sparse_t *make (float requested)
{
	const uint32_t caps[5] = {512, 64, 8, 8, 8};
	float		   spacing = RC_ReceiverSpacing (requested);
	return RC_SparseCreate (32, 2, spacing, spacing * 2, caps);
}
static void prepare (rc_sparse_t *s, float x)
{
	float cam[3] = {x, 0, 0}, lo[3] = {-2000, -2000, -2000}, hi[3] = {2000, 2000, 2000};
	assert (RC_SparseBeginFrame (s, cam));
	assert (RC_SparseMarkAABB (s, lo, hi, 2));
	assert (RC_SparseResolve (s));
}
static int covered (const rc_sparse_t *s, float x)
{
	const rc_level_t *c = &RC_SparseLayout (s)->levels[0];
	return x >= c->origin_spacing[0] && x <= c->origin_spacing[0] + c->layout[0] * c->origin_spacing[3];
}
int main (void)
{
	assert (RC_ReceiverSpacing (NAN) == 32 && RC_ReceiverSpacing (INFINITY) == 32);
	assert (RC_ReceiverSpacing (-1) == 32 && RC_ReceiverSpacing (0) == 32);
	assert (RC_ReceiverSpacing (1) == 8 && RC_ReceiverSpacing (1000) == 128);
	const float spacings[] = {8, 16, 24, 24.5f, 32, 47, 48, 64, 128};
	uint32_t records = 0, probes = 0;
	for (unsigned i = 0; i < sizeof(spacings)/sizeof(*spacings); ++i)
	{
		const float spacing = spacings[i];
		assert (RC_ReceiverSpacing (spacing) == spacing);
		rc_sparse_t *s = make (spacing);
		assert (s);
		prepare (s, 0);
		const rc_layout_t *layout = RC_SparseLayout (s);
		if (!i) { records = layout->record_count; probes = layout->probe_count; }
		assert (layout->record_count == records && layout->probe_count == probes);
		for (unsigned l = 0; l < RC_CASCADE_COUNT; ++l)
		{
			const rc_level_t *level = &layout->levels[l];
			assert (level->origin_spacing[3] == ldexpf (spacing, l));
			assert (level->interval[1] == ldexpf (spacing * 2, l));
			/* The same local probe index must decode to the new lattice. */
			float origin[3], direction[3];
			assert (RC_DecodeInterval (level, 0, origin, direction));
			for (unsigned axis = 0; axis < 3; ++axis)
				assert (origin[axis] == level->origin_spacing[axis] + level->origin_spacing[3] * .5f);
			float adjacent[3];
			assert (RC_DecodeInterval (level, level->layout[1] * level->layout[1], adjacent, direction));
			assert (adjacent[0] - origin[0] == level->origin_spacing[3]);
			assert (adjacent[1] == origin[1] && adjacent[2] == origin[2]);
		}
		RC_SparseDestroy (s);
	}
	rc_sparse_t *small = make (32), *large = make (64);
	assert (small && large);
	prepare (small, 0);
	prepare (large, 0);
	assert (!covered (small, 700) && covered (large, 700));
	const rc_layout_t *a = RC_SparseLayout (small), *b = RC_SparseLayout (large);
	assert (a->probe_count == b->probe_count && a->record_count == b->record_count);
	const uint32_t expected_records = b->record_count;
	assert (b->levels[0].interval[0] == 0 && b->levels[0].interval[1] == 128);
	for (unsigned i = 1; i < 5; i++)
		assert (b->levels[i].interval[0] == b->levels[i - 1].interval[1]);
	uint32_t n;
	RC_SparseClears (large, &n);
	assert (n > 0);
	uint64_t rev = RC_SparseTopologyRevision (large);
	prepare (large, 191);
	assert (RC_SparseTopologyRevision (large) == rev);
	RC_SparseClears (large, &n);
	assert (n == 0);
	prepare (large, 193);
	assert (RC_SparseTopologyRevision (large) != rev);
	RC_SparseClears (large, &n);
	assert (n > 0);
	RC_SparseDestroy (large);
	large = make (64);
	assert (large);
	prepare (large, 256);
	RC_SparseClears (large, &n);
	assert (n > 0); /* resource recreation clears the complete new pool */
	rc_scheduler_t scheduler;
	RC_SchedulerReset (&scheduler);
	uint32_t		   level, first, count;
	unsigned long long total = 0;
	while (scheduler.initial_sweep)
	{
		assert (RC_SparseSchedulerNext (&scheduler, large, 2048, &level, &first, &count));
		assert (count <= 2048);
		total += count;
	}
	assert (total == expected_records); /* every active interval respects the per-frame work budget */
	printf ("coverage candidate: probes=%u intervals=%llu, max budget=2048, all tests passed\n", RC_SparseLayout (large)->probe_count, total);
	RC_SparseDestroy (small);
	RC_SparseDestroy (large);
	return 0;
}
