/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_MESHER_H
#define QUIVER_TERRAIN_MESHER_H

#include "octree_world.h"

namespace quiver
{
// Worker-safe extraction from immutable snapshots. The 3x3x3 neighborhood is
// indexed (dz+1)*9 + (dy+1)*3 + dx+1; element 13 is the requested chunk.
// Existing exposed quads bound interior work. Missing neighbors use the base
// generator, and boundary cubes are clipped to the exact chunk bounds.
// Output uses clockwise exterior winding. Throws on an invalid snapshot or a
// bounded output allocation exceeding the per-chunk/configured mesh budget.
void BuildSmoothTerrainMesh (const WorldConfig &config, const std::array<std::shared_ptr<const Chunk>, 27> &chunks, ChunkMesh &mesh);
// Exact spatial subset of the same tetrahedral surface, clipped to
// [firstCell * VoxelSize, (firstCell + cellCount) * VoxelSize]. All bounds
// address the center chunk. Adjacent regions share only their boundary edges;
// clipped triangles can be subdivided but never change the underlying surface.
// A triangle lying on a positive tile plane belongs to the neighboring tile.
// Work and scratch allocations scale with the region, without a full mesh pass.
void BuildSmoothTerrainRegion (
	const WorldConfig &config, const std::array<std::shared_ptr<const Chunk>, 27> &chunks, const std::array<int, 3> &firstCell,
	const std::array<int, 3> &cellCount, ChunkMesh &mesh);
} // namespace quiver
#endif
