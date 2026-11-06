/* World-space radiance cascade layout shared by the CPU scheduler and GLSL. */
#ifndef QUIVER_RADIANCE_CASCADES_H
#define QUIVER_RADIANCE_CASCADES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RC_CASCADE_COUNT 5u

/* Each structure is 48 bytes and matches RCCascade in radiance_cascades.glsl
 * under both std140 and std430. The first probe is at origin + 0.5 * spacing. */
typedef struct rc_level_s
{
	float origin_spacing[4];
	float interval[4];
	uint32_t layout[4]; /* side of probe grid, side of direction grid, record offset, reserved */
} rc_level_t;

typedef struct rc_layout_s
{
	rc_level_t levels[RC_CASCADE_COUNT];
	uint32_t record_count;
	uint32_t probe_count;
} rc_layout_t;

typedef struct rc_interval_s
{
	float rgb[3];
	float transmittance;
} rc_interval_t;

/* World-unit spacing, clamped to [8,128]. Invalid/nonpositive requests use 32. */
float RC_ReceiverSpacing (float requested);
/* grid_side must be divisible by 16; direction_side is a power of two.
 * Returns false for invalid inputs or overflow and leaves out untouched. */
bool RC_LayoutInit (rc_layout_t *out, uint32_t grid_side, uint32_t direction_side, float spacing, float first_interval);
typedef struct rc_scroll_s
{
	uint32_t changed_mask;
	uint32_t reset_mask; /* first placement or a teleport beyond a grid */
	int32_t delta[RC_CASCADE_COUNT][3]; /* new origin minus old, in probes */
} rc_scroll_t;

/* Snap the grid origins to the camera, reporting the old-to-new cell shift.
 * Zero shifts preserve all records. For changed non-reset levels, copy the old
 * per-level arrays to scratch, dispatch radiance_scroll.comp with this shift,
 * and invalidate just newly exposed slabs. Reset levels must be fully cleared. */
bool RC_LayoutRecenterDetailed (rc_layout_t *layout, const float camera[3], rc_scroll_t *scroll);
/* Convenience wrapper returning only the changed mask. */
uint32_t RC_LayoutRecenter (rc_layout_t *layout, const float camera[3]);
/* Decode a level-local interval index into a world-space probe position and
 * equal-solid-angle direction. Returns false for an invalid index. */
bool RC_DecodeInterval (const rc_level_t *level, uint32_t index, float origin[3], float direction[3]);
/* Associative front-to-back radiance interval composition. */
rc_interval_t RC_Compose (rc_interval_t near_interval, rc_interval_t far_interval);

/* CPU reference for the scroll shader. Source and destination must not
 * overlap, and each has gridSide^3 * directionSide^2 elements. */
typedef struct rc_record_s
{
	rc_interval_t raw, merged;
	float raw_distance, merged_distance;
	uint32_t raw_valid, merged_valid;
} rc_record_t;
bool RC_ScrollRecords (const rc_level_t *level, const int32_t delta[3], const rc_record_t *old_records, rc_record_t *new_records);

/* The scheduler grants bounded contiguous work. A reset begins with the far
 * level so coarse incoming intervals can be merged into finer ones. After the
 * first sweep, levels are refreshed round-robin. */
typedef struct rc_scheduler_s
{
	uint32_t cursor[RC_CASCADE_COUNT];
	uint32_t level;
	bool initial_sweep;
} rc_scheduler_t;
void RC_SchedulerReset (rc_scheduler_t *scheduler);
bool RC_SchedulerNext (rc_scheduler_t *scheduler, const rc_layout_t *layout, uint32_t budget,
	uint32_t *level, uint32_t *first, uint32_t *count);

/* Budget adapts only from available completed-frame GPU samples; missing,
 * non-finite and non-positive measurements are ignored. */
typedef struct rc_budget_s
{
	uint32_t minimum, maximum, current, samples;
	double target_ms, smoothed_ms;
} rc_budget_t;
bool RC_BudgetInit (rc_budget_t *budget, uint32_t minimum, uint32_t maximum, uint32_t initial, double target_ms);
uint32_t RC_BudgetUpdate (rc_budget_t *budget, double completed_gpu_ms);

#endif
