/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_LOD_H
#define QUIVER_TERRAIN_LOD_H

#include "octree_world.h"

namespace quiver
{
constexpr int	 TerrainTileCells = 16;
constexpr int	 MaxTerrainTileLevel = 14;
constexpr size_t MaxTerrainTiles = 512;
constexpr double MaxTerrainDistance = double (ChunkSize) * (1 << MaxTerrainTileLevel);

struct TerrainTileKey
{
	// Aligned minimum CHUNK coordinates, not indices multiplied by tile size.
	// A level L tile covers 2^L by 2^L chunks, always anchored at world z=0.
	int64_t x = 0, y = 0;
	uint8_t level = 0;
	bool	operator== (const TerrainTileKey &) const = default;
};

struct TerrainVertex
{
	std::array<float, 3> position{};
	uint16_t			 material = 2;
	uint8_t				 face = 5; // Same axis*2 + positive-normal convention as MeshQuad.
	std::array<float, 3> normal{0, 0, 1};
};

struct TerrainTileMesh
{
	TerrainTileKey			   key;
	// Positions and bounds are local to {{key.x,key.y,0},{0,0,0}}. Indexed
	// triangles already use the renderer's clockwise-outside convention.
	std::vector<TerrainVertex> vertices;
	std::vector<uint32_t>	   indices;
	std::array<float, 3>	   boundsMin{}, boundsMax{};
	// Surface-only heights exclude skirts and oceans, for multi-chunk detail replacement.
	float					   surfaceMin = 0, surfaceMax = 0;
	uint32_t				   oceanIndexCount = 0; // Final indices, drawn independently of detail replacement.
};

// Immutable procedural backdrop only: edits, caves and placed structures are
// represented by nearby detailed chunks. Empty generators return an empty mesh.
// Pure worker-safe construction, fixed 16x16 top grid plus downward seam skirts.
// Invalid keys/configurations return null and set error; no chunk or DB access.
std::shared_ptr<const TerrainTileMesh> BuildTerrainTile (const WorldConfig &config, TerrainTileKey key, std::string *error = nullptr);

// Bounded, nonoverlapping quadtree cover of the horizontal view radius. Inner
// chunks retain level-zero fallback tiles; suppress them only once detailed
// GPU meshes are ready. detailRadius is clamped to 0..8 chunks; maxTiles 1..512.
// Invalid/noncanonical positions or nonfinite/nonpositive distance return empty.
// Distance is capped at MaxTerrainDistance; z never affects tile selection.
// A caller budget too small for the initial complete cover returns empty;
// otherwise budget pressure reduces refinement rather than opening holes.
std::vector<TerrainTileKey> SelectTerrainTiles (const WorldPosition &camera, double distance, int detailRadius = 4, size_t maxTiles = MaxTerrainTiles);
} // namespace quiver
#endif
