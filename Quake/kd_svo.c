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
// kd_svo.c -- Sparse Voxel Octree for collision detection

#include "quakedef.h"
#include "kd_model.h"

//=============================================================================
// SVO Data Structures
//=============================================================================

// Voxel size at maximum depth (in Quake units)
// 1 unit = roughly 1 inch in Quake scale
// 2 units gives better collision precision for thin walls
#define SVO_MIN_VOXEL_SIZE  2.0f
#define SVO_MAX_DEPTH       12      // 2^12 = 4096 voxels per axis max

// SVO node types
#define SVO_NODE_EMPTY      0       // Entirely empty (air)
#define SVO_NODE_SOLID      1       // Entirely solid (blocked)
#define SVO_NODE_MIXED      2       // Has children (partially solid)

// SVO node - 32 bytes
typedef struct svo_node_s
{
	uint8_t     type;               // SVO_NODE_EMPTY, SOLID, or MIXED
	uint8_t     depth;              // Depth in tree (0 = root)
	uint8_t     pad[2];
	int32_t     children[8];        // Child indices (-1 = none), ordered by octant
	                                // Octant: bit0=+X, bit1=+Y, bit2=+Z
} svo_node_t;

// SVO structure
typedef struct svo_s
{
	svo_node_t  *nodes;
	int         num_nodes;
	int         max_nodes;

	vec3_t      mins;               // World bounds
	vec3_t      maxs;
	float       root_size;          // Size of root node
	int         max_depth;          // Maximum subdivision depth
} svo_t;

// Global SVO for collision
static svo_t *collision_svo = NULL;

//=============================================================================
// SVO Building
//=============================================================================

/*
==================
SVO_AllocNode

Allocate a new SVO node
==================
*/
static int SVO_AllocNode (svo_t *svo)
{
	if (svo->num_nodes >= svo->max_nodes)
	{
		// Grow array
		svo->max_nodes = svo->max_nodes ? svo->max_nodes * 2 : 1024;
		svo->nodes = (svo_node_t *)realloc(svo->nodes, svo->max_nodes * sizeof(svo_node_t));
	}

	int idx = svo->num_nodes++;
	memset(&svo->nodes[idx], 0, sizeof(svo_node_t));
	for (int i = 0; i < 8; i++)
		svo->nodes[idx].children[i] = -1;

	return idx;
}

/*
==================
SVO_PointInBrush

Test if a point is inside a brush (defined by planes)
==================
*/
static qboolean SVO_PointInBrush (vec3_t point, mkdbrush_t *brush)
{
	for (int i = 0; i < brush->num_sides; i++)
	{
		mplane_t *plane = brush->sides[i].plane;
		float dist = DotProduct(point, plane->normal) - plane->dist;

		// If point is in front of any plane, it's outside the brush
		// (brush planes point outward)
		if (dist > 0.1f)
			return false;
	}
	return true;
}

/*
==================
SVO_BoxIntersectsBrush

Test if an AABB intersects a brush using separating axis theorem.
This handles thin brushes (floor/ceiling slabs) correctly.
==================
*/
static qboolean SVO_BoxIntersectsBrush (vec3_t box_mins, vec3_t box_maxs, mkdbrush_t *brush)
{
	// First do quick AABB test
	if (box_maxs[0] < brush->mins[0] || box_mins[0] > brush->maxs[0] ||
	    box_maxs[1] < brush->mins[1] || box_mins[1] > brush->maxs[1] ||
	    box_maxs[2] < brush->mins[2] || box_mins[2] > brush->maxs[2])
	{
		return false;
	}

	// Separating Axis Theorem: test each brush plane as a potential separating axis
	// If the box is entirely in front of ANY plane, they don't intersect
	for (int i = 0; i < brush->num_sides; i++)
	{
		mplane_t *plane = brush->sides[i].plane;

		// Find the box corner that is most "behind" this plane (closest to brush interior)
		vec3_t support;
		support[0] = (plane->normal[0] < 0) ? box_maxs[0] : box_mins[0];
		support[1] = (plane->normal[1] < 0) ? box_maxs[1] : box_mins[1];
		support[2] = (plane->normal[2] < 0) ? box_maxs[2] : box_mins[2];

		float dist = DotProduct(support, plane->normal) - plane->dist;

		// If the closest point is in front of the plane, no intersection
		if (dist > 0.1f)
			return false;
	}

	// If we didn't find a separating axis, they intersect
	return true;
}

/*
==================
SVO_BoxFullyInBrush

Test if an AABB is fully inside a brush
==================
*/
static qboolean SVO_BoxFullyInBrush (vec3_t box_mins, vec3_t box_maxs, mkdbrush_t *brush)
{
	// All 8 corners must be inside
	vec3_t corners[8];
	corners[0][0] = box_mins[0]; corners[0][1] = box_mins[1]; corners[0][2] = box_mins[2];
	corners[1][0] = box_maxs[0]; corners[1][1] = box_mins[1]; corners[1][2] = box_mins[2];
	corners[2][0] = box_mins[0]; corners[2][1] = box_maxs[1]; corners[2][2] = box_mins[2];
	corners[3][0] = box_maxs[0]; corners[3][1] = box_maxs[1]; corners[3][2] = box_mins[2];
	corners[4][0] = box_mins[0]; corners[4][1] = box_mins[1]; corners[4][2] = box_maxs[2];
	corners[5][0] = box_maxs[0]; corners[5][1] = box_mins[1]; corners[5][2] = box_maxs[2];
	corners[6][0] = box_mins[0]; corners[6][1] = box_maxs[1]; corners[6][2] = box_maxs[2];
	corners[7][0] = box_maxs[0]; corners[7][1] = box_maxs[1]; corners[7][2] = box_maxs[2];

	for (int i = 0; i < 8; i++)
	{
		if (!SVO_PointInBrush(corners[i], brush))
			return false;
	}
	return true;
}

/*
==================
SVO_ClassifyVoxel

Classify a voxel against all brushes
Returns: SVO_NODE_EMPTY, SVO_NODE_SOLID, or SVO_NODE_MIXED
==================
*/
static int SVO_ClassifyVoxel (vec3_t voxel_mins, vec3_t voxel_maxs,
                              mkdbrush_t *brushes, int num_brushes)
{
	qboolean any_intersect = false;
	qboolean fully_inside = false;

	for (int i = 0; i < num_brushes; i++)
	{
		mkdbrush_t *brush = &brushes[i];

		if (SVO_BoxFullyInBrush(voxel_mins, voxel_maxs, brush))
		{
			fully_inside = true;
			break;
		}

		if (SVO_BoxIntersectsBrush(voxel_mins, voxel_maxs, brush))
		{
			any_intersect = true;
		}
	}

	if (fully_inside)
		return SVO_NODE_SOLID;
	else if (any_intersect)
		return SVO_NODE_MIXED;
	else
		return SVO_NODE_EMPTY;
}

/*
==================
SVO_BuildRecursive

Recursively build the SVO
==================
*/
static void SVO_BuildRecursive (svo_t *svo, int node_idx, vec3_t node_mins, vec3_t node_maxs,
                                mkdbrush_t *brushes, int num_brushes, int depth)
{
	svo_node_t *node = &svo->nodes[node_idx];
	node->depth = depth;

	// Classify this voxel
	int classification = SVO_ClassifyVoxel(node_mins, node_maxs, brushes, num_brushes);

	// If fully empty or solid, we're done
	if (classification == SVO_NODE_EMPTY)
	{
		node->type = SVO_NODE_EMPTY;
		return;
	}

	if (classification == SVO_NODE_SOLID)
	{
		node->type = SVO_NODE_SOLID;
		return;
	}

	// Mixed - check if we should subdivide
	float voxel_size = node_maxs[0] - node_mins[0];

	if (depth >= svo->max_depth || voxel_size <= SVO_MIN_VOXEL_SIZE)
	{
		// At max depth or min size - treat as solid (conservative)
		node->type = SVO_NODE_SOLID;
		return;
	}

	// Subdivide into 8 children
	node->type = SVO_NODE_MIXED;

	vec3_t center;
	center[0] = (node_mins[0] + node_maxs[0]) * 0.5f;
	center[1] = (node_mins[1] + node_maxs[1]) * 0.5f;
	center[2] = (node_mins[2] + node_maxs[2]) * 0.5f;

	for (int octant = 0; octant < 8; octant++)
	{
		vec3_t child_mins, child_maxs;

		// Determine child bounds based on octant
		// Octant bits: 0=+X, 1=+Y, 2=+Z
		child_mins[0] = (octant & 1) ? center[0] : node_mins[0];
		child_maxs[0] = (octant & 1) ? node_maxs[0] : center[0];
		child_mins[1] = (octant & 2) ? center[1] : node_mins[1];
		child_maxs[1] = (octant & 2) ? node_maxs[1] : center[1];
		child_mins[2] = (octant & 4) ? center[2] : node_mins[2];
		child_maxs[2] = (octant & 4) ? node_maxs[2] : center[2];

		// Quick check if this octant intersects any brush
		qboolean octant_relevant = false;
		for (int i = 0; i < num_brushes; i++)
		{
			if (child_maxs[0] >= brushes[i].mins[0] && child_mins[0] <= brushes[i].maxs[0] &&
			    child_maxs[1] >= brushes[i].mins[1] && child_mins[1] <= brushes[i].maxs[1] &&
			    child_maxs[2] >= brushes[i].mins[2] && child_mins[2] <= brushes[i].maxs[2])
			{
				octant_relevant = true;
				break;
			}
		}

		if (!octant_relevant)
		{
			// This octant doesn't intersect any brushes - leave child as -1 (empty)
			continue;
		}

		// Allocate and build child
		int child_idx = SVO_AllocNode(svo);
		node = &svo->nodes[node_idx];  // Revalidate pointer after realloc
		node->children[octant] = child_idx;

		SVO_BuildRecursive(svo, child_idx, child_mins, child_maxs, brushes, num_brushes, depth + 1);
	}
}

/*
==================
SVO_Build

Build an SVO from brush data
==================
*/
svo_t *SVO_Build (mkdbrush_t *brushes, int num_brushes, vec3_t world_mins, vec3_t world_maxs)
{
	if (num_brushes == 0)
		return NULL;

	svo_t *svo = (svo_t *)calloc(1, sizeof(svo_t));

	// Compute root size (power of 2 that encompasses world)
	float world_size = 0;
	for (int i = 0; i < 3; i++)
	{
		float axis_size = world_maxs[i] - world_mins[i];
		if (axis_size > world_size)
			world_size = axis_size;
	}

	// Round up to power of 2
	svo->root_size = 1.0f;
	while (svo->root_size < world_size)
		svo->root_size *= 2.0f;

	// Center the SVO on the world
	vec3_t world_center;
	world_center[0] = (world_mins[0] + world_maxs[0]) * 0.5f;
	world_center[1] = (world_mins[1] + world_maxs[1]) * 0.5f;
	world_center[2] = (world_mins[2] + world_maxs[2]) * 0.5f;

	float half_size = svo->root_size * 0.5f;
	svo->mins[0] = world_center[0] - half_size;
	svo->mins[1] = world_center[1] - half_size;
	svo->mins[2] = world_center[2] - half_size;
	svo->maxs[0] = world_center[0] + half_size;
	svo->maxs[1] = world_center[1] + half_size;
	svo->maxs[2] = world_center[2] + half_size;

	// Calculate max depth based on min voxel size
	svo->max_depth = 0;
	float size = svo->root_size;
	while (size > SVO_MIN_VOXEL_SIZE && svo->max_depth < SVO_MAX_DEPTH)
	{
		size *= 0.5f;
		svo->max_depth++;
	}

	// Allocate root node
	SVO_AllocNode(svo);

	// Build the tree
	Con_Printf("Building SVO (root=%.0f, max_depth=%d, min_voxel=%.1f)...\n",
	           svo->root_size, svo->max_depth, SVO_MIN_VOXEL_SIZE);

	SVO_BuildRecursive(svo, 0, svo->mins, svo->maxs, brushes, num_brushes, 0);

	// Count node types for statistics
	int empty_count = 0, solid_count = 0, mixed_count = 0;
	for (int i = 0; i < svo->num_nodes; i++)
	{
		switch (svo->nodes[i].type)
		{
			case SVO_NODE_EMPTY: empty_count++; break;
			case SVO_NODE_SOLID: solid_count++; break;
			case SVO_NODE_MIXED: mixed_count++; break;
		}
	}

	Con_Printf("SVO built: %d nodes (empty=%d, solid=%d, mixed=%d)\n",
	           svo->num_nodes, empty_count, solid_count, mixed_count);

	return svo;
}

/*
==================
SVO_Free
==================
*/
void SVO_Free (svo_t *svo)
{
	if (svo)
	{
		if (svo->nodes)
			free(svo->nodes);
		free(svo);
	}
}

//=============================================================================
// SVO Queries
//=============================================================================

/*
==================
SVO_PointContents

Test if a point is solid or empty
==================
*/
int SVO_PointContents (svo_t *svo, vec3_t point)
{
	if (!svo || svo->num_nodes == 0)
		return CONTENTS_EMPTY;

	// Check if point is within SVO bounds
	if (point[0] < svo->mins[0] || point[0] > svo->maxs[0] ||
	    point[1] < svo->mins[1] || point[1] > svo->maxs[1] ||
	    point[2] < svo->mins[2] || point[2] > svo->maxs[2])
	{
		return CONTENTS_SOLID;  // Outside SVO = solid
	}

	// Traverse the tree
	int node_idx = 0;
	vec3_t node_mins, node_maxs;
	VectorCopy(svo->mins, node_mins);
	VectorCopy(svo->maxs, node_maxs);

	while (1)
	{
		svo_node_t *node = &svo->nodes[node_idx];

		if (node->type == SVO_NODE_EMPTY)
			return CONTENTS_EMPTY;

		if (node->type == SVO_NODE_SOLID)
			return CONTENTS_SOLID;

		// Mixed node - descend to appropriate child
		vec3_t center;
		center[0] = (node_mins[0] + node_maxs[0]) * 0.5f;
		center[1] = (node_mins[1] + node_maxs[1]) * 0.5f;
		center[2] = (node_mins[2] + node_maxs[2]) * 0.5f;

		// Determine octant
		int octant = 0;
		if (point[0] >= center[0]) octant |= 1;
		if (point[1] >= center[1]) octant |= 2;
		if (point[2] >= center[2]) octant |= 4;

		int child_idx = node->children[octant];

		if (child_idx < 0)
		{
			// No child in this octant = empty
			return CONTENTS_EMPTY;
		}

		// Update bounds and descend
		if (octant & 1) node_mins[0] = center[0]; else node_maxs[0] = center[0];
		if (octant & 2) node_mins[1] = center[1]; else node_maxs[1] = center[1];
		if (octant & 4) node_mins[2] = center[2]; else node_maxs[2] = center[2];

		node_idx = child_idx;
	}
}

/*
==================
SVO_TraceLine

Trace a ray through the SVO, finding the first solid hit
Uses DDA (Digital Differential Analyzer) for efficient traversal
==================
*/
qboolean SVO_TraceLine (svo_t *svo, vec3_t start, vec3_t end, trace_t *trace)
{
	memset(trace, 0, sizeof(trace_t));
	trace->fraction = 1.0f;
	trace->allsolid = false;
	trace->startsolid = false;
	trace->inopen = true;
	VectorCopy(end, trace->endpos);

	if (!svo || svo->num_nodes == 0)
		return true;

	// Check if start is in solid
	if (SVO_PointContents(svo, start) == CONTENTS_SOLID)
	{
		trace->startsolid = true;
		trace->allsolid = (SVO_PointContents(svo, end) == CONTENTS_SOLID);
		trace->fraction = 0;
		VectorCopy(start, trace->endpos);
		return false;
	}

	// Ray direction
	vec3_t dir;
	VectorSubtract(end, start, dir);
	float ray_length = VectorLength(dir);

	if (ray_length < 0.001f)
		return true;  // Zero-length ray

	// Normalize
	vec3_t dir_norm;
	VectorScale(dir, 1.0f / ray_length, dir_norm);

	// Step through the ray in small increments
	// This is a simplified approach - a proper implementation would use
	// hierarchical DDA traversal, but this works for moderate distances
	float step_size = SVO_MIN_VOXEL_SIZE * 0.5f;
	int num_steps = (int)(ray_length / step_size) + 1;

	for (int i = 1; i <= num_steps; i++)
	{
		float t = (float)i / (float)num_steps;
		vec3_t test_point;
		test_point[0] = start[0] + t * dir[0];
		test_point[1] = start[1] + t * dir[1];
		test_point[2] = start[2] + t * dir[2];

		if (SVO_PointContents(svo, test_point) == CONTENTS_SOLID)
		{
			// Found solid - back up slightly to find exact hit
			trace->fraction = t - (step_size / ray_length);
			if (trace->fraction < 0) trace->fraction = 0;

			trace->endpos[0] = start[0] + trace->fraction * dir[0];
			trace->endpos[1] = start[1] + trace->fraction * dir[1];
			trace->endpos[2] = start[2] + trace->fraction * dir[2];

			// Estimate normal by checking neighboring voxels
			float probe = step_size;
			trace->plane.normal[0] = 0;
			trace->plane.normal[1] = 0;
			trace->plane.normal[2] = 0;

			vec3_t probe_point;
			for (int axis = 0; axis < 3; axis++)
			{
				// Check +axis
				VectorCopy(test_point, probe_point);
				probe_point[axis] += probe;
				if (SVO_PointContents(svo, probe_point) == CONTENTS_EMPTY)
				{
					trace->plane.normal[axis] = 1.0f;
					break;
				}

				// Check -axis
				probe_point[axis] = test_point[axis] - probe;
				if (SVO_PointContents(svo, probe_point) == CONTENTS_EMPTY)
				{
					trace->plane.normal[axis] = -1.0f;
					break;
				}
			}

			// If no clear normal found, use ray direction
			if (trace->plane.normal[0] == 0 && trace->plane.normal[1] == 0 && trace->plane.normal[2] == 0)
			{
				trace->plane.normal[0] = -dir_norm[0];
				trace->plane.normal[1] = -dir_norm[1];
				trace->plane.normal[2] = -dir_norm[2];
			}

			trace->plane.dist = DotProduct(trace->endpos, trace->plane.normal);

			return false;
		}
	}

	return true;
}

/*
==================
SVO_TestExpandedPoint

Test if a point with hull expansion is in solid.
Tests points on a grid covering the player's bounding box surface,
with extra density to catch thin walls.

IMPORTANT: We expand the test box by HULL_EXPANSION to simulate hull expansion.
This catches walls BEFORE the player's actual bounding box overlaps them.
==================
*/
// Hull expansion for startsolid check. This defines how close the player can
// be to a surface before they're considered "in solid" (triggering startsolid).
// Use 4.0 units - this worked best in testing.
#define HULL_EXPANSION 4.0f

static qboolean SVO_TestExpandedPoint (svo_t *svo, vec3_t pos, vec3_t mins, vec3_t maxs,
                                        int *out_axis, int *out_side)
{
	vec3_t test;

	// Expand the test bounds to catch walls earlier
	vec3_t exp_mins, exp_maxs;
	exp_mins[0] = mins[0] - HULL_EXPANSION;
	exp_mins[1] = mins[1] - HULL_EXPANSION;
	exp_mins[2] = mins[2] - HULL_EXPANSION;
	exp_maxs[0] = maxs[0] + HULL_EXPANSION;
	exp_maxs[1] = maxs[1] + HULL_EXPANSION;
	exp_maxs[2] = maxs[2] + HULL_EXPANSION;

	// Test all 8 corners of the EXPANDED bounding box
	for (int corner = 0; corner < 8; corner++)
	{
		test[0] = pos[0] + ((corner & 1) ? exp_maxs[0] : exp_mins[0]);
		test[1] = pos[1] + ((corner & 2) ? exp_maxs[1] : exp_mins[1]);
		test[2] = pos[2] + ((corner & 4) ? exp_maxs[2] : exp_mins[2]);

		if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
		{
			if (out_axis && out_side)
			{
				// Determine which face we hit by probing
				*out_axis = -1;
				*out_side = 0;
				for (int axis = 0; axis < 3; axis++)
				{
					vec3_t probe;
					VectorCopy(test, probe);
					probe[axis] += 4.0f;
					if (SVO_PointContents(svo, probe) == CONTENTS_EMPTY)
					{
						*out_axis = axis;
						*out_side = 1;
						break;
					}
					probe[axis] = test[axis] - 4.0f;
					if (SVO_PointContents(svo, probe) == CONTENTS_EMPTY)
					{
						*out_axis = axis;
						*out_side = -1;
						break;
					}
				}
			}
			return true;
		}
	}

	// Test face centers - critical for detecting walls when approaching head-on
	// Using expanded bounds for earlier detection
	// Front/back faces (X axis)
	test[1] = pos[1];  // center Y
	test[2] = pos[2];  // center Z
	test[0] = pos[0] + exp_maxs[0];  // +X face
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 0;
		if (out_side) *out_side = 1;
		return true;
	}
	test[0] = pos[0] + exp_mins[0];  // -X face
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 0;
		if (out_side) *out_side = -1;
		return true;
	}

	// Left/right faces (Y axis)
	test[0] = pos[0];  // center X
	test[2] = pos[2];  // center Z
	test[1] = pos[1] + exp_maxs[1];  // +Y face
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 1;
		if (out_side) *out_side = 1;
		return true;
	}
	test[1] = pos[1] + exp_mins[1];  // -Y face
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 1;
		if (out_side) *out_side = -1;
		return true;
	}

	// Top/bottom faces (Z axis)
	test[0] = pos[0];  // center X
	test[1] = pos[1];  // center Y
	test[2] = pos[2] + exp_maxs[2];  // +Z (top)
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 2;
		if (out_side) *out_side = 1;
		return true;
	}
	test[2] = pos[2] + exp_mins[2];  // -Z (bottom)
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 2;
		if (out_side) *out_side = -1;
		return true;
	}

	// Test 4 bottom edge midpoints (most important for walking into wall edges)
	// These catch diagonal collision where corners pass on either side of a wall edge
	// Edge midpoints are at the CENTER of each edge (X or Y at 0, not at mins/maxs)

	// Bottom-front edge midpoint (center X, min Y)
	test[0] = pos[0]; test[1] = pos[1] + exp_mins[1]; test[2] = pos[2] + exp_mins[2];
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 1;
		if (out_side) *out_side = -1;
		return true;
	}

	// Bottom-back edge midpoint (center X, max Y)
	test[0] = pos[0]; test[1] = pos[1] + exp_maxs[1]; test[2] = pos[2] + exp_mins[2];
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 1;
		if (out_side) *out_side = 1;
		return true;
	}

	// Bottom-left edge midpoint (min X, center Y)
	test[0] = pos[0] + exp_mins[0]; test[1] = pos[1]; test[2] = pos[2] + exp_mins[2];
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 0;
		if (out_side) *out_side = -1;
		return true;
	}

	// Bottom-right edge midpoint (max X, center Y)
	test[0] = pos[0] + exp_maxs[0]; test[1] = pos[1]; test[2] = pos[2] + exp_mins[2];
	if (SVO_PointContents(svo, test) == CONTENTS_SOLID)
	{
		if (out_axis) *out_axis = 0;
		if (out_side) *out_side = 1;
		return true;
	}

	return false;
}

/*
==================
SVO_SweptAABB_Overlaps

Check if a swept AABB (moving from p1 to p2) overlaps with a static AABB at any point.
Used for octree traversal decisions - determines if we should recurse into a child.
Returns true if the swept AABB touches or passes through the expanded box.
==================
*/
static qboolean SVO_SweptAABB_Overlaps (vec3_t p1, vec3_t p2, vec3_t hull_mins, vec3_t hull_maxs,
                                         vec3_t box_mins, vec3_t box_maxs)
{
	// Expand the static box by the player's hull (Minkowski sum)
	vec3_t exp_mins, exp_maxs;
	exp_mins[0] = box_mins[0] - hull_maxs[0];
	exp_mins[1] = box_mins[1] - hull_maxs[1];
	exp_mins[2] = box_mins[2] - hull_maxs[2];
	exp_maxs[0] = box_maxs[0] - hull_mins[0];
	exp_maxs[1] = box_maxs[1] - hull_mins[1];
	exp_maxs[2] = box_maxs[2] - hull_mins[2];

	// Ray direction
	vec3_t dir;
	dir[0] = p2[0] - p1[0];
	dir[1] = p2[1] - p1[1];
	dir[2] = p2[2] - p1[2];

	// Slab intersection test (Kay-Kajiya algorithm)
	float t_enter = -999999.0f;
	float t_exit = 999999.0f;

	for (int axis = 0; axis < 3; axis++)
	{
		if (fabsf(dir[axis]) < 0.0001f)
		{
			// Ray is parallel to this axis - check if inside
			if (p1[axis] < exp_mins[axis] || p1[axis] > exp_maxs[axis])
				return false;  // No overlap
		}
		else
		{
			float inv_dir = 1.0f / dir[axis];
			float t1 = (exp_mins[axis] - p1[axis]) * inv_dir;
			float t2 = (exp_maxs[axis] - p1[axis]) * inv_dir;

			if (t1 > t2)
			{
				float tmp = t1; t1 = t2; t2 = tmp;
			}

			if (t1 > t_enter)
				t_enter = t1;
			if (t2 < t_exit)
				t_exit = t2;

			if (t_enter > t_exit)
				return false;  // No overlap
		}
	}

	// Check if any part of [t_enter, t_exit] intersects [0, 1]
	// Overlap occurs if t_exit >= 0 AND t_enter <= 1
	return (t_exit >= 0.0f && t_enter <= 1.0f);
}

/*
==================
SVO_SweptAABB_vs_AABB

Test if a swept AABB (moving from p1 to p2) hits a static AABB.
Returns the fraction [0..1] where collision occurs, or >1 if no collision.
Also returns the axis and side of the collision.

This uses the Minkowski sum approach: expand the static AABB by the moving
AABB's half-extents, then trace a ray from start to end.
==================
*/

static float SVO_SweptAABB_vs_AABB (vec3_t p1, vec3_t p2, vec3_t hull_mins, vec3_t hull_maxs,
                                    vec3_t box_mins, vec3_t box_maxs,
                                    int *out_axis, int *out_side)
{
	// Expand the static box by the player's hull (Minkowski sum)
	// We trace the player's CENTER through an expanded version of the solid box.
	// Minkowski sum: solid_box ⊕ (-player_hull)
	// For AABB Minkowski sum: exp = solid ⊕ (-hull)
	//   exp_mins = solid_mins - hull_maxs
	//   exp_maxs = solid_maxs - hull_mins
	//
	// Example: floor top at Z=-64, player hull_mins.z=-24 (feet 24 below origin)
	//   exp_maxs.z = -64 - (-24) = -40
	//   So player origin stops at Z=-40, feet at Z=-64 (on floor)
	vec3_t exp_mins, exp_maxs;
	exp_mins[0] = box_mins[0] - hull_maxs[0];
	exp_mins[1] = box_mins[1] - hull_maxs[1];
	exp_mins[2] = box_mins[2] - hull_maxs[2];
	exp_maxs[0] = box_maxs[0] - hull_mins[0];
	exp_maxs[1] = box_maxs[1] - hull_mins[1];
	exp_maxs[2] = box_maxs[2] - hull_mins[2];

	// Ray direction
	vec3_t dir;
	dir[0] = p2[0] - p1[0];
	dir[1] = p2[1] - p1[1];
	dir[2] = p2[2] - p1[2];

	// Slab intersection test (Kay-Kajiya algorithm)
	float t_enter = -999999.0f;
	float t_exit = 999999.0f;
	int enter_axis = -1;
	int enter_side = 0;

	for (int axis = 0; axis < 3; axis++)
	{
		if (fabsf(dir[axis]) < 0.0001f)
		{
			// Ray is parallel to this axis - check if inside
			if (p1[axis] < exp_mins[axis] || p1[axis] > exp_maxs[axis])
				return 2.0f;  // No collision
		}
		else
		{
			float inv_dir = 1.0f / dir[axis];
			float t1 = (exp_mins[axis] - p1[axis]) * inv_dir;  // Time to hit min face
			float t2 = (exp_maxs[axis] - p1[axis]) * inv_dir;  // Time to hit max face

			// The normal points OUTWARD from the box face we hit.
			// If we hit the min face, normal points negative (-1)
			// If we hit the max face, normal points positive (+1)
			int side_at_t1 = -1;  // Hitting min face -> normal points -
			int side_at_t2 = 1;   // Hitting max face -> normal points +

			// Ensure t1 < t2 (t1 is entry time, t2 is exit time)
			if (t1 > t2)
			{
				float tmp = t1; t1 = t2; t2 = tmp;
				int tmp_s = side_at_t1; side_at_t1 = side_at_t2; side_at_t2 = tmp_s;
			}

			if (t1 > t_enter)
			{
				t_enter = t1;
				enter_axis = axis;
				enter_side = side_at_t1;
			}
			if (t2 < t_exit)
				t_exit = t2;

			if (t_enter > t_exit)
				return 2.0f;  // No collision
		}
	}

	// Check if collision is within [0,1] range
	// t_exit < 0 means the box is entirely behind us - no collision.
	// t_enter > 1 means the collision is beyond our movement - no collision.
	if (t_enter > 1.0f || t_exit < 0.0f)
		return 2.0f;

	// If t_enter < 0, we started inside the expanded box.
	// Only report collision if we're moving DEEPER into the overlap.
	// If we're moving out or parallel, let the movement proceed.
	if (t_enter < 0.0f)
	{
		// Find which axis has the smallest overlap (closest to exit)
		float min_overlap = 999999.0f;
		int overlap_axis = -1;
		int overlap_side = 0;

		for (int axis = 0; axis < 3; axis++)
		{
			float dist_to_min = p1[axis] - exp_mins[axis];  // How far inside from min
			float dist_to_max = exp_maxs[axis] - p1[axis];  // How far inside from max

			// Check min face (we're inside if dist_to_min > 0)
			if (dist_to_min > 0 && dist_to_min < min_overlap)
			{
				min_overlap = dist_to_min;
				overlap_axis = axis;
				overlap_side = -1;  // Push toward negative (away from min face)
			}
			// Check max face (we're inside if dist_to_max > 0)
			if (dist_to_max > 0 && dist_to_max < min_overlap)
			{
				min_overlap = dist_to_max;
				overlap_axis = axis;
				overlap_side = 1;  // Push toward positive (away from max face)
			}
		}

		// Only block if we're moving deeper into the solid on this axis
		if (overlap_axis >= 0)
		{
			float move_on_axis = dir[overlap_axis];
			// overlap_side tells us which way to push out
			// If overlap_side = -1, solid is toward negative, so moving negative is bad
			// If overlap_side = 1, solid is toward positive, so moving positive is bad
			qboolean moving_into_solid = (overlap_side < 0 && move_on_axis < -0.001f) ||
			                             (overlap_side > 0 && move_on_axis > 0.001f);

			if (moving_into_solid)
			{
				*out_axis = overlap_axis;
				*out_side = overlap_side;
				return 0.0f;  // Block movement into solid
			}
		}
		// Not moving into solid, allow movement
		return 2.0f;
	}

	// Kay-Kajiya gives us a valid collision - return it.
	// The physics system (SV_FlyMove) will handle sliding along surfaces.

	*out_axis = enter_axis;
	*out_side = enter_side;
	return t_enter;
}

/*
==================
SVO_RecursiveTrace

Recursively trace through the SVO, finding the first solid hit.
Similar to Q1BSP_RecursiveHullTrace but for octrees.
==================
*/
typedef struct {
	svo_t *svo;
	vec3_t start;
	vec3_t end;
	vec3_t dir;
	vec3_t hull_mins;
	vec3_t hull_maxs;
	float best_frac;
	int best_axis;
	int best_side;
} svo_trace_ctx_t;

static void SVO_RecursiveTrace (svo_trace_ctx_t *ctx, int node_idx,
                                 vec3_t node_mins, vec3_t node_maxs,
                                 float p1f, float p2f)
{
	// Cull: if we already found a closer hit, skip this node
	if (p1f >= ctx->best_frac)
		return;

	svo_node_t *node = &ctx->svo->nodes[node_idx];

	// Debug: show what node types we visit at root level
	static int root_visit_debug = 0;
	qboolean in_test_area = (ctx->start[0] >= -300 && ctx->start[0] <= 300 &&
	                         ctx->start[1] >= -300 && ctx->start[1] <= 300 &&
	                         ctx->start[2] >= -100 && ctx->start[2] <= 200);
	if (root_visit_debug < 10 && node_idx == 0 && in_test_area)
	{
		root_visit_debug++;
		const char *type_str = (node->type == SVO_NODE_EMPTY) ? "EMPTY" :
		                       (node->type == SVO_NODE_SOLID) ? "SOLID" : "MIXED";
		Con_Printf("ROOT visit: type=%s bounds=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)\n",
		           type_str, node_mins[0], node_mins[1], node_mins[2],
		           node_maxs[0], node_maxs[1], node_maxs[2]);
	}

	// If this node is entirely empty, no collision
	if (node->type == SVO_NODE_EMPTY)
		return;

	// If this node is entirely solid, find where we enter it
	if (node->type == SVO_NODE_SOLID)
	{
		int axis = -1, side = 0;
		float frac = SVO_SweptAABB_vs_AABB(ctx->start, ctx->end,
		                                    ctx->hull_mins, ctx->hull_maxs,
		                                    node_mins, node_maxs,
		                                    &axis, &side);

		if (frac < ctx->best_frac)
		{
			// Debug: show which voxel we're hitting
			static int hit_debug_count = 0;
			qboolean in_test_area = (ctx->start[0] >= -300 && ctx->start[0] <= 300 &&
			                         ctx->start[1] >= -300 && ctx->start[1] <= 300 &&
			                         ctx->start[2] >= -100 && ctx->start[2] <= 200);
			if (hit_debug_count < 50 && in_test_area && frac < 1.0f)
			{
				hit_debug_count++;
				Con_Printf("  HIT voxel z=(%.1f,%.1f) frac=%.3f ax=%d sd=%d depth=%d\n",
				           node_mins[2], node_maxs[2], frac, axis, side, node->depth);
			}

			ctx->best_frac = frac;
			ctx->best_axis = axis;
			ctx->best_side = side;
		}
		return;
	}

	// Mixed node - need to check children
	vec3_t center;
	center[0] = (node_mins[0] + node_maxs[0]) * 0.5f;
	center[1] = (node_mins[1] + node_maxs[1]) * 0.5f;
	center[2] = (node_mins[2] + node_maxs[2]) * 0.5f;

	// For proper collision, we need to visit children that the swept AABB
	// might intersect. Test all 8 children against the swept AABB.
	// Use SVO_SweptAABB_Overlaps which correctly handles the case where we
	// start inside a child's bounds (t_enter < 0).
	for (int octant = 0; octant < 8; octant++)
	{
		int child_idx = node->children[octant];
		if (child_idx < 0)
			continue;

		// Calculate child bounds
		vec3_t child_mins, child_maxs;
		child_mins[0] = (octant & 1) ? center[0] : node_mins[0];
		child_mins[1] = (octant & 2) ? center[1] : node_mins[1];
		child_mins[2] = (octant & 4) ? center[2] : node_mins[2];
		child_maxs[0] = (octant & 1) ? node_maxs[0] : center[0];
		child_maxs[1] = (octant & 2) ? node_maxs[1] : center[1];
		child_maxs[2] = (octant & 4) ? node_maxs[2] : center[2];

		// Check if the swept AABB overlaps this child at any point along the path
		// This is different from collision detection - we want to recurse into
		// children we're already inside of, not just ones we enter.
		if (SVO_SweptAABB_Overlaps(ctx->start, ctx->end,
		                           ctx->hull_mins, ctx->hull_maxs,
		                           child_mins, child_maxs))
		{
			SVO_RecursiveTrace(ctx, child_idx, child_mins, child_maxs, p1f, p2f);
		}
	}
}

/*
==================
SVO_TraceBox

Trace a box through the SVO using predictive ray-AABB collision (Kay-Kajiya).
Uses SVO_TestExpandedPoint for startsolid check, then SVO_RecursiveTrace for
predictive collision detection.
==================
*/
qboolean SVO_TraceBox (svo_t *svo, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace)
{
	memset(trace, 0, sizeof(trace_t));
	trace->fraction = 1.0f;
	trace->allsolid = false;
	trace->startsolid = false;
	trace->inopen = true;
	VectorCopy(end, trace->endpos);

	if (!svo || svo->num_nodes == 0)
		return true;

	// Check if the trace is relevant to this SVO's bounds
	// If neither start nor end is within the SVO's expanded bounds, skip
	// Expand bounds by hull size to catch traces that might clip the edges
	vec3_t exp_mins, exp_maxs;
	exp_mins[0] = svo->mins[0] + mins[0] - 1.0f;
	exp_mins[1] = svo->mins[1] + mins[1] - 1.0f;
	exp_mins[2] = svo->mins[2] + mins[2] - 1.0f;
	exp_maxs[0] = svo->maxs[0] + maxs[0] + 1.0f;
	exp_maxs[1] = svo->maxs[1] + maxs[1] + 1.0f;
	exp_maxs[2] = svo->maxs[2] + maxs[2] + 1.0f;

	qboolean start_in = (start[0] >= exp_mins[0] && start[0] <= exp_maxs[0] &&
	                     start[1] >= exp_mins[1] && start[1] <= exp_maxs[1] &&
	                     start[2] >= exp_mins[2] && start[2] <= exp_maxs[2]);
	qboolean end_in = (end[0] >= exp_mins[0] && end[0] <= exp_maxs[0] &&
	                   end[1] >= exp_mins[1] && end[1] <= exp_maxs[1] &&
	                   end[2] >= exp_mins[2] && end[2] <= exp_maxs[2]);

	if (!start_in && !end_in)
	{
		// Trace is completely outside SVO bounds - no collision from SVO
		return true;
	}

	// Check if start is in solid using expanded point test
	if (SVO_TestExpandedPoint(svo, start, mins, maxs, NULL, NULL))
	{
		trace->startsolid = true;
		trace->allsolid = SVO_TestExpandedPoint(svo, end, mins, maxs, NULL, NULL);
		trace->fraction = 0;
		VectorCopy(start, trace->endpos);
		return false;
	}

	// Use predictive ray-AABB collision through the SVO
	svo_trace_ctx_t ctx;
	ctx.svo = svo;
	VectorCopy(start, ctx.start);
	VectorCopy(end, ctx.end);
	VectorSubtract(end, start, ctx.dir);
	VectorCopy(mins, ctx.hull_mins);
	VectorCopy(maxs, ctx.hull_maxs);
	ctx.best_frac = 1.0f;
	ctx.best_axis = -1;
	ctx.best_side = 0;

	// Debug: log ALL traces within test_box area
	static int debug_count = 0;
	static int svo_state_debug = 0;
	qboolean in_test_area = (start[0] >= -300 && start[0] <= 300 &&
	                         start[1] >= -300 && start[1] <= 300 &&
	                         start[2] >= -100 && start[2] <= 200);

	// Show SVO state once at the beginning
	if (svo_state_debug < 3 && in_test_area)
	{
		svo_state_debug++;
		Con_Printf("SVO STATE: root_type=%d num_nodes=%d bounds=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)\n",
		           svo->nodes[0].type, svo->num_nodes,
		           svo->mins[0], svo->mins[1], svo->mins[2],
		           svo->maxs[0], svo->maxs[1], svo->maxs[2]);
	}

	// Trace through the octree
	SVO_RecursiveTrace(&ctx, 0, svo->mins, svo->maxs, 0.0f, 1.0f);

	if (debug_count < 10000 && in_test_area)
	{
		debug_count++;
		Con_Printf("SVO: s=(%.0f,%.0f,%.0f) e=(%.0f,%.0f,%.0f) frac=%.3f ax=%d sd=%d\n",
			start[0], start[1], start[2], end[0], end[1], end[2],
			ctx.best_frac, ctx.best_axis, ctx.best_side);
	}

	// Process results
	if (ctx.best_frac < 1.0f)
	{
		// Hit something!
		// First calculate where we actually hit
		vec3_t hit_pos;
		hit_pos[0] = start[0] + ctx.best_frac * ctx.dir[0];
		hit_pos[1] = start[1] + ctx.best_frac * ctx.dir[1];
		hit_pos[2] = start[2] + ctx.best_frac * ctx.dir[2];

		// Back up along the hit axis. Use 4.0 units which worked best in testing.
		float svo_epsilon = 4.0f;

		// Calculate backed-up end position
		trace->endpos[0] = hit_pos[0];
		trace->endpos[1] = hit_pos[1];
		trace->endpos[2] = hit_pos[2];

		// Only back up along the collision axis (the one we hit)
		if (ctx.best_axis >= 0)
		{
			trace->endpos[ctx.best_axis] -= (float)ctx.best_side * svo_epsilon;
		}

		// Calculate fraction based on actual end position
		float move_len = VectorLength(ctx.dir);
		if (move_len > 0.001f)
		{
			vec3_t actual_move;
			actual_move[0] = trace->endpos[0] - start[0];
			actual_move[1] = trace->endpos[1] - start[1];
			actual_move[2] = trace->endpos[2] - start[2];
			float actual_len = VectorLength(actual_move);
			trace->fraction = actual_len / move_len;
			if (trace->fraction < 0.0f)
				trace->fraction = 0.0f;
			if (trace->fraction > 1.0f)
				trace->fraction = 1.0f;
		}
		else
		{
			trace->fraction = 0.0f;
		}

		// Set up collision plane normal
		trace->plane.normal[0] = 0;
		trace->plane.normal[1] = 0;
		trace->plane.normal[2] = 0;

		if (ctx.best_axis >= 0)
		{
			trace->plane.normal[ctx.best_axis] = (float)ctx.best_side;
		}
		else
		{
			vec3_t dir_norm;
			VectorCopy(ctx.dir, dir_norm);
			VectorNormalize(dir_norm);
			trace->plane.normal[0] = -dir_norm[0];
			trace->plane.normal[1] = -dir_norm[1];
			trace->plane.normal[2] = -dir_norm[2];
		}

		trace->plane.dist = DotProduct(trace->endpos, trace->plane.normal);

		return false;
	}

	return true;
}

//=============================================================================
// Integration with kd-tree system
//=============================================================================

/*
==================
SVO_SetCollisionTree

Set the active collision SVO (called from kd_compile)
==================
*/
void SVO_SetCollisionTree (svo_t *svo)
{
	if (collision_svo)
		SVO_Free(collision_svo);
	collision_svo = svo;
}

/*
==================
SVO_GetCollisionTree
==================
*/
svo_t *SVO_GetCollisionTree (void)
{
	return collision_svo;
}

/*
==================
SVO_Shutdown
==================
*/
void SVO_Shutdown (void)
{
	if (collision_svo)
	{
		SVO_Free(collision_svo);
		collision_svo = NULL;
	}
}
