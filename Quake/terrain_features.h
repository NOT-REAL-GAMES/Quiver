/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_FEATURES_H
#define QUIVER_TERRAIN_FEATURES_H
#include "world_solids.h"
namespace quiver
{
struct FeatureSample
{
	double	   density = -1;
	SolidPoint normal{0, 0, 1};
	uint16_t   material = 3;
	bool	   hard = false;
};
using FeatureSampler = std::function<FeatureSample (const SolidPoint &)>;
using FeatureDensitySampler = std::function<double (const SolidPoint &)>;
// Uniform dual contouring with bounded QEF vertices; sample one cell of negative
// halo so neighboring chunks share the same boundary vertices. Own grid edges by
// minimum endpoint in [minimum, minimum+cellCount*cellSize). Up to 128^3 cells.
// Output clockwise triangles; uses root-to-leaf edge bisection (16 iterations).
// Uses separate QEF vertices for disconnected inside-corner components.
void BuildFeatureSurface (
	const SolidPoint &minimum, const std::array<int, 3> &cellCount, double cellSize, const FeatureSampler &sample, ChunkMesh &mesh,
	const FeatureDensitySampler &densityOnly = {});
struct FeatureTrace
{
	bool								   hit = false;
	bool								   startSolid = false;
	// Positive-duration intersections of the swept hull with surface triangles.
	std::vector<std::pair<double, double>> surfaceIntervals;
	// Center-ray crossings: second is true on entry, false on exit. The world
	// merges crossings across chunk-owned triangles before classifying volume.
	std::vector<std::pair<double, bool>>   centerCrossings;
	double								   fraction = 1;
	SolidPoint							   normal{};
	uint32_t							   triangle = 0;
};
// Continuous swept AABB against mesh triangles. Caller separately determines
// start-solid from its authoritative density/CSG, as a surface is not a volume.
FeatureTrace
SweepFeatureMesh (const ChunkMesh &mesh, const SolidPoint &start, const SolidPoint &offset, const SolidPoint &mins = {}, const SolidPoint &maxs = {});
} // namespace quiver
#endif
