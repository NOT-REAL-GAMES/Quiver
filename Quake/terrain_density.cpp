/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_density.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace quiver
{
namespace
{
bool OffsetKey (int64_t key, int offset, int64_t &result)
{
	if ((offset < 0 && key == INT64_MIN) || (offset > 0 && key == INT64_MAX))
		return false;
	result = key + offset;
	return true;
}
} // namespace

TerrainDensityColumn TerrainSampleColumn (const WorldConfig &config, ChunkKey key, int x, int y)
{
	assert (x >= -2 && x <= VoxelsPerChunk + 1 && y >= -2 && y <= VoxelsPerChunk + 1);
	TerrainDensityColumn column;
	column.valid = true;
	int		coords[2] = {x, y};
	int64_t keys[2] = {key.x, key.y};
	float	locals[2];
	for (int axis = 0; axis < 2; ++axis)
	{
		const int offset = coords[axis] < 0 ? -1 : coords[axis] >= VoxelsPerChunk ? 1 : 0;
		if (OffsetKey (keys[axis], offset, keys[axis]))
		{
			coords[axis] -= offset * VoxelsPerChunk;
			locals[axis] = float (coords[axis] * VoxelSize + VoxelSize / 2);
		}
		else
		{
			// Extend the final legal column at the signed chunk-key limits.
			coords[axis] = offset < 0 ? 0 : VoxelsPerChunk - 1;
			locals[axis] = offset < 0 ? 0.0f : float (ChunkSize);
		}
	}
	column.x = keys[0];
	column.y = keys[1];
	column.localX = locals[0];
	column.localY = locals[1];
	if (config.generator == Generator::Terrain)
	{
		column.voxelHeight = uint8_t (TerrainHeightVoxels (config.seed, keys[0], keys[1], coords[0], coords[1]));
		column.height = TerrainSurfaceHeight (config.seed, keys[0], keys[1], locals[0], locals[1]);
	}
	if (config.generator == Generator::Planet && config.planet)
	{
		const auto sample = config.planet->Sample (keys[0], keys[1], locals[0], locals[1]);
		column.height = float (sample.height - double (key.z) * ChunkSize);
		column.voxelHeight = int (std::clamp (std::ceil ((column.height - VoxelSize / 2) / VoxelSize), -256.0f, 256.0f));
		column.material = sample.material;
	}
	return column;
}

TerrainDensitySample
TerrainSampleDensity (const WorldConfig &config, ChunkKey key, TerrainDensityPoint point, const TerrainNeighborhood &chunks, const TerrainDensityColumn &column)
{
	TerrainDensitySample sample;
	sample.valid = true;
	int					offset[3];
	TerrainDensityPoint local = point;
	bool				canonical = true;
	ChunkKey			sampleKey = key;
	for (int axis = 0; axis < 3; ++axis)
	{
		assert (point[axis] >= -2 && point[axis] <= VoxelsPerChunk + 1);
		offset[axis] = point[axis] < 0 ? -1 : point[axis] >= VoxelsPerChunk ? 1 : 0;
		canonical &= OffsetKey (key[axis], offset[axis], sampleKey[axis]);
		local[axis] -= offset[axis] * VoxelsPerChunk;
	}
	const bool	 generatedSolid = config.generator == Generator::Planet
									  ? point[2] < column.voxelHeight
									  : config.generator == Generator::Terrain && (sampleKey.z < 0 || (sampleKey.z == 0 && local[2] < column.voxelHeight));
	Cell		 cell{uint16_t (generatedSolid ? 1 : 0), int16_t (generatedSolid ? Solid : Empty)};
	const size_t neighbor = (offset[2] + 1) * 9 + (offset[1] + 1) * 3 + offset[0] + 1;
	if (canonical && chunks[neighbor])
		cell = chunks[neighbor]->cells.Get (local[0], local[1], local[2]);
	const bool occupied = cell.contents != Empty;
	sample.cell = cell;
	sample.modified = occupied != generatedSolid;
	sample.material = config.generator == Generator::Planet && !sample.modified ? column.material : (occupied ? cell.material : 2);
	if (sample.modified || config.generator == Generator::Empty)
		sample.density = occupied ? float (VoxelSize / 2) : float (-VoxelSize / 2);
	else if (config.generator == Generator::Planet)
		sample.density = std::clamp (column.height - float (point[2] * VoxelSize + VoxelSize / 2), float (-VoxelSize), float (VoxelSize));
	else if (sampleKey.z < 0)
		sample.density = float (VoxelSize);
	else if (sampleKey.z > 0)
		sample.density = float (-VoxelSize);
	else
		sample.density = std::clamp (column.height - float (local[2] * VoxelSize + VoxelSize / 2), float (-VoxelSize), float (VoxelSize));
	return sample;
}

TerrainDensitySample TerrainSampleDensity (const WorldConfig &config, ChunkKey key, TerrainDensityPoint point, const TerrainNeighborhood &chunks)
{
	return TerrainSampleDensity (config, key, point, chunks, TerrainSampleColumn (config, key, point[0], point[1]));
}
} // namespace quiver
