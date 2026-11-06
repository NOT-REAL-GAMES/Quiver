#include "radiance_cascades.h"

#include <math.h>
#include <limits.h>
#include <string.h>

#define RC_PI 3.14159265358979323846f

float RC_ReceiverSpacing (float requested)
{
	if (!isfinite (requested) || requested <= 0.0f) return 32.0f;
	return fminf (128.0f, fmaxf (8.0f, requested));
}

bool RC_LayoutInit (rc_layout_t *out, uint32_t grid_side, uint32_t direction_side, float spacing, float first_interval)
{
	rc_layout_t result;
	uint64_t records = 0, probes = 0;
	if (!out || grid_side < 16 || (grid_side & 15u) || !direction_side || (direction_side & (direction_side - 1u)) ||
		!isfinite (spacing) || spacing <= 0.0f || !isfinite (first_interval) || first_interval <= 0.0f)
		return false;
	memset (&result, 0, sizeof (result));
	for (uint32_t i = 0; i < RC_CASCADE_COUNT; ++i)
	{
		uint64_t side = grid_side >> i;
		uint64_t directions = (uint64_t)direction_side << i;
		/* Bound each multiplication before performing it. */
		if (!side || side > 1625u || directions > 65535u) return false;
		uint64_t level_probes = side * side * side;
		uint64_t level_records = level_probes * directions * directions;
		if (level_records > UINT32_MAX || records + level_records > UINT32_MAX || probes + level_probes > UINT32_MAX)
			return false;
		rc_level_t *level = &result.levels[i];
		level->origin_spacing[0] = level->origin_spacing[1] = level->origin_spacing[2] = NAN;
		level->origin_spacing[3] = ldexpf (spacing, (int)i);
		level->interval[0] = i ? ldexpf (first_interval, (int)i - 1) : 0.0f;
		level->interval[1] = ldexpf (first_interval, (int)i);
		if (!isfinite (level->origin_spacing[3]) || !isfinite (level->interval[1])) return false;
		level->layout[0] = (uint32_t)side;
		level->layout[1] = (uint32_t)directions;
		level->layout[2] = (uint32_t)records;
		records += level_records;
		probes += level_probes;
	}
	result.record_count = (uint32_t)records;
	result.probe_count = (uint32_t)probes;
	*out = result;
	return true;
}

bool RC_LayoutRecenterDetailed (rc_layout_t *layout, const float camera[3], rc_scroll_t *scroll)
{
	rc_layout_t next;
	rc_scroll_t result;
	if (!layout || !camera || !scroll || !isfinite (camera[0]) || !isfinite (camera[1]) || !isfinite (camera[2])) return false;
	next = *layout;
	memset (&result, 0, sizeof (result));
	for (uint32_t i = 0; i < RC_CASCADE_COUNT; ++i)
	{
		rc_level_t *level = &next.levels[i];
		const float spacing = level->origin_spacing[3];
		if (!isfinite (spacing) || spacing <= 0.0f || !level->layout[0]) return false;
		for (int axis = 0; axis < 3; ++axis)
		{
			const double cell = floor ((double)camera[axis] / (double)spacing) - (double)(level->layout[0] / 2u);
			const double origin = cell * (double)spacing;
			if (!isfinite (origin) || fabs (origin) > 3.402823466e38) return false;
			const float old = level->origin_spacing[axis], next_origin = (float)origin;
			if (!isfinite (old))
			{
				result.changed_mask |= 1u << i;
				result.reset_mask |= 1u << i;
			}
			else if (old != next_origin)
			{
				result.changed_mask |= 1u << i;
				const double cells = ((double)next_origin - (double)old) / spacing;
				const double rounded = round (cells);
				if (!isfinite (cells) || fabs (cells - rounded) > 0.001 || fabs (rounded) >= level->layout[0] ||
					rounded > INT32_MAX || rounded < INT32_MIN)
					result.reset_mask |= 1u << i;
				else
					result.delta[i][axis] = (int32_t)rounded;
			}
			level->origin_spacing[axis] = (float)origin;
		}
	}
	*layout = next;
	*scroll = result;
	return true;
}

uint32_t RC_LayoutRecenter (rc_layout_t *layout, const float camera[3])
{
	rc_scroll_t scroll;
	return RC_LayoutRecenterDetailed (layout, camera, &scroll) ? scroll.changed_mask : 0u;
}

bool RC_DecodeInterval (const rc_level_t *level, uint32_t index, float origin[3], float direction[3])
{
	if (!level || !origin || !direction) return false;
	const uint32_t grid_side = level->layout[0], direction_side = level->layout[1];
	const uint64_t direction_count = (uint64_t)direction_side * direction_side;
	const uint64_t count = (uint64_t)grid_side * grid_side * grid_side * direction_count;
	if (!grid_side || !direction_side || index >= count || !isfinite (level->origin_spacing[0])) return false;
	const uint32_t ray = (uint32_t)(index % direction_count);
	uint32_t probe = (uint32_t)(index / direction_count);
	for (int axis = 0; axis < 3; ++axis)
	{
		const uint32_t cell = probe % grid_side;
		probe /= grid_side;
		origin[axis] = level->origin_spacing[axis] + (cell + 0.5f) * level->origin_spacing[3];
	}
	const uint32_t row = ray / direction_side;
	const float azimuth = direction_side > 1 ? ((row & 1u) ? 0.25f : 0.75f) : 0.5f;
	const float u = ((ray % direction_side) + azimuth) / direction_side;
	const float v = ((ray / direction_side) + 0.5f) / direction_side;
	const float z = 1.0f - 2.0f * v;
	const float radius = sqrtf (fmaxf (0.0f, 1.0f - z * z));
	const float phi = 2.0f * RC_PI * u;
	direction[0] = radius * cosf (phi);
	direction[1] = radius * sinf (phi);
	direction[2] = z;
	return true;
}

rc_interval_t RC_Compose (rc_interval_t near_interval, rc_interval_t far_interval)
{
	rc_interval_t result;
	for (int i = 0; i < 3; ++i)
		result.rgb[i] = near_interval.rgb[i] + near_interval.transmittance * far_interval.rgb[i];
	result.transmittance = near_interval.transmittance * far_interval.transmittance;
	return result;
}

bool RC_ScrollRecords (const rc_level_t *level, const int32_t delta[3], const rc_record_t *old_records, rc_record_t *new_records)
{
	if (!level || !delta || !old_records || !new_records) return false;
	const uint32_t side = level->layout[0], directions = level->layout[1];
	if (!side || !directions || side > 1625u || directions > 65535u) return false;
	const uint64_t rays = (uint64_t)directions * directions;
	const uint64_t records = (uint64_t)side * side * side * rays;
	if (records > SIZE_MAX / sizeof (rc_record_t)) return false;
	const size_t bytes = (size_t)records * sizeof (rc_record_t);
	const uintptr_t a = (uintptr_t)old_records, b = (uintptr_t)new_records;
	if ((a <= b && b - a < bytes) || (b < a && a - b < bytes)) return false;
	const size_t probe_bytes = (size_t)rays * sizeof (rc_record_t);
	for (uint32_t z = 0; z < side; ++z)
		for (uint32_t y = 0; y < side; ++y)
			for (uint32_t x = 0; x < side; ++x)
			{
				rc_record_t *destination = new_records + (((uint64_t)z * side + y) * side + x) * rays;
				const int64_t ox = (int64_t)x + delta[0], oy = (int64_t)y + delta[1], oz = (int64_t)z + delta[2];
				if (ox < 0 || oy < 0 || oz < 0 || ox >= side || oy >= side || oz >= side)
					memset (destination, 0, probe_bytes);
				else
					memcpy (destination, old_records + (((uint64_t)oz * side + (uint64_t)oy) * side + (uint64_t)ox) * rays, probe_bytes);
			}
	return true;
}

void RC_SchedulerReset (rc_scheduler_t *scheduler)
{
	if (!scheduler) return;
	memset (scheduler, 0, sizeof (*scheduler));
	scheduler->level = RC_CASCADE_COUNT - 1u;
	scheduler->initial_sweep = true;
}

bool RC_SchedulerNext (rc_scheduler_t *scheduler, const rc_layout_t *layout, uint32_t budget,
	uint32_t *level_out, uint32_t *first_out, uint32_t *count_out)
{
	if (!scheduler || !layout || !level_out || !first_out || !count_out || !budget || scheduler->level >= RC_CASCADE_COUNT) return false;
	const uint32_t level = scheduler->level;
	const uint32_t offset = layout->levels[level].layout[2];
	const uint32_t end = level + 1u < RC_CASCADE_COUNT ? layout->levels[level + 1u].layout[2] : layout->record_count;
	if (end <= offset || scheduler->cursor[level] >= end - offset) return false;
	const uint32_t remaining = end - offset - scheduler->cursor[level];
	const uint32_t count = remaining < budget ? remaining : budget;
	*level_out = level;
	*first_out = scheduler->cursor[level];
	*count_out = count;
	scheduler->cursor[level] += count;
	if (scheduler->cursor[level] == end - offset)
	{
		scheduler->cursor[level] = 0;
		if (!level) scheduler->initial_sweep = false;
		scheduler->level = level ? level - 1u : RC_CASCADE_COUNT - 1u;
	}
	return true;
}

bool RC_BudgetInit (rc_budget_t *budget, uint32_t minimum, uint32_t maximum, uint32_t initial, double target_ms)
{
	if (!budget || !minimum || maximum < minimum || initial < minimum || initial > maximum || !isfinite (target_ms) || target_ms <= 0.0)
		return false;
	*budget = (rc_budget_t){.minimum = minimum, .maximum = maximum, .current = initial, .target_ms = target_ms};
	return true;
}

uint32_t RC_BudgetUpdate (rc_budget_t *budget, double completed_gpu_ms)
{
	if (!budget) return 0;
	if (!isfinite (completed_gpu_ms) || completed_gpu_ms <= 0.0) return budget->current;
	if (!budget->samples) budget->smoothed_ms = completed_gpu_ms;
	else budget->smoothed_ms = budget->smoothed_ms * 0.75 + completed_gpu_ms * 0.25;
	if (budget->samples != UINT32_MAX) ++budget->samples;
	/* Hysteresis prevents oscillation near target. Decrease promptly if the
	 * pass overruns; recover slowly to avoid oscillation and sudden stalls. */
	if (budget->smoothed_ms > budget->target_ms * 1.05)
	{
		double ratio = budget->target_ms / budget->smoothed_ms;
		if (ratio < 0.5) ratio = 0.5;
		uint32_t next = (uint32_t)floor ((double)budget->current * ratio);
		if (next >= budget->current && budget->current > budget->minimum) next = budget->current - 1u;
		budget->current = next < budget->minimum ? budget->minimum : next;
	}
	else if (budget->smoothed_ms < budget->target_ms * 0.90 && budget->current < budget->maximum)
	{
		uint32_t increase = budget->current / 8u;
		if (!increase) increase = 1u;
		budget->current += increase > budget->maximum - budget->current ? budget->maximum - budget->current : increase;
	}
	return budget->current;
}
