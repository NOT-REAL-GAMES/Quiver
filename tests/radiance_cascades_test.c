#include "radiance_cascades.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int near (float a, float b) { return fabsf (a - b) < 1e-5f; }

int main (void)
{
	rc_layout_t layout, before;
	assert (sizeof (rc_level_t) == 48);
	assert (offsetof (rc_level_t, interval) == 16);
	assert (offsetof (rc_level_t, layout) == 32);
	assert (RC_LayoutInit (&layout, 32, 4, 32.0f, 64.0f));
	assert (layout.record_count == 1015808u);
	assert (layout.probe_count == 37448u);
	for (uint32_t i = 0; i < RC_CASCADE_COUNT; ++i)
	{
		const rc_level_t *level = &layout.levels[i];
		assert (level->layout[0] == (32u >> i));
		assert (level->layout[1] == (4u << i));
		assert (near (level->origin_spacing[3], (float)(32u << i)));
		assert (near (level->interval[1], (float)(64u << i)));
		assert (near (level->interval[0], i ? layout.levels[i - 1].interval[1] : 0.0f));
	}
	before = layout;
	assert (!RC_LayoutInit (&layout, 31, 4, 32.0f, 64.0f));
	assert (!memcmp (&before, &layout, sizeof (layout)));
	assert (!RC_LayoutInit (&layout, 32, 3, 32.0f, 64.0f));
	assert (!RC_LayoutInit (&layout, 32, 4, NAN, 64.0f));
	assert (!RC_LayoutInit (&layout, UINT32_MAX, 4, 32.0f, 64.0f));
	float camera[3] = {0, 0, 0};
	assert (RC_LayoutRecenter (&layout, camera) == 31u);
	assert (RC_LayoutRecenter (&layout, camera) == 0u);
	assert (near (layout.levels[0].origin_spacing[0], -512.0f));
	camera[0] = 32.0f;
	assert (RC_LayoutRecenter (&layout, camera) == 1u);
	assert (near (layout.levels[0].origin_spacing[0], -480.0f));
	float origin[3], direction[3];
	assert (RC_DecodeInterval (&layout.levels[0], 0, origin, direction));
	assert (near (origin[0], -464.0f) && near (origin[1], -496.0f) && near (origin[2], -496.0f));
	assert (near (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2], 1.0f));
	assert (!RC_DecodeInterval (&layout.levels[0], 32u * 32u * 32u * 16u, origin, direction));
	/* The lowest 2x2 direction grid must cover all six axis-facing surfaces,
	 * while each staggered sample remains inside its azimuth bin. */
	rc_layout_t coarse;
	assert (RC_LayoutInit (&coarse, 32, 2, 32, 64) && RC_LayoutRecenter (&coarse, camera));
	float axis_coverage[6] = {0};
	for (uint32_t ray = 0; ray < 4; ++ray)
	{
		assert (RC_DecodeInterval (&coarse.levels[0], ray, origin, direction));
		for (int axis = 0; axis < 3; ++axis)
		{
			axis_coverage[2 * axis] += fmaxf (direction[axis], 0);
			axis_coverage[2 * axis + 1] += fmaxf (-direction[axis], 0);
		}
		float angle = atan2f (direction[1], direction[0]);
		if (angle < 0) angle += 6.28318530717958647692f;
		assert ((uint32_t)floorf (angle * (2.0f / 6.28318530717958647692f)) == ray % 2u);
	}
	for (int i = 0; i < 6; ++i) assert (axis_coverage[i] > .5f);
	rc_interval_t front = {{2, 3, 4}, 0.25f}, back = {{4, 8, 12}, 0.5f};
	rc_interval_t merged = RC_Compose (front, back);
	assert (near (merged.rgb[0], 3) && near (merged.rgb[1], 5) && near (merged.rgb[2], 7));
	assert (near (merged.transmittance, 0.125f));
	front.transmittance = 0.0f;
	merged = RC_Compose (front, back);
	assert (near (merged.rgb[0], front.rgb[0]) && near (merged.transmittance, 0));
	rc_scheduler_t scheduler;
	RC_SchedulerReset (&scheduler);
	uint64_t visited = 0;
	uint32_t previous_level = 4, level, first, count;
	while (scheduler.initial_sweep)
	{
		assert (RC_SchedulerNext (&scheduler, &layout, 8192, &level, &first, &count));
		assert (level <= previous_level && count > 0 && count <= 8192);
		previous_level = level;
		visited += count;
	}
	assert (visited == layout.record_count);
	assert (RC_SchedulerNext (&scheduler, &layout, 7, &level, &first, &count));
	assert (level == 4 && first == 0 && count == 7);
	assert (!RC_SchedulerNext (&scheduler, &layout, 0, &level, &first, &count));
	rc_budget_t budget;
	assert (RC_BudgetInit (&budget, 1024, 32768, 8192, 2.0));
	assert (!RC_BudgetInit (&budget, 0, 32768, 8192, 2.0));
	assert (RC_BudgetUpdate (&budget, NAN) == 8192);
	assert (RC_BudgetUpdate (&budget, 8.0) == 4096);
	for (int i = 0; i < 20; ++i) RC_BudgetUpdate (&budget, 0.1);
	assert (budget.current >= budget.minimum && budget.current <= budget.maximum);
	uint32_t recovered = budget.current;
	for (int i = 0; i < 5; ++i) RC_BudgetUpdate (&budget, 0.1);
	assert (budget.current > recovered);

	/* A one-cell scroll retains overlap instead of destroying every sample. */
	rc_layout_t scroll_layout;
	rc_scroll_t scroll;
	assert (RC_LayoutInit (&scroll_layout, 16, 1, 32.0f, 64.0f));
	float scroll_camera[3] = {0, 0, 0};
	assert (RC_LayoutRecenterDetailed (&scroll_layout, scroll_camera, &scroll));
	assert (scroll.changed_mask == 31u && scroll.reset_mask == 31u);
	const uint32_t cells = 16u * 16u * 16u;
	rc_record_t *old = calloc (cells, sizeof (*old));
	rc_record_t *next = calloc (cells, sizeof (*next));
	assert (old && next);
	for (uint32_t i = 0; i < cells; ++i)
	{
		old[i].raw.rgb[0] = (float)i;
		old[i].merged.rgb[1] = (float)(i + 1u);
		old[i].raw_valid = old[i].merged_valid = 1u;
	}
	scroll_camera[0] = 32.0f;
	assert (RC_LayoutRecenterDetailed (&scroll_layout, scroll_camera, &scroll));
	assert (scroll.changed_mask == 1u && scroll.reset_mask == 0u && scroll.delta[0][0] == 1);
	assert (RC_ScrollRecords (&scroll_layout.levels[0], scroll.delta[0], old, next));
	for (uint32_t z = 0; z < 16; ++z)
		for (uint32_t y = 0; y < 16; ++y)
			for (uint32_t x = 0; x < 16; ++x)
			{
				uint32_t index = (z * 16 + y) * 16 + x;
				if (x == 15) assert (!next[index].raw_valid && !next[index].merged_valid);
				else
				{
					assert (next[index].raw_valid && next[index].merged_valid);
					assert (near (next[index].raw.rgb[0], old[index + 1].raw.rgb[0]));
					assert (near (next[index].merged.rgb[1], old[index + 1].merged.rgb[1]));
				}
			}
	assert (!RC_ScrollRecords (&scroll_layout.levels[0], scroll.delta[0], old, old));
	scroll_camera[0] = -1024.0f;
	assert (RC_LayoutRecenterDetailed (&scroll_layout, scroll_camera, &scroll));
	assert ((scroll.reset_mask & 1u) != 0);
	free (old);
	free (next);
	puts ("PASS: 3D cascade layout, intervals, composition, and bounded scheduling");
	return 0;
}
