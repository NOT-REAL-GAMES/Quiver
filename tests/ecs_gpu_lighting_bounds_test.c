/* CPU lighting broad-phase must never reject a shader-visible instance. */
#include "../Quake/ecs_gpu_cull.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t state = 0x5a71b82du;
static uint32_t random_bits (void)
{
	state ^= state << 13; state ^= state >> 17; state ^= state << 5;
	return state;
}
static float random_signed (void) { return (float)(int32_t)random_bits () / 2147483648.0f; }

/* Same float operations and rejection tolerance as alias_ecs_cull.comp. */
static bool shader_rejects (const float lo[3], const float hi[3], const float planes[4][4])
{
	for (int p = 0; p < 4; ++p)
	{
		float dot = 0.0f, absolute_dot = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			float n = planes[p][axis], corner = n < 0.0f ? lo[axis] : hi[axis];
			dot += n * corner;
			absolute_dot += fabsf (n) * fabsf (corner);
		}
		float tolerance = 0.00001f * fmaxf (1.0f, fabsf (planes[p][3]) + absolute_dot);
		if (dot < planes[p][3] - tolerance) return true;
	}
	return false;
}

int main (void)
{
	unsigned rejected = 0, retained = 0;
	for (unsigned iteration = 0; iteration < 50000; ++iteration)
	{
		float lo[64][3], hi[64][3], mins[3], maxs[3], planes[4][4] = {{0}};
		float magnitude = iteration % 4 == 0 ? 1e8f : iteration % 4 == 1 ? 1e5f : 1e3f;
		float center[3] = {random_signed () * magnitude, random_signed () * magnitude, random_signed () * magnitude};
		for (int i = 0; i < 64; ++i)
			for (int a = 0; a < 3; ++a)
			{
				float origin = center[a] + random_signed () * 1000.0f;
				lo[i][a] = origin - 16.0f; hi[i][a] = origin + 16.0f;
				if (!i || lo[i][a] < mins[a]) mins[a] = lo[i][a];
				if (!i || hi[i][a] > maxs[a]) maxs[a] = hi[i][a];
			}
		for (int a = 0; a < 3; ++a) planes[0][a] = random_signed ();
		double upper = 0.0;
		for (int a = 0; a < 3; ++a)
			upper += (double)planes[0][a] * (planes[0][a] < 0 ? mins[a] : maxs[a]);
		/* Include distances just either side of the broad-phase threshold. */
		planes[0][3] = (float)(upper + random_signed () * (magnitude * 0.0002 + 5));
		/* Remaining zero planes cannot reject any finite point. */
		if (ECSGPULightingBoundsHidden (mins, maxs, planes))
		{
			++rejected;
			for (int i = 0; i < 64; ++i)
				if (!shader_rejects (lo[i], hi[i], planes))
				{
					fprintf (stderr, "visible instance in rejected block: iteration %u item %d\n", iteration, i);
					return 1;
				}
		}
		else ++retained;
	}
	float invalid[3] = {NAN, 0, 0}, valid[3] = {1, 1, 1}, planes[4][4] = {{0}};
	if (ECSGPULightingBoundsHidden (invalid, valid, planes) || !rejected || !retained)
		return 1;
	printf ("PASS: %u rejected and %u retained blocks; no shader-visible instance rejected\n", rejected, retained);
	return 0;
}
