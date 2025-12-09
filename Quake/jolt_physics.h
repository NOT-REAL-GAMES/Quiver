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
// jolt_physics.h -- Jolt Physics integration for collision detection

#ifndef JOLT_PHYSICS_H
#define JOLT_PHYSICS_H

// Include quakedef.h OUTSIDE of extern "C" to avoid template issues
#include "quakedef.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to Jolt collision world
typedef struct jolt_world_s jolt_world_t;

// Initialize Jolt Physics system (call once at startup)
void Jolt_Init (void);

// Shutdown Jolt Physics system (call at exit)
void Jolt_Shutdown (void);

// Create a collision world from triangle mesh data
// vertices: array of vec3_t positions
// num_vertices: number of vertices
// indices: array of triangle indices (3 per triangle)
// num_indices: total number of indices
jolt_world_t *Jolt_CreateWorld (vec3_t *vertices, int num_vertices, int *indices, int num_indices);

// Free a collision world
void Jolt_FreeWorld (jolt_world_t *world);

// Set the active collision world for traces
void Jolt_SetActiveWorld (jolt_world_t *world);

// Get the active collision world
jolt_world_t *Jolt_GetActiveWorld (void);

// Trace a box through the world
// start: starting position (center of box)
// mins, maxs: box extents relative to center
// end: ending position
// trace: output trace result
// Returns true if trace hit something
qboolean Jolt_TraceBox (jolt_world_t *world, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace);

// Trace a ray (point) through the world
qboolean Jolt_TraceLine (jolt_world_t *world, vec3_t start, vec3_t end, trace_t *trace);

// Point containment test
int Jolt_PointContents (jolt_world_t *world, vec3_t point);

#ifdef __cplusplus
}
#endif

#endif // JOLT_PHYSICS_H
