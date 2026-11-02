/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_COLLISION_H
#define QUIVER_TERRAIN_COLLISION_H

#include "terrain_density.h"

namespace quiver
{
struct TerrainCollisionInterval
{
	// Unclamped times along start + offset * t. Intervals intersect [0,1],
	// have positive duration, and can be unioned across cubes/tetrahedra.
	double				 enter = 0, exit = 0;
	std::array<float, 3> normal{};
	Cell				 cell{0, Solid};
	bool				 startSolid = false;
};

struct TerrainCubeTrace
{
	std::array<TerrainCollisionInterval, 6> intervals;
	size_t									count = 0;
};

// Sweeps an axis-aligned box through the exact piecewise-linear solid of one
// eight-unit density cube. Corner i is minimum + VoxelSize * (i&1,i>>1&1,i>>2).
// Positions share any small local coordinate frame; no absolute world floats,
// render meshes, cache access, worker wait, or heap allocation are required.
// Projection-width-zero axes include boundaries so rays cannot leak between
// tetrahedra. Nonzero-width axes require penetration, allowing wall/floor slide.
// Positive liquid samples are nonblocking. Density-created solid above a
// quantized voxel boundary still reports Solid even when that sample was Empty.
TerrainCubeTrace TraceTerrainCube (
	const std::array<TerrainDensitySample, 8> &samples, const std::array<double, 3> &minimum, const std::array<double, 3> &start,
	const std::array<double, 3> &offset, const std::array<float, 3> &mins = {}, const std::array<float, 3> &maxs = {});
} // namespace quiver
#endif
