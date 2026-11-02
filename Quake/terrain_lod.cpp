/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_lod.h"
#include "planet_surface.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace quiver
{
namespace
{
constexpr float SkirtDepth = 128;

bool CheckedAdd (int64_t a, int64_t b, int64_t &result)
{
	if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
		return false;
	result = a + b;
	return true;
}

bool ValidKey (TerrainTileKey key)
{
	if (key.level > MaxTerrainTileLevel)
		return false;
	const int64_t span = int64_t (1) << key.level;
	return key.x % span == 0 && key.y % span == 0 && key.x <= INT64_MAX - (span - 1) && key.y <= INT64_MAX - (span - 1);
}

int64_t AlignDown (int64_t value, int64_t span)
{
	int64_t remainder = value % span;
	if (remainder < 0)
		remainder += span;
	// All supported spans divide 2^63, so rounding toward INT64_MIN is safe.
	return value - remainder;
}

double ChunkOffset (int64_t to, int64_t from)
{
	// Defined unsigned subtraction retains small relative distances at either
	// signed limit; no absolute coordinate is ever converted to a float.
	return to >= from ? double (uint64_t (to) - uint64_t (from)) * ChunkSize : -double (uint64_t (from) - uint64_t (to)) * ChunkSize;
}

struct Candidate
{
	TerrainTileKey key;
	double		   distanceSquared = 0;
	double		   priority = 0;
	bool		   refine = false;
};

struct CandidateOrder
{
	bool operator() (const Candidate &a, const Candidate &b) const
	{
		return std::tie (a.priority, a.key.level, a.key.x, a.key.y) > std::tie (b.priority, b.key.level, b.key.x, b.key.y);
	}
};
} // namespace

std::shared_ptr<const TerrainTileMesh> BuildTerrainTile (const WorldConfig &config, TerrainTileKey key, std::string *error)
{
	try
	{
		if (!ValidKey (key))
			throw std::runtime_error ("invalid or unaligned terrain tile key");
		if (config.generator != Generator::Empty && config.generator != Generator::Terrain && config.generator != Generator::Planet)
			throw std::runtime_error ("invalid terrain generator");
		if (config.generator == Generator::Planet && (!config.planet || !config.planet->Ready ()))
			throw std::runtime_error ("planet generator must be prepared before terrain LOD");
		auto mesh = std::make_shared<TerrainTileMesh> ();
		mesh->key = key;
		if (config.generator == Generator::Empty)
		{
			if (error)
				error->clear ();
			return mesh;
		}
		constexpr int Stride = TerrainTileCells + 1;
		const int64_t span = int64_t (1) << key.level;
		const float	  width = float (span * ChunkSize);
		const float	  step = width / TerrainTileCells;
		mesh->vertices.reserve (Stride * Stride + 8 * Stride);
		mesh->indices.reserve (6 * TerrainTileCells * TerrainTileCells + 24 * TerrainTileCells);
		mesh->boundsMin = {0, 0, std::numeric_limits<float>::max ()};
		mesh->boundsMax = {width, width, std::numeric_limits<float>::lowest ()};
		mesh->surfaceMin = std::numeric_limits<float>::max ();
		mesh->surfaceMax = std::numeric_limits<float>::lowest ();
		// Coarse mountain samples can differ by much more than the legacy 128-unit hills.
		const float skirtDepth = config.generator == Generator::Planet ? std::max (SkirtDepth, step * 2) : SkirtDepth;
		for (int y = 0; y < Stride; ++y)
			for (int x = 0; x < Stride; ++x)
			{
				const int64_t		 vx = span * VoxelsPerChunk * x / TerrainTileCells;
				const int64_t		 vy = span * VoxelsPerChunk * y / TerrainTileCells;
				// Express the final endpoint as local128 in the last chunk. This
				// also samples a tile ending at INT64_MAX without overflowing.
				const int64_t		 cx = std::min (vx / VoxelsPerChunk, span - 1);
				const int64_t		 cy = std::min (vy / VoxelsPerChunk, span - 1);
				const int			 localVoxelX = int (vx - cx * VoxelsPerChunk), localVoxelY = int (vy - cy * VoxelsPerChunk);
				const float			 localX = float (localVoxelX * VoxelSize), localY = float (localVoxelY * VoxelSize);
				float				 height;
				std::array<float, 3> normal;
				uint16_t			 material = 2;
				if (config.generator == Generator::Planet)
				{
					// Avoid constructing erosion regions for sparse horizon vertices.
					// Existing downward skirts bridge the erosion-detail transition.
					const auto sample = SamplePlanetBackdrop (config, key.x + cx, key.y + cy, localX, localY, step >= ChunkSize);
					height = float (sample.height);
					normal = sample.normal;
					material = sample.material;
				}
				else
				{
					height = config.smoothTerrain ? TerrainSurfaceHeight (config.seed, key.x + cx, key.y + cy, localX, localY)
												  : float (VoxelSize * TerrainHeightVoxels (config.seed, key.x + cx, key.y + cy, localVoxelX, localVoxelY));
					normal = TerrainSurfaceNormal (config.seed, key.x + cx, key.y + cy, localX, localY);
				}
				mesh->vertices.push_back ({{x * step, y * step, height}, material, 5, normal});
				const float detailMargin = config.generator == Generator::Planet ? 64.0f : 0.0f;
				mesh->surfaceMin = std::min (mesh->surfaceMin, height - detailMargin);
				mesh->surfaceMax = std::max (mesh->surfaceMax, height + detailMargin);
				mesh->boundsMin[2] = std::min (mesh->boundsMin[2], height - skirtDepth);
				mesh->boundsMax[2] = std::max (mesh->boundsMax[2], height);
			}
		auto triangle = [&] (uint32_t a, uint32_t b, uint32_t c)
		{
			mesh->indices.push_back (a);
			mesh->indices.push_back (b);
			mesh->indices.push_back (c);
		};
		for (int y = 0; y < TerrainTileCells; ++y)
			for (int x = 0; x < TerrainTileCells; ++x)
			{
				const uint32_t first = y * Stride + x;
				triangle (first, first + Stride, first + Stride + 1);
				triangle (first, first + Stride + 1, first + 1);
			}
		constexpr uint8_t Faces[] = {2, 1, 3, 0};
		for (int side = 0; side < 4; ++side)
		{
			const uint32_t		 first = uint32_t (mesh->vertices.size ());
			std::array<float, 3> normal{};
			normal[Faces[side] / 2] = (Faces[side] & 1) ? 1.0f : -1.0f;
			for (int i = 0; i < Stride; ++i)
			{
				int x, y;
				switch (side)
				{
				case 0:
					x = i;
					y = 0;
					break;
				case 1:
					x = TerrainTileCells;
					y = i;
					break;
				case 2:
					x = TerrainTileCells - i;
					y = TerrainTileCells;
					break;
				default:
					x = 0;
					y = TerrainTileCells - i;
					break;
				}
				const auto	  &top = mesh->vertices[y * Stride + x];
				auto		   position = top.position;
				const uint16_t material = config.generator == Generator::Planet ? top.material : 1;
				// Skirts are seam coverage, not physical vertical cliffs. Keep
				// planet surface shading continuous when a subpixel edge is visible.
				const auto	   skirtNormal = config.generator == Generator::Planet ? top.normal : normal;
				mesh->vertices.push_back ({position, material, Faces[side], skirtNormal});
				position[2] -= skirtDepth;
				mesh->vertices.push_back ({position, material, Faces[side], skirtNormal});
			}
			for (int i = 0; i < TerrainTileCells; ++i)
			{
				const uint32_t a = first + i * 2;
				triangle (a, a + 2, a + 3);
				triangle (a, a + 3, a + 1);
			}
		}
		if (config.generator == Generator::Planet && mesh->surfaceMin < config.planetSettings.seaLevel)
		{
			const float	   sea = float (config.planetSettings.seaLevel);
			const uint32_t first = uint32_t (mesh->vertices.size ());
			mesh->vertices.push_back ({{0, 0, sea}, 5});
			mesh->vertices.push_back ({{width, 0, sea}, 5});
			mesh->vertices.push_back ({{0, width, sea}, 5});
			mesh->vertices.push_back ({{width, width, sea}, 5});
			triangle (first, first + 2, first + 3);
			triangle (first, first + 3, first + 1);
			mesh->oceanIndexCount = 6;
			mesh->boundsMax[2] = std::max (mesh->boundsMax[2], sea);
		}
		if (error)
			error->clear ();
		return mesh;
	}
	catch (const std::exception &failure)
	{
		if (error)
			*error = failure.what ();
		return nullptr;
	}
}

std::vector<TerrainTileKey> SelectTerrainTiles (const WorldPosition &camera, double distance, int detailRadius, size_t maxTiles)
{
	std::vector<TerrainTileKey> result;
	if (!std::isfinite (distance) || distance <= 0 || !maxTiles)
		return result;
	for (float local : camera.local)
		if (!std::isfinite (local) || local < 0 || local >= ChunkSize)
			return result;
	distance = std::min (distance, MaxTerrainDistance);
	detailRadius = std::clamp (detailRadius, 0, 8);
	maxTiles = std::min (maxTiles, MaxTerrainTiles);
	uint8_t rootLevel = 0;
	while (rootLevel < MaxTerrainTileLevel && double (int64_t (1) << rootLevel) * ChunkSize < distance)
		++rootLevel;
	const int64_t rootSpan = int64_t (1) << rootLevel;
	const int64_t rootX = AlignDown (camera.chunk.x, rootSpan);
	const int64_t rootY = AlignDown (camera.chunk.y, rootSpan);
	const double  distanceSquared = distance * distance;
	auto		  candidate = [&] (TerrainTileKey key)
	{
		const double width = double (int64_t (1) << key.level) * ChunkSize;
		const double x = ChunkOffset (key.x, camera.chunk.x);
		const double y = ChunkOffset (key.y, camera.chunk.y);
		const double dx = std::max ({x - camera.local[0], -x - width + camera.local[0], 0.0});
		const double dy = std::max ({y - camera.local[1], -y - width + camera.local[1], 0.0});
		const double squared = dx * dx + dy * dy;
		const double nearMin = -double (detailRadius) * ChunkSize;
		const double nearMax = double (detailRadius + 1) * ChunkSize;
		const bool	 near = x < nearMax && y < nearMax && x + width > nearMin && y + width > nearMin;
		return Candidate{key, squared, near ? -width : squared / (width * width), key.level > 0 && (near || squared < 2.25 * width * width)};
	};
	std::priority_queue<Candidate, std::vector<Candidate>, CandidateOrder> pending;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			TerrainTileKey key;
			key.level = rootLevel;
			if (!CheckedAdd (rootX, x * rootSpan, key.x) || !CheckedAdd (rootY, y * rootSpan, key.y))
				continue;
			Candidate next = candidate (key);
			if (next.distanceSquared <= distanceSquared)
				pending.push (next);
		}
	// A budget smaller than the initial cover cannot provide complete coverage.
	if (pending.size () > maxTiles)
		return result;
	while (!pending.empty ())
	{
		Candidate current = pending.top ();
		pending.pop ();
		std::array<Candidate, 4> children;
		size_t					 count = 0;
		if (current.refine)
		{
			const int64_t half = int64_t (1) << (current.key.level - 1);
			for (int y = 0; y < 2; ++y)
				for (int x = 0; x < 2; ++x)
				{
					Candidate child = candidate ({current.key.x + x * half, current.key.y + y * half, uint8_t (current.key.level - 1)});
					if (child.distanceSquared <= distanceSquared)
						children[count++] = child;
				}
		}
		if (count && result.size () + pending.size () + count <= maxTiles)
		{
			for (size_t i = 0; i < count; ++i)
				pending.push (children[i]);
		}
		else
			result.push_back (current.key);
	}
	std::sort (
		result.begin (), result.end (),
		[&] (TerrainTileKey a, TerrainTileKey b)
		{
			const auto ca = candidate (a), cb = candidate (b);
			return std::tie (ca.distanceSquared, a.level, a.x, a.y) < std::tie (cb.distanceSquared, b.level, b.x, b.y);
		});
	return result;
}
} // namespace quiver
