/* The including compute shader selects software or hardware traversal. */
#define RC_TRACE_WRITEONLY 1
#define RC_INDIRECT_BOUNCE 1
#include "radiance_cascades.glsl"
#include "radiance_scene_trace.glsl"
#include "radiance_lookup.glsl"
#include "radiance_lighting.glsl"

layout (local_size_x = 64) in;
layout (push_constant) uniform RCWork { uint level; uint first; uint count; uint flags; } rcWork;

void main ()
{
	uint invocation = gl_GlobalInvocationID.x;
	if (invocation >= rcWork.count || rcWork.level >= RC_LEVELS) return;
	RCCascade c = rcLevels[rcWork.level];
	uint localIndex = rcWork.first + invocation;
	if (localIndex < rcWork.first) return;
	uvec3 cell; uvec2 bin;
	uint index;
	if (!RC_DecodeWork (c, localIndex, rcWork.flags, cell, bin, index)) return;
	vec3 origin = RC_ProbePosition (c, cell);
	vec3 direction = RC_Direction (bin, c.meta.y);
	RCSceneHit hit;
	if (RC_SceneTrace (origin, direction, c.interval.x, c.interval.y, hit))
	{
		rcRaw[index] = vec4 (max (RC_ShadeHit (hit, direction), vec3 (0.0)), 0.0);
		rcRawDistance[index] = hit.distance;
	}
	else if (rcWork.level + 1u == RC_LEVELS)
	{
		/* The outermost interval terminates on the sky/environment. */
		rcRaw[index] = vec4 ((rcLighting.counts.w & 16u) != 0u ? vec3 (0.0) :
			max (RC_SkyRadiance (direction), vec3 (0.0)), 0.0);
		rcRawDistance[index] = RC_NO_HIT;
	}
	else
	{
		rcRaw[index] = vec4 (0.0, 0.0, 0.0, 1.0);
		rcRawDistance[index] = RC_NO_HIT;
	}
	rcRawValid[index] = 1u;
}
