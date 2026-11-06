/* Conservative CPU broad phase for skipping GPU-instance lighting work. */
#ifndef ECS_GPU_CULL_H
#define ECS_GPU_CULL_H

#include <stdbool.h>
#include <math.h>

/* mins/maxs enclose all model-space AABBs in the block, already translated
 * to world space. A positive result means every per-instance shader test is
 * outside one shared plane, including its floating-point tolerance. */
static inline bool ECSGPULightingBoundsHidden (const float mins[3], const float maxs[3], const float planes[4][4])
{
	for (int axis = 0; axis < 3; ++axis)
		if (!isfinite (mins[axis]) || !isfinite (maxs[axis]) || mins[axis] > maxs[axis]) return false;
	for (int p = 0; p < 4; ++p)
	{
		double upper = 0.0, max_abs_dot = 0.0;
		for (int axis = 0; axis < 3; ++axis)
		{
			const double normal = planes[p][axis];
			const double lo = mins[axis], hi = maxs[axis];
			upper += normal * (normal < 0.0 ? lo : hi);
			max_abs_dot += fabs (normal) * fmax (fabs (lo), fabs (hi));
		}
		const double distance = planes[p][3];
		const double slack = 0.01 + 0.00004 * fmax (1.0, fabs (distance) + max_abs_dot);
		if (upper < distance - slack) return true;
	}
	return false;
}

#endif
