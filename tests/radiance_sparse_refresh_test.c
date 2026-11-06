#include "radiance_sparse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main (void)
{
	const uint32_t capacities[RC_CASCADE_COUNT] = {128, 64, 8, 1, 1};
	rc_sparse_t *s = RC_SparseCreate (32, 4, 32, 64, capacities);
	assert (s);
	float camera[3] = {0}, bounds_min[3] = {-1500, -1500, -1500}, bounds_max[3] = {1500, 1500, 1500};
	assert (RC_SparseBeginFrame (s, camera));
	assert (RC_SparseMarkAABB (s, bounds_min, bounds_max, 0));
	assert (RC_SparseResolve (s));
	uint32_t total_active;
	assert (RC_SparseActive (s, &total_active) && total_active > 8);
	/* Newly allocated slots receive a priority pass, starting with fine
	 * camera-near work, even while the normal sweep builds far cascades. */
	assert (RC_SparseRefreshPending (s) == total_active);
	rc_sparse_refresh_work_t initial;
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &initial) && initial.level == 0);
	assert (RC_SparseRefreshCommit (s, &initial, true));
	while (RC_SparseRefreshNext (s, UINT32_MAX, &initial)) assert (RC_SparseRefreshCommit (s, &initial, true));
	assert (RC_SparseRefreshPending (s) == 0);
	float remote[3] = {1000000, 1000000, 1000000}, local[3] = {0};
	assert (RC_SparseRequestRefresh (s, remote, remote));
	assert (RC_SparseRefreshPending (s) == 0);
	assert (RC_SparseRequestRefresh (s, local, local));
	uint32_t requested = RC_SparseRefreshPending (s);
	assert (requested > 0 && requested < total_active);
	/* Duplicate notifications before processing coalesce into one pass. */
	assert (RC_SparseRequestRefresh (s, local, local) && RC_SparseRefreshPending (s) == requested);
	rc_sparse_refresh_work_t first, retry, tampered;
	assert (RC_SparseRefreshNext (s, 17, &first) && first.count == 17);
	assert (!RC_SparseRefreshNext (s, 17, &retry));
	assert (!RC_SparseBeginFrame (s, camera)); /* frame slot is reserved */
	tampered = first; ++tampered.count;
	assert (!RC_SparseRefreshCommit (s, &tampered, true));
	assert (RC_SparseRefreshCommit (s, &first, false)); /* discarded command buffer */
	assert (RC_SparseRefreshNext (s, 17, &retry) && !memcmp (&retry, &first, sizeof (first)));
	assert (RC_SparseRefreshCommit (s, &retry, true));
	/* A change in the middle of a brick queues a later complete pass, rather
	 * than resetting its current progress on every animation frame. */
	assert (RC_SparseRequestRefresh (s, local, local));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry));
	assert (retry.slot == first.slot && retry.first == first.first + first.count);
	assert (RC_SparseRefreshCommit (s, &retry, true));
	assert (RC_SparseRefreshPending (s) == requested);
	uint32_t dispatches = 0;
	while (RC_SparseRefreshNext (s, UINT32_MAX, &retry))
	{
		assert (RC_SparseRefreshCommit (s, &retry, true));
		assert (++dispatches <= requested + 1);
	}
	assert (dispatches == requested && RC_SparseRefreshPending (s) == 0);
	/* A second light change can arrive after a batch is reserved but before
	 * that batch is submitted. Its result cannot represent the newer light
	 * state, so completing the first pass must leave a complete retry queued. */
	assert (RC_SparseRequestRefresh (s, local, local));
	requested = RC_SparseRefreshPending (s);
	assert (requested > 0 && RC_SparseRefreshNext (s, 17, &first));
	assert (RC_SparseRequestRefresh (s, local, local));
	assert (RC_SparseRefreshCommit (s, &first, true));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry) && retry.slot == first.slot);
	assert (RC_SparseRefreshCommit (s, &retry, true));
	assert (RC_SparseRefreshPending (s) == requested);
	dispatches = 0;
	while (RC_SparseRefreshNext (s, UINT32_MAX, &retry))
	{
		assert (RC_SparseRefreshCommit (s, &retry, true));
		assert (++dispatches <= requested + 1);
	}
	assert (dispatches == requested && RC_SparseRefreshPending (s) == 0);
	/* An explosion can prioritize a nearby fine brick, even when the normal
	 * queue is already long. Discarded work is retried and persistent effects
	 * cannot starve ordinary refreshes. */
	uint32_t base, count, per, overflow;
	assert (RC_SparseLevelWork (s, 0, &base, &count, &per, &overflow) && count > 1);
	const rc_sparse_brick_t *active = RC_SparseActive (s, NULL);
	const rc_sparse_brick_t target = active[base + count - 1];
	const rc_layout_t *layout = RC_SparseLayout (s);
	float point[3];
	const uint32_t xyz[3] = {target.x, target.y, target.z};
	for (int a = 0; a < 3; ++a)
		point[a] = layout->levels[0].origin_spacing[a] + (xyz[a] * 4.0f + 2.0f) * layout->levels[0].origin_spacing[3];
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	assert (RC_SparsePrioritizeInfluence (s, point, 0, point));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &first) && first.level == 0 && first.slot == target.record_base / per);
	assert (RC_SparseRefreshCommit (s, &first, false));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry) && !memcmp (&retry, &first, sizeof (first)));
	assert (RC_SparseRefreshCommit (s, &retry, true));
	for (int i = 0; i < 2; ++i)
	{
		assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
		assert (RC_SparsePrioritizeInfluence (s, point, 0, point));
		assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry) && retry.slot == first.slot);
		assert (RC_SparseRefreshCommit (s, &retry, true));
	}
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	assert (RC_SparsePrioritizeInfluence (s, point, 0, point));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry) && retry.slot != first.slot);
	assert (RC_SparseRefreshCommit (s, &retry, true));
	/* A continuously changing source must make progress around a receiver,
	 * rather than selecting the same nearest brick after every commit. */
	while (RC_SparseRefreshNext (s, UINT32_MAX, &retry)) assert (RC_SparseRefreshCommit (s, &retry, true));
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	assert (RC_SparsePrioritizeInfluence (s, camera, 10000, point));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &first) && first.level == 0);
	assert (RC_SparseRefreshCommit (s, &first, true));
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	assert (RC_SparsePrioritizeInfluence (s, camera, 10000, point));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry) && retry.level == 0 && retry.slot != first.slot);
	assert (RC_SparseRefreshCommit (s, &retry, true));
	/* Retiring slots during a teleport drops stale work instead of applying it
	 * to newly assigned bricks, whose validity will be cleared on the GPU. */
	assert (RC_SparseRequestRefresh (s, local, local) && RC_SparseRefreshPending (s));
	assert (RC_SparseBeginFrame (s, remote));
	assert (RC_SparseMarkAABB (s, remote, remote, 0));
	assert (RC_SparseResolve (s));
	assert (RC_SparseActive (s, &total_active));
	assert (RC_SparseRefreshPending (s) == total_active);
	RC_SparseDestroy (s);
	/* A transient's visible influence can be far in front of the camera.
	 * Prioritize near the source at camera height, and revisit that same
	 * completed brick when the source disappears. */
	s = RC_SparseCreate (32, 2, 32, 64, capacities);
	assert (s && RC_SparseBeginFrame (s, camera));
	assert (RC_SparseMarkAABB (s, bounds_min, bounds_max, 0) && RC_SparseResolve (s));
	while (RC_SparseRefreshNext (s, UINT32_MAX, &retry)) assert (RC_SparseRefreshCommit (s, &retry, true));
	assert (RC_SparseLevelWork (s, 0, &base, &count, &per, &overflow) && count > 1);
	active = RC_SparseActive (s, NULL);
	const rc_sparse_brick_t source_brick = active[base + count - 1];
	layout = RC_SparseLayout (s);
	const uint32_t source_xyz[3] = {source_brick.x, source_brick.y, source_brick.z};
	float source[3], viewer[3];
	for (int axis = 0; axis < 3; ++axis)
		source[axis] = layout->levels[0].origin_spacing[axis] + (source_xyz[axis] * 4.0f + 2.0f) * layout->levels[0].origin_spacing[3];
	viewer[0] = source[0] - 400.0f; viewer[1] = source[1] - 400.0f; viewer[2] = source[2];
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	assert (RC_SparsePrioritizeInfluence (s, source, 10000, viewer));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &first));
	assert (first.level == 0 && first.slot == source_brick.record_base / per);
	assert (RC_SparseRefreshCommit (s, &first, true));
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	assert (RC_SparsePrioritizeExpiredInfluence (s, source, 0));
	assert (RC_SparseRefreshNext (s, 17, &retry));
	assert (retry.slot == first.slot && retry.count == 17);
	assert (RC_SparseRefreshCommit (s, &retry, false));
	assert (RC_SparseRefreshNext (s, 17, &tampered) && !memcmp (&retry, &tampered, sizeof (retry)));
	assert (RC_SparseRefreshCommit (s, &tampered, true));
	while (RC_SparseRefreshNext (s, UINT32_MAX, &retry)) assert (RC_SparseRefreshCommit (s, &retry, true));
	const rc_sparse_brick_t other_brick = active[base];
	const uint32_t other_xyz[3] = {other_brick.x, other_brick.y, other_brick.z};
	float other_source[3];
	for (int axis = 0; axis < 3; ++axis)
		other_source[axis] = layout->levels[0].origin_spacing[axis] + (other_xyz[axis] * 4.0f + 2.0f) * layout->levels[0].origin_spacing[3];
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	assert (RC_SparsePrioritizeInfluence (s, source, 0, source));
	assert (RC_SparsePrioritizeInfluence (s, other_source, 0, other_source));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &first));
	assert (RC_SparseRefreshCommit (s, &first, true));
	assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry));
	assert (RC_SparseRefreshCommit (s, &retry, true));
	const uint32_t a = source_brick.record_base / per, b = other_brick.record_base / per;
	assert ((first.slot == a && retry.slot == b) || (first.slot == b && retry.slot == a));
	while (RC_SparseRefreshNext (s, UINT32_MAX, &retry)) assert (RC_SparseRefreshCommit (s, &retry, true));
	assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
	for (uint32_t i = 0; i < 20; ++i)
	{
		const rc_sparse_brick_t *brick = &active[base + i];
		const uint32_t cell[3] = {brick->x, brick->y, brick->z};
		float moving[3];
		for (int axis = 0; axis < 3; ++axis)
			moving[axis] = layout->levels[0].origin_spacing[axis] + (cell[axis] * 4.0f + 2.0f) * layout->levels[0].origin_spacing[3];
		assert (RC_SparsePrioritizeInfluence (s, moving, 0, moving));
		assert (RC_SparseUrgentPending (s) <= RC_SPARSE_URGENT_MAX);
	}
	assert (RC_SparseUrgentPending (s) == RC_SPARSE_URGENT_MAX);
	/* At least every fourth completed urgent brick yields to ordinary work. */
	for (int i = 0; i < 4; ++i)
	{
		uint32_t before = RC_SparseUrgentPending (s);
		assert (RC_SparseRefreshNext (s, UINT32_MAX, &retry));
		assert (RC_SparseRefreshCommit (s, &retry, true));
		assert (RC_SparseUrgentPending (s) == before - (i < 3));
	}
	/* Rapid movement and disappearance cannot grow the urgent queue beyond
	 * its cap, and partial work continues within the caller's ray budget. */
	for (uint32_t i = 0; i < 64; ++i)
	{
		const rc_sparse_brick_t *brick = &active[base + i % count];
		const uint32_t cell[3] = {brick->x, brick->y, brick->z};
		float moving[3];
		for (int axis = 0; axis < 3; ++axis)
			moving[axis] = layout->levels[0].origin_spacing[axis] + (cell[axis] * 4.0f + 2.0f) * layout->levels[0].origin_spacing[3];
		assert (RC_SparseRequestRefresh (s, bounds_min, bounds_max));
		assert (RC_SparsePrioritizeExpiredInfluence (s, moving, 128));
		assert (RC_SparsePrioritizeInfluence (s, moving, 128, moving));
		assert (RC_SparseUrgentPending (s) <= RC_SPARSE_URGENT_MAX);
		assert (RC_SparseRefreshNext (s, 17, &retry) && retry.count <= 17);
		assert (RC_SparseRefreshCommit (s, &retry, true));
	}
	assert (RC_SparseBeginFrame (s, remote));
	assert (RC_SparseMarkAABB (s, remote, remote, 0) && RC_SparseResolve (s));
	assert (RC_SparseUrgentPending (s) == 0);
	RC_SparseDestroy (s);
	puts ("PASS: bounded, fair and transactional sparse refresh");
	return 0;
}
