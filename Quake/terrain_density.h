/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_DENSITY_H
#define QUIVER_TERRAIN_DENSITY_H

#include "octree_world.h"

namespace quiver
{
using TerrainDensityPoint = std::array<int, 3>;
using TerrainNeighborhood = std::array<std::shared_ptr<const Chunk>, 27>;

// One body diagonal and identical face diagonals across every density cube.
// Both render extraction and collision must use this decomposition.
inline constexpr int TerrainTetrahedra[6][4] = {{0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7}, {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};

struct TerrainDensityColumn
{
	int64_t	 x = 0, y = 0;
	float	 localX = 0, localY = 0, height = 0;
	int		 voxelHeight = 0;
	uint16_t material = 1;
	bool	 valid = false;
};

struct TerrainDensitySample
{
	float	 density = 0;
	uint16_t material = 0;
	bool	 modified = false;
	bool	 valid = false;
	Cell	 cell;
};

// Random-access, allocation-free sampling shared by physics and meshing.
// Points identify voxel centers relative to key. The one-chunk halo permits
// indices -2..129 (including mesher normal probes), without absolute floats.
// Missing snapshots use the procedural generator; the world trace separately
// treats unloaded chunks as solid barriers. The caller may cache columns and
// samples in any traversal order; these functions have no rolling-slab state.
TerrainDensityColumn TerrainSampleColumn (const WorldConfig &config, ChunkKey key, int x, int y);
TerrainDensitySample TerrainSampleDensity (
	const WorldConfig &config, ChunkKey key, TerrainDensityPoint point, const TerrainNeighborhood &chunks, const TerrainDensityColumn &column);
TerrainDensitySample TerrainSampleDensity (const WorldConfig &config, ChunkKey key, TerrainDensityPoint point, const TerrainNeighborhood &chunks);
} // namespace quiver
#endif
