/*
Copyright (C) 2024 Quiver Contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// kd_world.c -- Kd-tree collision detection

#include "quakedef.h"
#include "kd_model.h"
#include "kd_svo.h"
#include "jolt_physics.h"

// Epsilon values for collision
#define KD_DIST_EPSILON 0.03125f    // Small epsilon for trace calculations
#define KD_PUSHBACK     0.125f      // Larger pushback to prevent sticking (1/8 unit)

/*
==================
KD_PointContents

Returns the contents (CONTENTS_EMPTY, CONTENTS_SOLID, etc.) at a point.

For kd-tree maps compiled from .map files representing hollow rooms:
- Points INSIDE the model bounds (interior space) are EMPTY
- Points OUTSIDE the model bounds are SOLID (can't go there)

This is a simplified approximation that works for axis-aligned box rooms.
==================
*/
int KD_PointContents (kdmodel_t *model, vec3_t p)
{
	if (!model)
		return CONTENTS_EMPTY;

	// Simple bounds-based check for interior/exterior
	// For a hollow room, the model bounds represent the interior walkable area
	// Points outside the bounds are SOLID (blocked)
	if (p[0] < model->mins[0] || p[0] > model->maxs[0] ||
	    p[1] < model->mins[1] || p[1] > model->maxs[1] ||
	    p[2] < model->mins[2] || p[2] > model->maxs[2])
	{
		return CONTENTS_SOLID;
	}

	// Point is inside the model bounds - it's in the playable/empty area
	return CONTENTS_EMPTY;
}

/*
==================
KD_TraceLine

Trace a line through the kd-tree world using bounds-based collision.
For a hollow room map, the model bounds define the playable interior.
A trace from inside trying to exit hits the walls.
==================
*/
qboolean KD_TraceLine (kdmodel_t *model, vec3_t start, vec3_t end, trace_t *trace)
{
	// Initialize trace
	memset (trace, 0, sizeof(trace_t));
	trace->fraction = 1.0f;
	trace->allsolid = false;
	trace->startsolid = false;
	trace->inopen = true;
	VectorCopy (end, trace->endpos);

	if (!model)
		return true;

	// Check if start is outside bounds (in solid)
	qboolean start_inside =
		start[0] >= model->mins[0] && start[0] <= model->maxs[0] &&
		start[1] >= model->mins[1] && start[1] <= model->maxs[1] &&
		start[2] >= model->mins[2] && start[2] <= model->maxs[2];

	// Check if end is outside bounds
	qboolean end_inside =
		end[0] >= model->mins[0] && end[0] <= model->maxs[0] &&
		end[1] >= model->mins[1] && end[1] <= model->maxs[1] &&
		end[2] >= model->mins[2] && end[2] <= model->maxs[2];

	if (!start_inside)
	{
		// Started in solid
		trace->startsolid = true;
		trace->allsolid = !end_inside;
		trace->inopen = false;
		trace->fraction = 0;
		VectorCopy(start, trace->endpos);
		return false;
	}

	if (end_inside)
	{
		// Both start and end inside - clear path
		return true;
	}

	// Start inside, end outside - find where we hit the wall
	// Ray-box intersection from inside
	vec3_t dir;
	VectorSubtract(end, start, dir);

	float best_frac = 1.0f;
	int best_axis = -1;
	int best_side = 0;

	for (int axis = 0; axis < 3; axis++)
	{
		if (fabs(dir[axis]) < 0.001f)
			continue;

		float frac;
		int side;

		// Check min plane (would exit through min)
		if (dir[axis] < 0)
		{
			frac = (model->mins[axis] - start[axis]) / dir[axis];
			side = -1;
		}
		// Check max plane (would exit through max)
		else
		{
			frac = (model->maxs[axis] - start[axis]) / dir[axis];
			side = 1;
		}

		if (frac >= 0 && frac < best_frac)
		{
			// Verify this hit point is actually on a face of the box
			vec3_t hit;
			hit[0] = start[0] + frac * dir[0];
			hit[1] = start[1] + frac * dir[1];
			hit[2] = start[2] + frac * dir[2];

			// Check if hit point is within the other two axis bounds
			int a1 = (axis + 1) % 3;
			int a2 = (axis + 2) % 3;
			if (hit[a1] >= model->mins[a1] - 0.1f && hit[a1] <= model->maxs[a1] + 0.1f &&
			    hit[a2] >= model->mins[a2] - 0.1f && hit[a2] <= model->maxs[a2] + 0.1f)
			{
				best_frac = frac;
				best_axis = axis;
				best_side = side;
			}
		}
	}

	if (best_axis >= 0 && best_frac < 1.0f)
	{
		// Hit a wall
		trace->fraction = best_frac;
		trace->endpos[0] = start[0] + best_frac * dir[0];
		trace->endpos[1] = start[1] + best_frac * dir[1];
		trace->endpos[2] = start[2] + best_frac * dir[2];

		// Normal points inward (from solid back into empty space)
		trace->plane.normal[0] = 0;
		trace->plane.normal[1] = 0;
		trace->plane.normal[2] = 0;
		trace->plane.normal[best_axis] = (float)(-best_side);  // Negate to point inward
		trace->plane.dist = (best_side > 0) ? model->maxs[best_axis] : model->mins[best_axis];

		return false;
	}

	return true;
}

/*
==================
KD_TraceBox

Trace a box through the kd-tree world using hull expansion.
For bounds-based collision, we shrink the interior bounds by the hull size
so the box center stays the required distance from walls.
==================
*/
qboolean KD_TraceBox (kdmodel_t *model, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace)
{
	// Initialize trace
	memset (trace, 0, sizeof(trace_t));
	trace->fraction = 1.0f;
	trace->allsolid = false;
	trace->startsolid = false;
	trace->inopen = true;
	VectorCopy (end, trace->endpos);

	if (!model)
		return true;

	// Hull expansion: shrink the valid interior bounds by the hull size
	// This is equivalent to expanding the walls by the hull extents
	vec3_t expanded_mins, expanded_maxs;
	expanded_mins[0] = model->mins[0] - mins[0];  // mins[0] is negative, so this adds
	expanded_mins[1] = model->mins[1] - mins[1];
	expanded_mins[2] = model->mins[2] - mins[2];
	expanded_maxs[0] = model->maxs[0] - maxs[0];  // maxs[0] is positive, so this subtracts
	expanded_maxs[1] = model->maxs[1] - maxs[1];
	expanded_maxs[2] = model->maxs[2] - maxs[2];

	// Check if start is outside expanded bounds (in solid)
	qboolean start_inside =
		start[0] >= expanded_mins[0] && start[0] <= expanded_maxs[0] &&
		start[1] >= expanded_mins[1] && start[1] <= expanded_maxs[1] &&
		start[2] >= expanded_mins[2] && start[2] <= expanded_maxs[2];

	// Check if end is outside expanded bounds
	qboolean end_inside =
		end[0] >= expanded_mins[0] && end[0] <= expanded_maxs[0] &&
		end[1] >= expanded_mins[1] && end[1] <= expanded_maxs[1] &&
		end[2] >= expanded_mins[2] && end[2] <= expanded_maxs[2];

	if (!start_inside)
	{
		// Started in solid
		trace->startsolid = true;
		trace->allsolid = !end_inside;
		trace->inopen = false;
		trace->fraction = 0;
		VectorCopy(start, trace->endpos);
		return false;
	}

	if (end_inside)
	{
		// Both start and end inside - clear path
		return true;
	}

	// Start inside, end outside - find where we hit the wall
	vec3_t dir;
	VectorSubtract(end, start, dir);

	float best_frac = 1.0f;
	int best_axis = -1;
	int best_side = 0;

	for (int axis = 0; axis < 3; axis++)
	{
		if (fabs(dir[axis]) < 0.001f)
			continue;

		float frac;
		int side;

		// Check min plane (would exit through min)
		if (dir[axis] < 0)
		{
			frac = (expanded_mins[axis] - start[axis]) / dir[axis];
			side = -1;
		}
		// Check max plane (would exit through max)
		else
		{
			frac = (expanded_maxs[axis] - start[axis]) / dir[axis];
			side = 1;
		}

		if (frac >= 0 && frac < best_frac)
		{
			// Verify this hit point is actually on a face of the box
			vec3_t hit;
			hit[0] = start[0] + frac * dir[0];
			hit[1] = start[1] + frac * dir[1];
			hit[2] = start[2] + frac * dir[2];

			// Check if hit point is within the other two axis bounds
			int a1 = (axis + 1) % 3;
			int a2 = (axis + 2) % 3;
			if (hit[a1] >= expanded_mins[a1] - 0.1f && hit[a1] <= expanded_maxs[a1] + 0.1f &&
			    hit[a2] >= expanded_mins[a2] - 0.1f && hit[a2] <= expanded_maxs[a2] + 0.1f)
			{
				best_frac = frac;
				best_axis = axis;
				best_side = side;
			}
		}
	}

	if (best_axis >= 0 && best_frac < 1.0f)
	{
		// Hit a wall
		trace->fraction = best_frac;
		trace->endpos[0] = start[0] + best_frac * dir[0];
		trace->endpos[1] = start[1] + best_frac * dir[1];
		trace->endpos[2] = start[2] + best_frac * dir[2];

		// Normal points inward (from solid back into empty space)
		trace->plane.normal[0] = 0;
		trace->plane.normal[1] = 0;
		trace->plane.normal[2] = 0;
		trace->plane.normal[best_axis] = (float)(-best_side);
		trace->plane.dist = (best_side > 0) ? expanded_maxs[best_axis] : expanded_mins[best_axis];

		return false;
	}

	return true;
}

/*
==================
KD_ClipMoveToWorld

Clip a move against the kd-tree world geometry.
Returns true if the kd-tree trace was closer than the existing trace.

Uses SVO collision when available (more accurate for complex geometry),
falls back to bounds-based collision otherwise.

NOTE: We do NOT handle sliding here - Quake's SV_FlyMove already handles
sliding via ClipVelocity. We just need to report accurate collision info.
==================
*/
qboolean KD_ClipMoveToWorld (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace)
{
	// Try Jolt collision first (most accurate - proper physics engine)
	jolt_world_t *jolt = Jolt_GetActiveWorld();
	if (jolt)
	{
		trace_t jolt_trace;
		Jolt_TraceBox(jolt, start, mins, maxs, end, &jolt_trace);

		// If Jolt trace hit something closer, use that result
		if (jolt_trace.fraction < trace->fraction && jolt_trace.fraction < 1.0f)
		{
			*trace = jolt_trace;

			// Push the endpoint away from the surface along the normal
			trace->endpos[0] += trace->plane.normal[0] * KD_PUSHBACK;
			trace->endpos[1] += trace->plane.normal[1] * KD_PUSHBACK;
			trace->endpos[2] += trace->plane.normal[2] * KD_PUSHBACK;

			return true;
		}

		// Check if we started in solid in Jolt
		if (jolt_trace.startsolid)
		{
			trace->startsolid = true;
			if (jolt_trace.allsolid)
				trace->allsolid = true;
		}

		return false;
	}

	// Fall back to SVO collision (less accurate due to voxelization)
	svo_t *svo = SVO_GetCollisionTree();
	if (svo)
	{
		trace_t svo_trace;
		SVO_TraceBox(svo, start, mins, maxs, end, &svo_trace);

		// If SVO trace hit something closer, use that result
		if (svo_trace.fraction < trace->fraction && svo_trace.fraction < 1.0f)
		{
			*trace = svo_trace;

			// Push the endpoint away from the surface along the normal
			trace->endpos[0] += trace->plane.normal[0] * KD_PUSHBACK;
			trace->endpos[1] += trace->plane.normal[1] * KD_PUSHBACK;
			trace->endpos[2] += trace->plane.normal[2] * KD_PUSHBACK;

			return true;
		}

		// Check if we started in solid in SVO
		if (svo_trace.startsolid)
		{
			trace->startsolid = true;
			if (svo_trace.allsolid)
				trace->allsolid = true;
		}

		return false;
	}

	// Fallback to bounds-based collision
	kdmodel_t *model = KD_GetDebugModel();
	if (!model)
		return false;

	// Only apply kd-tree collision if movement is near the kd-tree bounds
	// This prevents the kd-tree from interfering with normal BSP collision
	vec3_t move_mins, move_maxs;
	for (int i = 0; i < 3; i++)
	{
		if (start[i] < end[i])
		{
			move_mins[i] = start[i] + mins[i];
			move_maxs[i] = end[i] + maxs[i];
		}
		else
		{
			move_mins[i] = end[i] + mins[i];
			move_maxs[i] = start[i] + maxs[i];
		}
	}

	// Check if movement box intersects kd-tree bounds (with some padding)
	float pad = 32.0f;
	if (move_maxs[0] < model->mins[0] - pad || move_mins[0] > model->maxs[0] + pad ||
	    move_maxs[1] < model->mins[1] - pad || move_mins[1] > model->maxs[1] + pad ||
	    move_maxs[2] < model->mins[2] - pad || move_mins[2] > model->maxs[2] + pad)
	{
		return false; // Movement is outside kd-tree bounds, skip
	}

	trace_t kd_trace;
	KD_TraceBox (model, start, mins, maxs, end, &kd_trace);

	// If kd-tree trace hit something closer, use that result
	if (kd_trace.fraction < trace->fraction && kd_trace.fraction < 1.0f)
	{
		// Copy the kd-tree trace result
		*trace = kd_trace;

		// Push the endpoint away from the surface along the normal
		// Use a larger pushback to prevent getting stuck when already at boundary
		trace->endpos[0] += trace->plane.normal[0] * KD_PUSHBACK;
		trace->endpos[1] += trace->plane.normal[1] * KD_PUSHBACK;
		trace->endpos[2] += trace->plane.normal[2] * KD_PUSHBACK;

		return true;
	}

	// Check if we started in solid in kd-tree
	if (kd_trace.startsolid)
	{
		trace->startsolid = true;
		if (kd_trace.allsolid)
			trace->allsolid = true;
	}

	return false;
}

/*
==================
KD_TraceTest_f

Console command to test kd-tree collision
==================
*/
static void KD_TraceTest_f (void)
{
	kdmodel_t *model = KD_GetDebugModel();

	if (!model)
	{
		Con_Printf ("No kd-tree model loaded. Use kd_load first.\n");
		return;
	}

	// Get player position
	vec3_t pos;
	VectorCopy (cl.viewent.origin, pos);

	// Test point contents
	int contents = KD_PointContents (model, pos);
	const char *contents_str = "UNKNOWN";
	switch (contents)
	{
		case CONTENTS_EMPTY: contents_str = "EMPTY"; break;
		case CONTENTS_SOLID: contents_str = "SOLID"; break;
		case CONTENTS_WATER: contents_str = "WATER"; break;
		case CONTENTS_SLIME: contents_str = "SLIME"; break;
		case CONTENTS_LAVA:  contents_str = "LAVA"; break;
		case CONTENTS_SKY:   contents_str = "SKY"; break;
	}

	Con_Printf ("Position: (%.1f, %.1f, %.1f)\n", pos[0], pos[1], pos[2]);
	Con_Printf ("KD_PointContents: %s (%d)\n", contents_str, contents);

	// Test trace downward
	vec3_t end;
	VectorCopy (pos, end);
	end[2] -= 1000; // Trace down

	trace_t trace;
	KD_TraceLine (model, pos, end, &trace);

	Con_Printf ("Trace down: fraction=%.3f, startsolid=%d, allsolid=%d\n",
	            trace.fraction, trace.startsolid, trace.allsolid);
	if (trace.fraction < 1.0f)
	{
		Con_Printf ("  Hit at: (%.1f, %.1f, %.1f)\n",
		            trace.endpos[0], trace.endpos[1], trace.endpos[2]);
		Con_Printf ("  Normal: (%.2f, %.2f, %.2f)\n",
		            trace.plane.normal[0], trace.plane.normal[1], trace.plane.normal[2]);
	}
}

/*
==================
KD_WorldInit

Initialize kd-tree collision system
==================
*/
void KD_WorldInit (void)
{
	Cmd_AddCommand ("kd_trace", KD_TraceTest_f);
}
