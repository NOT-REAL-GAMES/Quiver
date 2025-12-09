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
// kd_svo.h -- Sparse Voxel Octree for collision detection

#ifndef KD_SVO_H
#define KD_SVO_H

#include "quakedef.h"
#include "kd_model.h"

// Opaque SVO type
typedef struct svo_s svo_t;

// Build an SVO from brush geometry
svo_t *SVO_Build (mkdbrush_t *brushes, int num_brushes, vec3_t world_mins, vec3_t world_maxs);

// Free an SVO
void SVO_Free (svo_t *svo);

// Point containment test
int SVO_PointContents (svo_t *svo, vec3_t point);

// Ray tracing
qboolean SVO_TraceLine (svo_t *svo, vec3_t start, vec3_t end, trace_t *trace);

// Box tracing (hull expansion)
qboolean SVO_TraceBox (svo_t *svo, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, trace_t *trace);

// Global collision SVO management
void SVO_SetCollisionTree (svo_t *svo);
svo_t *SVO_GetCollisionTree (void);
void SVO_Shutdown (void);

#endif // KD_SVO_H
