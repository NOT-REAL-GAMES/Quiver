/* Bounded world-space probe bricks for radiance cascades. */
#ifndef QUIVER_RADIANCE_SPARSE_H
#define QUIVER_RADIANCE_SPARSE_H

#include "radiance_cascades.h"

#define RC_SPARSE_ABSENT UINT32_MAX

typedef struct rc_sparse_s rc_sparse_t;

/* Changes whenever probe locations or the active physical slot mapping change. */
uint64_t RC_SparseTopologyRevision (const rc_sparse_t *state);
typedef struct rc_sparse_brick_s
{
	uint32_t x, y, z, record_base; /* std430 uvec4 */
} rc_sparse_brick_t;
typedef struct rc_sparse_range_s
{
	uint32_t first, count;
} rc_sparse_range_t;

/* Each level has its own non-overlapping physical pool. A brick is 4^3
 * probes, or the whole level when its grid is smaller than 4. Capacities must
 * be nonzero and at most the number of logical bricks in the level. */
rc_sparse_t *RC_SparseCreate (uint32_t grid_side, uint32_t direction_side, float spacing, float first_interval,
	const uint32_t capacities[RC_CASCADE_COUNT]);
void RC_SparseDestroy (rc_sparse_t *state);

/* Start a new request set. The camera snaps in whole-brick increments;
 * overlap retains the same physical slots and world-space probe positions. */
bool RC_SparseBeginFrame (rc_sparse_t *state, const float camera[3]);
/* Request an AABB on all levels with at least one probe of interpolation
 * halo. Lower priority values win; ties are resolved deterministically by
 * camera distance and logical brick index, independent of call order. */
bool RC_SparseMarkAABB (rc_sparse_t *state, const float mins[3], const float maxs[3], uint32_t priority);
/* Resolve requests against per-level limits. Previously retained slots remain
 * intact. Newly assigned or reused ranges are returned by RC_SparseClears;
 * clear raw and merged validity for these ranges before dispatch. */
bool RC_SparseResolve (rc_sparse_t *state);

/* Returned arrays belong to the state and remain valid only until its next
 * begin/resolve. Upload copies into separately fenced frame-local GPU buffers. */
const rc_layout_t *RC_SparseLayout (const rc_sparse_t *state);
const uint32_t *RC_SparseTable (const rc_sparse_t *state, uint32_t *count);
const rc_sparse_brick_t *RC_SparseActive (const rc_sparse_t *state, uint32_t *count);
const rc_sparse_range_t *RC_SparseClears (const rc_sparse_t *state, uint32_t *count);
bool RC_SparseLevelWork (const rc_sparse_t *state, uint32_t level, uint32_t *active_base,
	uint32_t *active_count, uint32_t *records_per_brick, uint32_t *overflow);

/* Schedule actual active-brick records, not the logical grid size. Empty
 * levels are skipped, and cursors are clamped when the sparse set shrinks. */
bool RC_SparseSchedulerNext (rc_scheduler_t *scheduler, const rc_sparse_t *state, uint32_t budget,
	uint32_t *level, uint32_t *first, uint32_t *count);

typedef struct rc_sparse_refresh_work_s
{
	uint32_t level, first, count;
	uint32_t slot, generation; /* opaque reservation identity */
} rc_sparse_refresh_work_t;

/* Upper bound on expedited bricks, independent of the ordinary work queue. */
#define RC_SPARSE_URGENT_MAX 8u

/* Newly assigned bricks are queued automatically, in fine-to-coarse slot
 * order, alongside the normal far-to-near sweep for faster initial lighting.
 * Request a budgeted temporal refresh after an occluder, emitter or light
 * changes. Pass both old and new world bounds, including the influence radius
 * of a light. Only active probe bricks within each level's tracing distance
 * are queued; existing valid radiance stays visible until replaced. Merge the
 * coarser results into finer levels after each dispatched batch. */
bool RC_SparseRequestRefresh (rc_sparse_t *state, const float mins[3], const float maxs[3]);
/* Expedite one queued fine brick near the receiver, constrained to the
 * source's influence. The ordinary queue retains a fair share. */
bool RC_SparsePrioritizeInfluence (rc_sparse_t *state, const float source[3], float max_distance, const float receiver[3]);
/* When a transient disappears, promptly revisit recently expedited bricks in
 * its former influence volume so their stored radiance cannot linger. */
bool RC_SparsePrioritizeExpiredInfluence (rc_sparse_t *state, const float source[3], float max_distance);
/* Reserve one contiguous batch. Submit it through RCVk_Record using level,
 * first and count. At most one reservation is outstanding. A change requested
 * during a partially completed brick causes another full pass of that brick,
 * so a continuous stream of edits cannot indefinitely restart its progress. */
bool RC_SparseRefreshNext (rc_sparse_t *state, uint32_t budget, rc_sparse_refresh_work_t *work);
/* Commit only after successful queue submission. Pass false after recording is
 * abandoned; the exact same work remains pending for a later retry. The
 * current snapshot cannot be rebuilt while a reservation is outstanding. */
bool RC_SparseRefreshCommit (rc_sparse_t *state, const rc_sparse_refresh_work_t *work, bool submitted);
uint32_t RC_SparseRefreshPending (const rc_sparse_t *state);
uint32_t RC_SparseUrgentPending (const rc_sparse_t *state);

#endif
