#include "terrain_lod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace quiver;

namespace
{
void Check (bool value, const char *message)
{
	if (!value)
		throw std::runtime_error (message);
}

uint64_t Hash (const std::vector<uint8_t> &bytes)
{
	uint64_t hash = UINT64_C (14695981039346656037);
	for (uint8_t byte : bytes)
	{
		hash ^= byte;
		hash *= UINT64_C (1099511628211);
	}
	return hash;
}

void CheckUnitNormal (const std::array<float, 3> &normal)
{
	double squared = 0;
	for (float value : normal)
	{
		Check (std::isfinite (value), "terrain normals are finite");
		squared += double (value) * value;
	}
	Check (std::abs (squared - 1) < 1e-6, "terrain normals have unit length");
}

void NormalTests ()
{
	const std::array<float, 3> up{0, 0, 1};
	Check (TerrainSurfaceNormal (1, 0, 0, 128, 128) == up, "spawn plateau has an upward normal");
	Check (
		TerrainSurfaceNormal (1, 0, 0, -1, 0) == up && TerrainSurfaceNormal (1, 0, 0, 0, 1025) == up &&
			TerrainSurfaceNormal (1, 0, 0, std::numeric_limits<float>::quiet_NaN (), 0) == up &&
			TerrainSurfaceNormal (1, 0, 0, 0, std::numeric_limits<float>::infinity ()) == up,
		"invalid normal input has a finite fallback");
	bool positiveX = false, negativeX = false, positiveY = false, negativeY = false;
	for (uint64_t seed : {uint64_t (1), uint64_t (987654321), UINT64_MAX})
	{
		// An interior point of a single bilinear patch has an independently
		// calculable derivative from its four exact lattice heights.
		const double h00 = VoxelSize * TerrainHeightVoxels (seed, 3, -2, 0, 0);
		const double h10 = VoxelSize * TerrainHeightVoxels (seed, 3, -2, 32, 0);
		const double h01 = VoxelSize * TerrainHeightVoxels (seed, 3, -2, 0, 32);
		const double h11 = VoxelSize * TerrainHeightVoxels (seed, 3, -2, 32, 32);
		const auto	 center = TerrainSurfaceNormal (seed, 3, -2, 128, 128);
		Check (
			std::abs (center[0] / center[2] + ((h10 - h00) + (h11 - h01)) / 512) < 1e-7 &&
				std::abs (center[1] / center[2] + ((h01 - h00) + (h11 - h10)) / 512) < 1e-7,
			"normal points against the independently derived height gradient");
		for (ChunkKey key :
			 {ChunkKey{0, 0, 0}, ChunkKey{-1, -1, 0}, ChunkKey{-1000000000, 1000000000, 0}, ChunkKey{INT64_MIN, INT64_MAX - 1, 0},
			  ChunkKey{INT64_MAX - 1, INT64_MIN, 0}})
		{
			for (float y : {0.0f, 16.0f, 128.0f, 256.0f, 512.0f, 768.0f, 1024.0f})
				for (float x : {0.0f, 16.0f, 128.0f, 256.0f, 512.0f, 768.0f, 1024.0f})
				{
					const auto normal = TerrainSurfaceNormal (seed, key.x, key.y, x, y);
					CheckUnitNormal (normal);
					Check (normal[2] > 0.8f, "terrain normals remain upward at huge coordinates");
					positiveX |= normal[0] > 0.01f;
					negativeX |= normal[0] < -0.01f;
					positiveY |= normal[1] > 0.01f;
					negativeY |= normal[1] < -0.01f;
				}
			for (float local : {0.0f, 32.0f, 128.0f, 333.25f, 512.0f, 1024.0f})
			{
				Check (
					TerrainSurfaceNormal (seed, key.x, key.y, 1024, local) == TerrainSurfaceNormal (seed, key.x + 1, key.y, 0, local),
					"same world point has identical normals at X chunk seams");
				Check (
					TerrainSurfaceNormal (seed, key.x, key.y, local, 1024) == TerrainSurfaceNormal (seed, key.x, key.y + 1, local, 0),
					"same world point has identical normals at Y chunk seams");
			}
		}
		for (float x : {256.0f, 384.0f, 512.0f, 640.0f, 768.0f})
		{
			const auto left = TerrainSurfaceNormal (seed, 0, 0, x - 0.001f, 128);
			const auto right = TerrainSurfaceNormal (seed, 0, 0, x + 0.001f, 128);
			for (int axis = 0; axis < 3; ++axis)
				Check (std::abs (left[axis] - right[axis]) < 0.0001f, "smoothed normals are continuous across lattice and spawn blend boundaries");
		}
		CheckUnitNormal (TerrainSurfaceNormal (seed, INT64_MAX, INT64_MAX, 1024, 1024));
		CheckUnitNormal (TerrainSurfaceNormal (seed, INT64_MIN, INT64_MIN, 0, 0));
	}
	Check (positiveX && negativeX && positiveY && negativeY, "hills produce varied opposing slopes in both horizontal directions");
}

void SamplerTests ()
{
	struct Golden
	{
		uint64_t seed;
		ChunkKey key;
		size_t	 bytes;
		uint64_t hash;
	};
	// Captured from the original voxel generator before sharing its sampler.
	const Golden golden[] = {
		{1, {0, 0, 0}, 107361, UINT64_C (0xb80cbc4405c5ecb2)},
		{1, {-1, -1, 0}, 130869, UINT64_C (0xe6131b8889f62633)},
		{987654321, {-1000000000, 1000000000, 0}, 131553, UINT64_C (0x248bcb8011eabaf5)},
		{987654321, {INT64_MIN, INT64_MAX, 0}, 115281, UINT64_C (0xb024e065d20f2e7f)},
		{UINT64_MAX, {INT64_MAX, INT64_MIN, 0}, 132309, UINT64_C (0x6a3b6392cc12e020)},
		{UINT64_MAX, {0, -1, 0}, 117801, UINT64_C (0xabfcb8f8838cb74f)},
	};
	for (const auto &entry : golden)
	{
		WorldConfig config;
		config.seed = entry.seed;
		World world (config);
		Check (world.Require (entry.key), "generate sampler reference chunk");
		auto chunk = world.Find (entry.key);
		auto encoded = chunk->cells.Encode ();
		Check (encoded.size () == entry.bytes && Hash (encoded) == entry.hash, "voxel generator bytes remain compatible");
		for (int y = 0; y < VoxelsPerChunk; ++y)
			for (int x = 0; x < VoxelsPerChunk; ++x)
			{
				int h = TerrainHeightVoxels (entry.seed, entry.key.x, entry.key.y, x, y);
				Check (h >= 8 && h <= 16, "sampled height stays in generator range");
				Check (chunk->cells.Get (x, y, h - 1).IsSolid () && !chunk->cells.Get (x, y, h).IsSolid (), "shared sampler matches every voxel column");
			}
		if (entry.key.x < INT64_MAX)
			for (int y = 0; y <= VoxelsPerChunk; ++y)
				Check (
					TerrainHeightVoxels (entry.seed, entry.key.x, entry.key.y, 128, y) == TerrainHeightVoxels (entry.seed, entry.key.x + 1, entry.key.y, 0, y),
					"shared X endpoint");
		if (entry.key.y < INT64_MAX)
			for (int x = 0; x <= VoxelsPerChunk; ++x)
				Check (
					TerrainHeightVoxels (entry.seed, entry.key.x, entry.key.y, x, 128) == TerrainHeightVoxels (entry.seed, entry.key.x, entry.key.y + 1, x, 0),
					"shared Y endpoint");
	}
	Check (!TerrainHeightVoxels (1, 0, 0, -1, 0) && !TerrainHeightVoxels (1, 0, 0, 0, 129), "invalid sampler columns rejected");
}

void MeshTests (bool smooth)
{
	WorldConfig config;
	config.seed = 987654321;
	config.smoothTerrain = smooth;
	constexpr int Stride = TerrainTileCells + 1;
	for (int level = 0; level <= MaxTerrainTileLevel; ++level)
		for (bool extreme : {false, true})
		{
			const int64_t  span = int64_t (1) << level;
			TerrainTileKey key{extreme ? INT64_MIN : -span, extreme ? INT64_MAX - span + 1 : -2 * span, uint8_t (level)};
			auto		   mesh = BuildTerrainTile (config, key);
			Check (mesh && mesh->key == key, "valid terrain tile generated");
			Check (mesh->vertices.size () == 425 && mesh->indices.size () == 1920, "fixed bounded tile geometry");
			const double width = double (span) * ChunkSize;
			double		 projectedArea = 0;
			for (int y = 0; y < Stride; ++y)
				for (int x = 0; x < Stride; ++x)
				{
					const int64_t vx = span * VoxelsPerChunk * x / TerrainTileCells;
					const int64_t vy = span * VoxelsPerChunk * y / TerrainTileCells;
					const int64_t cx = std::min (vx / VoxelsPerChunk, span - 1), cy = std::min (vy / VoxelsPerChunk, span - 1);
					const int	  lx = int (vx - cx * VoxelsPerChunk), ly = int (vy - cy * VoxelsPerChunk);
					const float	  expected = smooth ? TerrainSurfaceHeight (config.seed, key.x + cx, key.y + cy, float (lx * VoxelSize), float (ly * VoxelSize))
													: float (VoxelSize * TerrainHeightVoxels (config.seed, key.x + cx, key.y + cy, lx, ly));
					Check (mesh->vertices[y * Stride + x].position[2] == expected, "far height follows the selected smooth or block terrain surface");
				}
			for (const auto &vertex : mesh->vertices)
			{
				CheckUnitNormal (vertex.normal);
				if (vertex.face == 5)
					Check (vertex.normal[2] > 0.8f, "far top vertices have upward slope normals");
				else
				{
					std::array<float, 3> expected{};
					expected[vertex.face / 2] = (vertex.face & 1) ? 1.0f : -1.0f;
					Check (vertex.normal == expected, "skirt normals point exactly outward");
				}
				for (int axis = 0; axis < 3; ++axis)
					Check (
						std::isfinite (vertex.position[axis]) && vertex.position[axis] >= mesh->boundsMin[axis] &&
							vertex.position[axis] <= mesh->boundsMax[axis],
						"finite local vertex inside bounds");
			}
			for (size_t i = 0; i < mesh->indices.size (); i += 3)
			{
				std::array<double, 3> a{}, b{}, cross{};
				for (int j = 0; j < 3; ++j)
				{
					Check (mesh->indices[i + j] < mesh->vertices.size (), "valid terrain triangle index");
					a[j] = double (mesh->vertices[mesh->indices[i + 1]].position[j]) - mesh->vertices[mesh->indices[i]].position[j];
					b[j] = double (mesh->vertices[mesh->indices[i + 2]].position[j]) - mesh->vertices[mesh->indices[i]].position[j];
				}
				for (int j = 0; j < 3; ++j)
					cross[j] = a[(j + 1) % 3] * b[(j + 2) % 3] - a[(j + 2) % 3] * b[(j + 1) % 3];
				const auto &vertex = mesh->vertices[mesh->indices[i]];
				const int	axis = vertex.face / 2, sign = vertex.face & 1 ? 1 : -1;
				Check (cross[axis] * sign < 0, "top and all four skirts use nondegenerate clockwise outward winding");
				if (vertex.face == 5)
				{
					projectedArea -= cross[2] * 0.5;
					Check (vertex.material == 2, "top material matches procedural surface");
				}
				else
					Check (vertex.material == 1, "skirt material matches terrain interior");
			}
			Check (projectedArea == width * width, "terrain top exactly covers tile square");
			for (int side = 0; side < 4; ++side)
				for (int i = 0; i < Stride; ++i)
				{
					const auto &top = mesh->vertices[Stride * Stride + side * Stride * 2 + i * 2].position;
					const auto &bottom = mesh->vertices[Stride * Stride + side * Stride * 2 + i * 2 + 1].position;
					Check (
						top[0] == bottom[0] && top[1] == bottom[1] && top[2] - bottom[2] == 128 && bottom[2] <= 0,
						"skirts span the full possible neighboring height range");
				}
		}
	config.generator = Generator::Empty;
	auto empty = BuildTerrainTile (config, {});
	Check (empty && empty->vertices.empty () && empty->indices.empty (), "empty worlds never acquire terrain");
	std::string error;
	Check (!BuildTerrainTile (config, {1, 0, 1}, &error) && !error.empty (), "unaligned tile rejected");
	Check (!BuildTerrainTile (config, {0, 0, 255}), "excessive tile level rejected");
	config.generator = static_cast<Generator> (123);
	Check (!BuildTerrainTile (config, {}), "invalid generator rejected");
}

void SeamTests (bool smooth)
{
	WorldConfig config;
	config.seed = UINT64_MAX;
	config.smoothTerrain = smooth;
	constexpr int Stride = TerrainTileCells + 1;
	for (int level : {0, 1, 4, 10, 14})
	{
		const int64_t span = int64_t (1) << level;
		auto		  west = BuildTerrainTile (config, {-span, -span, uint8_t (level)});
		auto		  east = BuildTerrainTile (config, {0, -span, uint8_t (level)});
		for (int y = 0; y < Stride; ++y)
		{
			Check (
				west->vertices[y * Stride + TerrainTileCells].position[2] == east->vertices[y * Stride].position[2],
				"equal-level tiles share exactly sampled boundary heights");
			Check (
				west->vertices[y * Stride + TerrainTileCells].normal == east->vertices[y * Stride].normal,
				"equal-level tiles share identical boundary normals");
		}
	}
	auto coarse = BuildTerrainTile (config, {-4, -4, 2});
	for (int half = 0; half < 2; ++half)
	{
		auto fine = BuildTerrainTile (config, {0, -4 + half * 2, 1});
		for (int y = 0; y < Stride; ++y)
		{
			const int	row = half * 8 + y / 2;
			const float exact = fine->vertices[y * Stride].position[2];
			float		other = coarse->vertices[row * Stride + TerrainTileCells].position[2];
			if (!(y & 1))
			{
				Check (exact == other, "mixed-level coincident vertices have identical heights");
				Check (
					fine->vertices[y * Stride].normal == coarse->vertices[row * Stride + TerrainTileCells].normal,
					"shared normals are independent of tile LOD");
			}
			else
				other = (other + coarse->vertices[(row + 1) * Stride + TerrainTileCells].position[2]) * 0.5f;
			Check (exact - 128 <= other && other - 128 <= exact, "skirts overlap across mixed-level interpolation gaps");
		}
	}
}

void SmoothHeightTests ()
{
	WorldConfig smooth;
	smooth.seed = 987654321;
	smooth.smoothTerrain = true;
	WorldConfig block = smooth;
	block.smoothTerrain = false;
	const auto smoothMesh = BuildTerrainTile (smooth, {3, -2, 0});
	const auto blockMesh = BuildTerrainTile (block, {3, -2, 0});
	bool	   fractionalHeight = false;
	for (size_t i = 0; i < size_t ((TerrainTileCells + 1) * (TerrainTileCells + 1)); ++i)
	{
		const float height = smoothMesh->vertices[i].position[2];
		fractionalHeight |= std::abs (height - blockMesh->vertices[i].position[2]) > 0.01f;
		Check (height >= 64 && height <= 128, "continuous heights preserve the procedural range");
	}
	Check (fractionalHeight, "smooth far terrain removes height quantization between lattice samples");
	const double h00 = VoxelSize * TerrainHeightVoxels (smooth.seed, 3, -2, 0, 0);
	const double h10 = VoxelSize * TerrainHeightVoxels (smooth.seed, 3, -2, 32, 0);
	const double h01 = VoxelSize * TerrainHeightVoxels (smooth.seed, 3, -2, 0, 32);
	const double h11 = VoxelSize * TerrainHeightVoxels (smooth.seed, 3, -2, 32, 32);
	const double expected = (h00 * 0.75 + h10 * 0.25) * 0.375 + (h01 * 0.75 + h11 * 0.25) * 0.625;
	Check (std::abs (TerrainSurfaceHeight (smooth.seed, 3, -2, 64, 160) - expected) < 0.00001, "continuous height retains the original bilinear terrain");
	Check (TerrainSurfaceHeight (smooth.seed, 0, 0, 128, 128) == 64, "continuous terrain preserves the spawn plateau");
	for (ChunkKey key : {ChunkKey{-1, -1, 0}, ChunkKey{INT64_MIN, INT64_MAX - 1, 0}, ChunkKey{INT64_MAX - 1, INT64_MIN, 0}})
		for (float local : {0.0f, 16.125f, 333.25f, 512.0f, 1024.0f})
		{
			Check (
				TerrainSurfaceHeight (smooth.seed, key.x, key.y, 1024, local) == TerrainSurfaceHeight (smooth.seed, key.x + 1, key.y, 0, local),
				"continuous X seam heights remain exact at negative and enormous coordinates");
			Check (
				TerrainSurfaceHeight (smooth.seed, key.x, key.y, local, 1024) == TerrainSurfaceHeight (smooth.seed, key.x, key.y + 1, local, 0),
				"continuous Y seam heights remain exact at negative and enormous coordinates");
		}
}

bool Contains (TerrainTileKey key, ChunkKey point)
{
	const uint64_t span = uint64_t (1) << key.level;
	return point.x >= key.x && point.y >= key.y && uint64_t (point.x) - uint64_t (key.x) < span && uint64_t (point.y) - uint64_t (key.y) < span;
}

void CheckCover (const WorldPosition &camera, double distance, const std::vector<TerrainTileKey> &tiles)
{
	Check (!tiles.empty () && tiles.size () <= MaxTerrainTiles, "bounded nonempty horizon cover");
	for (size_t i = 0; i < tiles.size (); ++i)
	{
		const auto	 &a = tiles[i];
		const int64_t as = int64_t (1) << a.level;
		Check (a.level <= MaxTerrainTileLevel && a.x % as == 0 && a.y % as == 0, "selected tile globally aligned");
		for (size_t j = 0; j < i; ++j)
		{
			const auto	 &b = tiles[j];
			const int64_t bs = int64_t (1) << b.level;
			Check (a.x + (as - 1) < b.x || b.x + (bs - 1) < a.x || a.y + (as - 1) < b.y || b.y + (bs - 1) < a.y, "selected tiles never overlap");
		}
	}
	for (int y = -8; y <= 8; ++y)
		for (int x = -8; x <= 8; ++x)
		{
			if (x * x + y * y > 64)
				continue;
			WorldPosition point;
			if (!Add (camera, {x * distance / 8, y * distance / 8, 0}, point))
				continue;
			Check (
				std::count_if (tiles.begin (), tiles.end (), [&] (auto key) { return Contains (key, point.chunk); }) == 1,
				"view circle has complete unique coverage");
		}
}

void SelectionTests ()
{
	for (WorldPosition camera :
		 {WorldPosition{{0, 0, 0}, {0, 0, 96}}, WorldPosition{{-1, -1, 0}, {1023.5f, 1023.5f, 96}},
		  WorldPosition{{-1000000000, 1000000000, 0}, {0.125f, 512.5f, 96}}, WorldPosition{{INT64_MIN, INT64_MAX, 0}, {0, 1023, 96}},
		  WorldPosition{{INT64_MAX, INT64_MIN, 0}, {1023, 0, 96}}})
		for (double distance : {1024.0, 4096.0, 32768.0, 65536.0})
		{
			auto tiles = SelectTerrainTiles (camera, distance);
			CheckCover (camera, distance, tiles);
			Check (tiles == SelectTerrainTiles (camera, distance), "selection is deterministic");
			auto high = camera;
			high.chunk.z = INT64_MAX;
			high.local[2] = 1023;
			Check (tiles == SelectTerrainTiles (high, distance), "terrain selection stays anchored at world z zero");
			for (int y = -4; y <= 4; ++y)
				for (int x = -4; x <= 4; ++x)
				{
					const double dx = x * double (ChunkSize) + 512 - camera.local[0], dy = y * double (ChunkSize) + 512 - camera.local[1];
					if (dx * dx + dy * dy > distance * distance)
						continue;
					WorldPosition point;
					if (Add (camera, {dx, dy, 0}, point))
						Check (
							std::any_of (tiles.begin (), tiles.end (), [&] (auto key) { return !key.level && Contains (key, point.chunk); }),
							"all visible near chunks retain level-zero fallback");
				}
		}
	WorldPosition camera{{-1, 0, 0}, {1000, 5, 96}};
	for (size_t budget : {size_t (1), size_t (4), size_t (16), size_t (64)})
	{
		auto tiles = SelectTerrainTiles (camera, 65536, 4, budget);
		Check (tiles.size () <= budget, "caller tile budget honored");
		if (!tiles.empty ())
			CheckCover (camera, 65536, tiles);
	}
	CheckCover (camera, MaxTerrainDistance, SelectTerrainTiles (camera, 1e30));
	Check (
		SelectTerrainTiles (camera, 0).empty () && SelectTerrainTiles (camera, std::numeric_limits<double>::quiet_NaN ()).empty () &&
			SelectTerrainTiles (camera, std::numeric_limits<double>::infinity ()).empty (),
		"invalid view radii rejected");
	camera.local[0] = 1024;
	Check (SelectTerrainTiles (camera, 32768).empty (), "noncanonical camera rejected");
}
void PlanetTileTests ()
{
	WorldConfig config;
	config.generator = Generator::Planet;
	config.planetSettings.globalResolution = 32;
	config.planetSettings.extentChunks = 512;
	config.planetSettings.regionalHydraulicIterations = 2;
	config.planetSettings.regionalThermalIterations = 2;
	Check (!BuildTerrainTile (config, {}), "unprepared planet rejected safely");
	config.planet = std::make_shared<PlanetGenerator> (config.planetSettings, 12345);
	Check (config.planet->Prepare (), "prepare planet LOD fixture");
	bool ocean = false, mountain = false;
	for (int x : {-1, 0, 16, 100, 255, 400, 511})
	{
		const auto a = BuildTerrainTile (config, {x, 0, 0});
		const auto b = BuildTerrainTile (config, {x + 512, 0, 0});
		Check (a && b && a->vertices.size () == b->vertices.size (), "periodic planet tiles build");
		Check (a->surfaceMin <= a->surfaceMax && a->boundsMin[2] < a->surfaceMin, "mountain bounds include skirts");
		mountain |= a->surfaceMax > ChunkSize || a->surfaceMin < -ChunkSize;
		ocean |= a->oceanIndexCount == 6;
		for (size_t i = 0; i < a->vertices.size (); ++i)
		{
			Check (
				a->vertices[i].position == b->vertices[i].position && a->vertices[i].normal == b->vertices[i].normal,
				"wrapped aliases have identical terrain and ocean vertices");
			CheckUnitNormal (a->vertices[i].normal);
		}
		// A visible subpixel skirt must inherit the adjacent terrain's lighting;
		// assigning an artificial vertical normal produced dotted dark seams.
		constexpr size_t topCount = (TerrainTileCells + 1) * (TerrainTileCells + 1);
		for (size_t i = topCount; i < topCount + 8 * (TerrainTileCells + 1); ++i)
		{
			const auto &skirt = a->vertices[i];
			auto		top = std::find_if (
				   a->vertices.begin (), a->vertices.begin () + topCount,
				   [&] (const auto &vertex) { return vertex.position[0] == skirt.position[0] && vertex.position[1] == skirt.position[1]; });
			Check (
				top != a->vertices.begin () + topCount && skirt.normal == top->normal && skirt.material == top->material,
				"planet seam shading matches terrain edge");
		}
		const auto right = BuildTerrainTile (config, {x + 1, 0, 0});
		for (int y = 0; y <= TerrainTileCells; ++y)
		{
			const auto &left = a->vertices[y * (TerrainTileCells + 1) + TerrainTileCells];
			const auto &next = right->vertices[y * (TerrainTileCells + 1)];
			Check (left.position[2] == next.position[2] && left.normal == next.normal, "planet LOD chunk endpoints match");
		}
	}
	Check (ocean && mountain, "planet LOD covers ocean and multi-z terrain");
}

} // namespace

int main ()
{
	try
	{
		SamplerTests ();
		NormalTests ();
		MeshTests (false);
		MeshTests (true);
		SeamTests (false);
		SeamTests (true);
		SmoothHeightTests ();
		SelectionTests ();
		PlanetTileTests ();
		std::cout << "terrain LOD tests passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << "terrain LOD test failed: " << error.what () << '\n';
		return 1;
	}
}
