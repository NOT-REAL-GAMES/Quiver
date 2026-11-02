/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "planet_surface.h"
#include "terrain_lod.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
namespace quiver
{
namespace
{
// Periodic fixed-lattice rock displacement. Vertical variation permits natural
// overhangs on steep slopes; flat ground and the spawn plateau remain unchanged.
std::array<double, 4> RockNoise (const WorldConfig &config, ChunkKey key, const SolidPoint &p, bool gradient = true)
{
	std::array<int64_t, 3> cell;
	SolidPoint			   f, w, dw;
	for (int a = 0; a < 3; ++a)
	{
		double q = p[a] / 64.0;
		cell[a] = int64_t (std::floor (q));
		f[a] = q - std::floor (q);
		w[a] = f[a] * f[a] * (3 - 2 * f[a]);
		dw[a] = 6 * f[a] * (1 - f[a]) / 64.0;
	}
	key = CanonicalWorldKey (config, key);
	std::array<double, 4> result{};
	for (int i = 0; i < 8; ++i)
	{
		uint64_t   h = config.seed;
		double	   weight = 1;
		SolidPoint factors;
		for (int a = 0; a < 3; ++a)
		{
			uint64_t coordinate = uint64_t (key[a]) * 16 + uint64_t (cell[a]) + uint64_t ((i >> a) & 1);
			if (a < 2)
			{
				const int64_t count = config.planetSettings.extentChunks * 16;
				int64_t		  c = int64_t (key[a]) * 16 + cell[a] + ((i >> a) & 1);
				c %= count;
				if (c < 0)
					c += count;
				coordinate = uint64_t (c);
			}
			h ^= coordinate + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
			h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
			factors[a] = ((i >> a) & 1) ? w[a] : 1 - w[a];
			weight *= factors[a];
		}
		const double value = double ((h >> 40) & 65535) / 32767.5 - 1;
		result[0] += weight * value;
		for (int a = 0; gradient && a < 3; ++a)
		{
			double d = dw[a] * (((i >> a) & 1) ? 1 : -1);
			for (int b = 0; b < 3; ++b)
				if (a != b)
					d *= factors[b];
			result[a + 1] += value * d;
		}
	}
	return result;
}
struct SurfaceCache
{
	struct XYHash
	{
		size_t operator() (const std::pair<double, double> &xy) const
		{
			const size_t x = std::hash<double>{}(xy.first), y = std::hash<double>{}(xy.second);
			return x ^ (y + size_t (0x9e3779b9) + (x << 6) + (x >> 2));
		}
	};
	int																	 firstX, firstY, width, height;
	std::vector<FeatureSample>											 lattice;
	std::vector<uint8_t>												 valid;
	std::vector<double>													 centers;
	std::vector<uint8_t>												 centerValid;
	std::unordered_map<std::pair<double, double>, FeatureSample, XYHash> columns;
	std::vector<PreparedSolid>											 solids;
	std::vector<SolidPoint>												 offsets;
	SurfaceCache (int x = -1, int y = -1, int w = VoxelsPerChunk + 2, int h = VoxelsPerChunk + 2)
		: firstX (x), firstY (y), width (w), height (h), lattice (size_t (w) * h), valid (size_t (w) * h), centers (size_t (w) * h),
		  centerValid (size_t (w) * h)
	{
	}
	size_t Index (int x, int y) const
	{
		return x >= firstX && x < firstX + width && y >= firstY && y < firstY + height ? size_t (y - firstY) * width + x - firstX : size_t (-1);
	}
};
FeatureSample BaseColumn (const WorldConfig &config, const ChunkKey &key, const SolidPoint &p)
{
	FeatureSample result;
	result.density = -1e9;
	if (config.generator == Generator::Planet)
	{
		const auto terrain = config.planet->Sample (key.x, key.y, p[0], p[1]);
		result.density = terrain.height - double (key.z) * ChunkSize;
		result.normal = {terrain.normal[0], terrain.normal[1], terrain.normal[2]};
		result.material = terrain.material;
	}
	else if (config.generator == Generator::Terrain)
	{
		WorldPosition pos{key, {}};
		Add (pos, p, pos);
		result.density = TerrainSurfaceHeight (config.seed, pos.chunk.x, pos.chunk.y, pos.local[0], pos.local[1]) - double (key.z) * ChunkSize;
		auto normal = TerrainSurfaceNormal (config.seed, pos.chunk.x, pos.chunk.y, pos.local[0], pos.local[1]);
		result.normal = {normal[0], normal[1], normal[2]};
		result.material = 1;
	}
	return result;
}
FeatureSample CachedColumn (const WorldConfig &config, const ChunkKey &key, const SolidPoint &p, SurfaceCache *cache)
{
	if (!cache)
		return BaseColumn (config, key, p);
	const int	 x = int (std::floor (p[0] / VoxelSize)), y = int (std::floor (p[1] / VoxelSize));
	const size_t index = cache->Index (x, y);
	if (index != size_t (-1) && p[0] == x * VoxelSize + VoxelSize / 2 && p[1] == y * VoxelSize + VoxelSize / 2)
	{
		if (!cache->valid[index])
		{
			cache->lattice[index] = BaseColumn (config, key, p);
			cache->valid[index] = 1;
		}
		return cache->lattice[index];
	}
	const auto xy = std::make_pair (p[0], p[1]);
	auto	   it = cache->columns.find (xy);
	if (it != cache->columns.end ())
		return it->second;
	const auto result = BaseColumn (config, key, p);
	if (cache->columns.size () < 65536)
		cache->columns.emplace (xy, result);
	return result;
}
void ApplyRock (const WorldConfig &config, ChunkKey key, const SolidPoint &p, FeatureSample &result, bool signOnly = false)
{
	if (config.generator == Generator::Planet)
	{
		const double strength = 64.0 * std::clamp ((1.0 - result.normal[2]) / .4, 0.0, 1.0);
		// Extraction uses only signs until locating a Hermite root. Bounded
		// displacement cannot change this sign, so avoid noise far from a surface.
		if (signOnly && std::abs (result.density) > strength)
			return;
		if (strength > 0)
		{
			auto rock = RockNoise (config, key, p, !signOnly);
			result.density += strength * rock[0];
			if (signOnly)
				return;
			double nz = std::max (.01, result.normal[2]);
			result.normal = {result.normal[0] / nz - strength * rock[1], result.normal[1] / nz - strength * rock[2], 1 - strength * rock[3]};
			double length = std::hypot (result.normal[0], result.normal[1], result.normal[2]);
			if (length > 0)
				for (auto &n : result.normal)
					n /= length;
			if (strength > 16)
				result.material = 3;
		}
	}
}
FeatureSample Sample (const WorldConfig &config, const TerrainNeighborhood &chunks, const SolidPoint &p, SurfaceCache *cache = nullptr, bool signOnly = false)
{
	const ChunkKey key = chunks[13]->key;
	FeatureSample  result = CachedColumn (config, key, p, cache);
	result.density -= p[2];
	ApplyRock (config, key, p, result, signOnly);
	WorldPosition pos{key, {}};
	if (Add (pos, p, pos))
	{
		int	 index = 13;
		bool valid = true;
		for (int a = 0, stride = 1; a < 3; ++a, stride *= 3)
		{
			auto d = pos.chunk[a] - key[a];
			if (d < -1 || d > 1)
			{
				valid = false;
				break;
			}
			index += int (d) * stride;
		}
		if (valid && chunks[index])
		{
			int v[3];
			for (int a = 0; a < 3; ++a)
				v[a] = int (pos.local[a]) / VoxelSize;
			auto		 actual = chunks[index]->cells.Get (v[0], v[1], v[2]);
			const double centerZ = double (pos.chunk.z) * ChunkSize + v[2] * VoxelSize + VoxelSize / 2;
			double		 centerH = -1e9;
			const int	 centerX = int (std::floor (p[0] / VoxelSize)), centerY = int (std::floor (p[1] / VoxelSize));
			const size_t centerIndex = cache ? cache->Index (centerX, centerY) : size_t (-1);
			if (cache && centerIndex != size_t (-1) && cache->centerValid[centerIndex])
				centerH = cache->centers[centerIndex];
			else
			{
				if (config.generator == Generator::Planet)
					centerH = cache ? CachedColumn (config, key, {double (centerX * VoxelSize + 4), double (centerY * VoxelSize + 4), 0}, cache).density +
										  double (key.z) * ChunkSize
									: config.planet->Height (pos.chunk.x, pos.chunk.y, v[0] * VoxelSize + 4, v[1] * VoxelSize + 4);
				else if (config.generator == Generator::Terrain)
					centerH = TerrainHeightVoxels (config.seed, pos.chunk.x, pos.chunk.y, v[0], v[1]) * VoxelSize;
				if (cache && centerIndex != size_t (-1))
				{
					cache->centers[centerIndex] = centerH;
					cache->centerValid[centerIndex] = 1;
				}
			}
			if (actual.IsSolid () != (centerZ < centerH))
			{
				double face = VoxelSize;
				int	   axis = 0;
				double sign = 1;
				for (int a = 0; a < 3; ++a)
				{
					double q = pos.local[a] - v[a] * VoxelSize;
					double f = std::min (q, double (VoxelSize) - q);
					if (f < face)
					{
						face = f;
						axis = a;
						sign = q < VoxelSize / 2 ? -1 : 1;
					}
				}
				result.density = actual.IsSolid () ? face : -face;
				result.normal = {};
				result.normal[axis] = actual.IsSolid () ? sign : -sign;
				result.material = actual.material;
				result.hard = true;
			}
		}
	}
	if (chunks[13]->solids)
	{
		for (size_t i = 0; i < chunks[13]->solids->size (); ++i)
		{
			const auto &solid = (*chunks[13]->solids)[i];
			SolidPoint	offset;
			if (cache)
				offset = cache->offsets[i];
			else if (!WorldDelta (config, WorldPosition{key, {}}, solid.origin, offset, 16777216))
				continue;
			auto point = p;
			for (int a = 0; a < 3; ++a)
				point[a] += offset[a];
			auto		 value = cache ? cache->solids[i].Sample (point) : SampleSolid (solid, point);
			const bool	 subtract = solid.operation == SolidOperation::Subtract;
			const double d = subtract ? -value.density : value.density;
			if ((subtract && d < result.density) || (!subtract && d > result.density))
			{
				result.density = d;
				result.normal = value.normal;
				if (subtract)
					for (auto &n : result.normal)
						n = -n;
				result.material = value.material;
				result.hard = true;
			}
		}
	}
	return result;
}
} // namespace
void SamplePlanetSurfaceLattice (
	const WorldConfig &config, const TerrainNeighborhood &chunks, const std::array<int, 3> &firstSample, int sampleCount, std::vector<float> &density)
{
	SurfaceCache cache (firstSample[0], firstSample[1], sampleCount, sampleCount);
	if (chunks[13]->solids)
		for (const auto &solid : *chunks[13]->solids)
		{
			cache.solids.emplace_back (solid);
			SolidPoint offset{};
			if (!WorldDelta (config, WorldPosition{chunks[13]->key, {}}, solid.origin, offset, 16777216))
				throw std::runtime_error ("Solid placement outside collision lattice range");
			cache.offsets.push_back (offset);
		}
	density.resize (size_t (sampleCount) * sampleCount * sampleCount);
	for (int z = 0; z < sampleCount; ++z)
		for (int y = 0; y < sampleCount; ++y)
			for (int x = 0; x < sampleCount; ++x)
			{
				SolidPoint point{
					double ((firstSample[0] + x) * VoxelSize + 4), double ((firstSample[1] + y) * VoxelSize + 4),
					double ((firstSample[2] + z) * VoxelSize + 4)};
				density[(z * sampleCount + y) * sampleCount + x] = float (Sample (config, chunks, point, &cache, true).density);
			}
}
FeatureSample SamplePlanetSurface (const WorldConfig &config, const TerrainNeighborhood &chunks, const SolidPoint &point)
{
	return Sample (config, chunks, point);
}
PlanetSample SamplePlanetBackdrop (const WorldConfig &config, int64_t chunkX, int64_t chunkY, double localX, double localY, bool coarse)
{
	auto		 terrain = coarse ? config.planet->SampleCoarse (chunkX, chunkY, localX, localY) : config.planet->Sample (chunkX, chunkY, localX, localY);
	const double strength = 64.0 * std::clamp ((1.0 - terrain.normal[2]) / .4, 0.0, 1.0);
	if (strength == 0)
		return terrain;
	const ChunkKey key{chunkX, chunkY, 0};
	auto		   at = [&] (double z)
	{
		FeatureSample sample;
		sample.density = terrain.height - z;
		sample.normal = {terrain.normal[0], terrain.normal[1], terrain.normal[2]};
		sample.material = terrain.material;
		ApplyRock (config, key, {localX, localY, z}, sample);
		return sample;
	};
	// Highest crossing gives the distant silhouette even when the density folds
	// into an overhang. Four-unit probes resolve the 64-unit rock noise lattice.
	double		 upper = terrain.height + strength, lower = upper;
	const double bottom = terrain.height - strength;
	while (lower > bottom)
	{
		lower = std::max (bottom, upper - 4.0);
		if (at (lower).density >= 0)
			break;
		upper = lower;
	}
	for (int i = 0; i < 20; ++i)
	{
		const double middle = (lower + upper) * 0.5;
		if (at (middle).density >= 0)
			lower = middle;
		else
			upper = middle;
	}
	const double height = (lower + upper) * 0.5;
	const auto	 surface = at (height);
	terrain.height = height;
	terrain.material = surface.material;
	for (int a = 0; a < 3; ++a)
		terrain.normal[a] = float (surface.normal[a]);
	return terrain;
}
void BuildPlanetTransitions (const WorldConfig &config, ChunkMesh &mesh)
{
	mesh.transitionVertices.clear ();
	mesh.transitionIndices.clear ();
	mesh.transitionCounts = {};
	if (config.generator != Generator::Planet)
		return;
	using Point = std::array<float, 3>;
	using EdgeKey = std::pair<Point, Point>;
	struct Edge
	{
		uint32_t a = 0, b = 0, count = 0;
	};
	std::array<std::map<EdgeKey, Edge>, 4> sides;
	for (size_t t = 0; t + 2 < mesh.indices.size (); t += 3)
		for (int e = 0; e < 3; ++e)
		{
			const uint32_t a = mesh.indices[t + e], b = mesh.indices[t + (e + 1) % 3];
			const auto	  &p = mesh.vertices[a].position, &q = mesh.vertices[b].position;
			if (p == q)
				continue;
			for (int side = 0; side < 4; ++side)
			{
				const int	axis = side / 2;
				const float plane = (side & 1) ? ChunkSize : 0;
				if (std::abs (p[axis] - plane) > VoxelSize || std::abs (q[axis] - plane) > VoxelSize)
					continue;
				auto &edge = sides[side][p < q ? EdgeKey{p, q} : EdgeKey{q, p}];
				edge.a = a;
				edge.b = b;
				++edge.count;
			}
		}
	constexpr double							step = double (ChunkSize) / TerrainTileCells;
	std::map<std::pair<int, int>, PlanetSample> samples;
	auto										sample = [&] (int x, int y) -> const PlanetSample &
	{
		const auto key = std::make_pair (x, y);
		auto	   it = samples.find (key);
		if (it == samples.end ())
			it = samples.emplace (key, SamplePlanetBackdrop (config, mesh.key.x, mesh.key.y, x * step, y * step)).first;
		return it->second;
	};
	auto backdrop = [&] (MeshVertex vertex, int side)
	{
		const int axis = side / 2;
		// Finish inside the adjacent tile. Its surface covers the small overlap;
		// bias the endpoint below that surface to prevent coplanar depth fighting.
		vertex.position[axis] = (side & 1) ? ChunkSize + 16 : -16;
		const double px = vertex.position[0] / step, py = vertex.position[1] / step;
		const int	 x = int (std::floor (px)), y = int (std::floor (py));
		const double u = px - x, v = py - y;
		const auto	&a = sample (x, y), &b = sample (x + 1, y), &c = sample (x, y + 1), &d = sample (x + 1, y + 1);
		// Match the exact diagonal used by BuildTerrainTile, not bilinear height.
		const double height =
			u < v ? a.height + u * (d.height - c.height) + v * (c.height - a.height) : a.height + u * (b.height - a.height) + v * (d.height - b.height);
		vertex.position[2] = float (height - double (mesh.key.z) * ChunkSize - 1.0 / 64.0);
		for (int k = 0; k < 3; ++k)
			vertex.normal[k] = float (
				u < v ? a.normal[k] + u * (d.normal[k] - c.normal[k]) + v * (c.normal[k] - a.normal[k])
					  : a.normal[k] + u * (b.normal[k] - a.normal[k]) + v * (d.normal[k] - b.normal[k]));
		return vertex;
	};
	for (int side = 0; side < 4; ++side)
		for (const auto &[key, edge] : sides[side])
		{
			if (edge.count != 1)
				continue;
			const uint32_t first = uint32_t (mesh.transitionVertices.size ());
			const auto	   a = mesh.vertices[edge.a], b = mesh.vertices[edge.b];
			mesh.transitionVertices.insert (mesh.transitionVertices.end (), {a, b, backdrop (a, side), backdrop (b, side)});
			// Open edges can face either direction on overhangs. The bridge is
			// deliberately double sided, with original interpolated surface normals.
			for (uint32_t i : {0u, 1u, 3u, 0u, 3u, 2u, 0u, 3u, 1u, 0u, 2u, 3u})
				mesh.transitionIndices.push_back (first + i);
			mesh.transitionCounts[side] += 12;
		}
}

void BuildPlanetSurfaceRegion (
	const WorldConfig &config, const TerrainNeighborhood &chunks, const std::array<int, 3> &firstCell, const std::array<int, 3> &cellCount, ChunkMesh &mesh)
{
	if (!chunks[13])
		throw std::invalid_argument ("Surface region requires its owning chunk");
	for (int axis = 0; axis < 3; ++axis)
		if (firstCell[axis] < 0 || cellCount[axis] < 1 || firstCell[axis] > VoxelsPerChunk - cellCount[axis])
			throw std::invalid_argument ("Surface region lies outside its owning chunk");
	SurfaceCache cache (firstCell[0] - 1, firstCell[1] - 1, cellCount[0] + 2, cellCount[1] + 2);
	if (chunks[13]->solids)
		for (const auto &solid : *chunks[13]->solids)
		{
			cache.solids.emplace_back (solid);
			SolidPoint offset{};
			if (!WorldDelta (config, WorldPosition{chunks[13]->key, {}}, solid.origin, offset, 16777216))
				throw std::runtime_error ("Solid placement outside extraction range");
			cache.offsets.push_back (offset);
		}
	mesh.key = chunks[13]->key;
	BuildFeatureSurface (
		{double (firstCell[0] * VoxelSize + 4), double (firstCell[1] * VoxelSize + 4), double (firstCell[2] * VoxelSize + 4)}, cellCount, VoxelSize,
		[&] (const SolidPoint &p) { return Sample (config, chunks, p, &cache); }, mesh,
		[&] (const SolidPoint &p) { return Sample (config, chunks, p, &cache, true).density; });
}

void BuildPlanetSurface (const WorldConfig &config, const TerrainNeighborhood &chunks, ChunkMesh &mesh)
{
	SurfaceCache cache;
	double		 low = 1e30, high = -1e30;
	if (config.generator != Generator::Empty)
	{
		for (int y = -1; y <= VoxelsPerChunk; ++y)
			for (int x = -1; x <= VoxelsPerChunk; ++x)
			{
				auto value =
					CachedColumn (config, chunks[13]->key, {double (x * VoxelSize + VoxelSize / 2), double (y * VoxelSize + VoxelSize / 2), 0}, &cache);
				low = std::min (low, value.density);
				high = std::max (high, value.density);
			}
	}
	chunks[13]->cells.Visit (
		[&] (int, int, int z, int size, Cell cell)
		{
			if (cell.IsSolid () && z * VoxelSize > high)
			{
				low = std::min (low, double (z * VoxelSize));
				high = (z + size) * VoxelSize;
			}
			if (config.generator != Generator::Empty && !cell.IsSolid () && (z + size) * VoxelSize < low)
				low = z * VoxelSize;
		});
	if (chunks[13]->solids)
		for (const auto &solid : *chunks[13]->solids)
		{
			cache.solids.emplace_back (solid);
			SolidPoint offset{};
			if (!WorldDelta (config, WorldPosition{chunks[13]->key, {}}, solid.origin, offset, 16777216))
				throw std::runtime_error ("Solid placement outside extraction range");
			cache.offsets.push_back (offset);
			for (const auto &point : solid.vertices)
			{
				low = std::min (low, point[2] - offset[2]);
				high = std::max (high, point[2] - offset[2]);
			}
		}
	int lo = int (std::clamp (std::floor ((low - (config.generator == Generator::Planet ? 64 : 0)) / VoxelSize) - 2, 0.0, double (VoxelsPerChunk)));
	int hi = int (std::clamp (std::ceil ((high + (config.generator == Generator::Planet ? 64 : 0)) / VoxelSize) + 2, 0.0, double (VoxelsPerChunk)));
	mesh.smooth = true;
	if (hi <= lo)
		return;
	BuildFeatureSurface (
		{double (VoxelSize / 2), double (VoxelSize / 2), double (lo * VoxelSize + VoxelSize / 2)}, {VoxelsPerChunk, VoxelsPerChunk, hi - lo}, VoxelSize,
		[&] (const SolidPoint &p) { return Sample (config, chunks, p, &cache); }, mesh,
		[&] (const SolidPoint &p) { return Sample (config, chunks, p, &cache, true).density; });
	BuildPlanetTransitions (config, mesh);
}
} // namespace quiver
