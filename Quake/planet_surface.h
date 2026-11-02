/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_PLANET_SURFACE_H
#define QUIVER_PLANET_SURFACE_H
#include "terrain_density.h"
#include "terrain_features.h"
namespace quiver
{
void BuildPlanetSurface (const WorldConfig &config, const TerrainNeighborhood &chunks, ChunkMesh &mesh);
// Exact subset of full extraction on the same globally aligned4+8*i lattice.
// Own only edges in the supplied cell range; include the same negative halo
// for QEF vertices. No render transitions or whole-chunk height scan.
void BuildPlanetSurfaceRegion (
	const WorldConfig &config, const TerrainNeighborhood &chunks, const std::array<int, 3> &firstCell, const std::array<int, 3> &cellCount, ChunkMesh &mesh);
// Worker-side scalar lattice preparation, using cached columns and prepared
// solids. Output order is Z/Y/X with X varying fastest.
void SamplePlanetSurfaceLattice (
	const WorldConfig &config, const TerrainNeighborhood &chunks, const std::array<int, 3> &firstSample, int sampleCount, std::vector<float> &density);
FeatureSample SamplePlanetSurface (const WorldConfig &config, const TerrainNeighborhood &chunks, const SolidPoint &point);
// Highest natural surface, sharing rock displacement, normals and material
// selection with detailed extraction. Coarse skips regional erosion.
PlanetSample  SamplePlanetBackdrop (const WorldConfig &config, int64_t chunkX, int64_t chunkY, double localX, double localY, bool coarse = false);
// Worker-side render bridges; source/collision triangles remain unchanged.
void		  BuildPlanetTransitions (const WorldConfig &config, ChunkMesh &mesh);
} // namespace quiver
#endif
