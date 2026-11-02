#include "terrain_mesher.h"
#include "terrain_collision.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace quiver;

namespace
{
void Check (bool value, const char *message)
{
	if (!value)
		throw std::runtime_error (message);
}

ChunkMesh Smooth (World &world, ChunkKey key)
{
	Check (world.Require (key), "require smooth test chunk");
	auto blocks = world.Mesh (key, true);
	Check (bool (blocks), "build source block mesh");
	ChunkMesh									 mesh = *blocks;
	std::array<std::shared_ptr<const Chunk>, 27> chunks;
	for (int z = -1; z <= 1; ++z)
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
			{
				if ((x < 0 && key.x == INT64_MIN) || (x > 0 && key.x == INT64_MAX) || (y < 0 && key.y == INT64_MIN) || (y > 0 && key.y == INT64_MAX) ||
					(z < 0 && key.z == INT64_MIN) || (z > 0 && key.z == INT64_MAX))
					continue;
				chunks[(z + 1) * 9 + (y + 1) * 3 + x + 1] = world.Find ({key.x + x, key.y + y, key.z + z});
			}
	BuildSmoothTerrainMesh (world.Config (), chunks, mesh);
	return mesh;
}

void Validate (const ChunkMesh &mesh, bool upward)
{
	Check (mesh.smooth && mesh.indices.size () % 3 == 0, "indexed smooth mesh published");
	for (const auto &vertex : mesh.vertices)
	{
		double squared = 0;
		for (int axis = 0; axis < 3; ++axis)
		{
			Check (std::isfinite (vertex.position[axis]) && vertex.position[axis] >= 0 && vertex.position[axis] <= ChunkSize, "finite clipped position");
			Check (std::isfinite (vertex.normal[axis]), "finite normal");
			squared += double (vertex.normal[axis]) * vertex.normal[axis];
		}
		Check (std::abs (squared - 1) < 1e-5, "unit smooth normal");
		int total = 0;
		for (uint8_t weight : vertex.blend.weights)
			total += weight;
		Check (total >= 253 && total <= 257, "normalized packed material weights survive clipping");
		for (size_t channel = 0; channel < 3; ++channel)
			Check (vertex.blend.tint[channel] <= vertex.blend.tint[3], "tint remains premultiplied");
	}
	for (size_t i = 0; i < mesh.indices.size (); i += 3)
	{
		for (size_t j = 0; j < 3; ++j)
			Check (mesh.indices[i + j] < mesh.vertices.size (), "valid index");
		const auto			 &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]], &c = mesh.vertices[mesh.indices[i + 2]];
		std::array<double, 3> ab{}, ac{}, cross{};
		for (int axis = 0; axis < 3; ++axis)
		{
			ab[axis] = double (b.position[axis]) - a.position[axis];
			ac[axis] = double (c.position[axis]) - a.position[axis];
		}
		for (int axis = 0; axis < 3; ++axis)
			cross[axis] = ab[(axis + 1) % 3] * ac[(axis + 2) % 3] - ab[(axis + 2) % 3] * ac[(axis + 1) % 3];
		Check (cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] > 1e-12, "no zero-area triangle");
		if (upward)
			Check (cross[2] < 0, "procedural surface has clockwise exterior winding");
	}
}

using Position2 = std::array<int64_t, 2>;
using Segment = std::array<Position2, 2>;
std::set<Segment> Seam (const ChunkMesh &mesh, int axis, float plane)
{
	std::map<Segment, size_t> counts;
	for (size_t i = 0; i < mesh.indices.size (); i += 3)
		for (size_t edge = 0; edge < 3; ++edge)
		{
			const auto &a = mesh.vertices[mesh.indices[i + edge]].position;
			const auto &b = mesh.vertices[mesh.indices[i + (edge + 1) % 3]].position;
			if (a[axis] != plane || b[axis] != plane)
				continue;
			Segment segment;
			for (int j = 0; j < 2; ++j)
			{
				const int component = (axis + j + 1) % 3;
				segment[0][j] = std::llround (double (a[component]) * 1024);
				segment[1][j] = std::llround (double (b[component]) * 1024);
			}
			if (segment[0] > segment[1])
				std::swap (segment[0], segment[1]);
			if (segment[0] != segment[1])
				++counts[segment];
		}
	std::set<Segment> result;
	for (const auto &[segment, count] : counts)
		if (count & 1)
			result.insert (segment);
	return result;
}

void ProceduralTests ()
{
	WorldConfig config;
	config.smoothTerrain = false;
	config.seed = 987654321;
	World world (config);
	for (ChunkKey key : {ChunkKey{3, -2, 0}, ChunkKey{4, -2, 0}, ChunkKey{3, -1, 0}})
		Check (world.Require (key), "load adjacent terrain");
	const auto		before = world.Find ({3, -2, 0})->cells.Encode ();
	const ChunkMesh center = Smooth (world, {3, -2, 0}), right = Smooth (world, {4, -2, 0}), above = Smooth (world, {3, -1, 0});
	Validate (center, true);
	Validate (right, true);
	Validate (above, true);
	Check (!center.indices.empty (), "procedural mesh has surface");
	Check (center.vertices.size () < center.indices.size () / 2, "shared tetrahedral edges deduplicate vertices");
	Check (world.Find ({3, -2, 0})->cells.Encode () == before, "smoothing does not mutate authoritative voxel occupancy");
	bool fractionalHeight = false;
	for (const auto &vertex : center.vertices)
	{
		fractionalHeight |= std::abs (vertex.position[2] / VoxelSize - std::round (vertex.position[2] / VoxelSize)) > 0.05f;
		Check (vertex.blend == TerrainClassifyMaterial (1, true), "natural surface and interior use continuous cover without strata bands");
	}
	Check (fractionalHeight, "continuous surface removes quantized voxel terraces");
	Check (!Seam (center, 0, ChunkSize).empty () && Seam (center, 0, ChunkSize) == Seam (right, 0, 0), "matching complete X seam segments");
	Check (Seam (center, 1, ChunkSize) == Seam (above, 1, 0), "matching complete Y seam segments");

	World			missing (config);
	const ChunkMesh fallback = Smooth (missing, {3, -2, 0});
	Check (Seam (fallback, 0, ChunkSize) == Seam (center, 0, ChunkSize), "missing halo uses the same procedural boundary");
	for (ChunkKey key : {ChunkKey{INT64_MAX, INT64_MIN, 0}, ChunkKey{INT64_MIN, INT64_MAX, 0}, ChunkKey{9007199254740993LL, -9007199254740993LL, 0}})
		Validate (Smooth (world, key), true);
}

void EditTests ()
{
	WorldConfig config;
	config.generator = Generator::Empty;
	config.smoothTerrain = false;
	World world (config);
	Check (world.Require ({0, 0, 0}) && world.Require ({1, 0, 0}), "load edited neighbors");
	Check (world.Edit ({{0, 0, 0}, {1020, 132, 132}}, {7, Solid}), "place boundary voxel");
	Check (world.Edit ({{1, 0, 0}, {4, 132, 132}}, {8, Solid}), "place neighboring material voxel");
	const auto left = Smooth (world, {0, 0, 0}), right = Smooth (world, {1, 0, 0});
	Validate (left, false);
	Validate (right, false);
	Check (!left.indices.empty () && !right.indices.empty (), "isolated edited voxels survive extraction");
	Check (!Seam (left, 0, ChunkSize).empty () && Seam (left, 0, ChunkSize) == Seam (right, 0, 0), "edited cross-chunk shape has matching seams");
	for (const auto &mesh : {left, right})
		for (const auto &vertex : mesh.vertices)
			Check (
				vertex.position[1] >= 128 && vertex.position[1] <= 136 && vertex.position[2] >= 128 && vertex.position[2] <= 136,
				"single-voxel smoothing stays within edited cell bounds");
	Check (world.Edit ({{0, 0, 0}, {1020, 132, 132}}, {}), "remove boundary voxel");
	const auto emptyLeft = Smooth (world, {0, 0, 0});
	Validate (emptyLeft, false);
	Check (world.Edit ({{1, 0, 0}, {4, 132, 132}}, {}), "remove remaining voxel");
	const auto empty = Smooth (world, {1, 0, 0});
	Check (empty.vertices.empty () && empty.indices.empty (), "empty generator has no accidental terrain");
	Check (world.Edit ({{1, 0, 0}, {500, 500, 500}}, {4, Water}), "place visible liquid material");
	const auto liquid = Smooth (world, {1, 0, 0});
	Validate (liquid, false);
	Check (!liquid.indices.empty (), "existing visible liquid edits remain rendered");
}

void WindingTests ()
{
	WorldConfig config;
	config.generator = Generator::Empty;
	config.smoothTerrain = false;
	World isolated (config);
	Check (isolated.Require ({}), "load isolated voxel chunk");
	Check (isolated.Edit ({{}, {500, 500, 500}}, {3, Solid}), "place isolated voxel");
	const auto single = Smooth (isolated, {});
	Validate (single, false);
	Check (!single.indices.empty (), "isolated voxel has triangles");
	config.generator = Generator::Terrain;
	World terrain (config);
	Check (terrain.Require ({}), "load cavity terrain chunk");
	Check (terrain.Edit ({{}, {132, 132, 28}}, {}), "carve underground cavity");
	const auto cavity = Smooth (terrain, {});
	Validate (cavity, false);
	for (bool inward : {false, true})
	{
		const auto				   &mesh = inward ? cavity : single;
		const std::array<double, 3> center = inward ? std::array<double, 3>{132, 132, 28} : std::array<double, 3>{500, 500, 500};
		for (const auto &vertex : mesh.vertices)
		{
			if (inward && vertex.position[2] > 40)
				continue;
			double outward = 0;
			for (int axis = 0; axis < 3; ++axis)
				outward += vertex.normal[axis] * (vertex.position[axis] - center[axis]) * (inward ? -1 : 1);
			Check (outward > 0, "isolated voxel and cavity normals point toward empty space");
		}

		size_t				checked = 0;
		std::array<bool, 6> faces{};
		for (size_t i = 0; i < mesh.indices.size (); i += 3)
		{
			const auto &a = mesh.vertices[mesh.indices[i]].position, &b = mesh.vertices[mesh.indices[i + 1]].position,
					   &c = mesh.vertices[mesh.indices[i + 2]].position;
			if (inward && (a[2] > 40 || b[2] > 40 || c[2] > 40))
				continue;
			std::array<double, 3> ab{}, ac{}, outward{};
			for (int axis = 0; axis < 3; ++axis)
			{
				ab[axis] = double (b[axis]) - a[axis];
				ac[axis] = double (c[axis]) - a[axis];
				outward[axis] = ((double (a[axis]) + b[axis] + c[axis]) / 3 - center[axis]) * (inward ? -1 : 1);
			}
			double dot = 0;
			for (int axis = 0; axis < 3; ++axis)
			{
				const double cross = ab[(axis + 1) % 3] * ac[(axis + 2) % 3] - ab[(axis + 2) % 3] * ac[(axis + 1) % 3];
				dot += cross * outward[axis];
				if (std::abs (cross) > 1e-6)
					faces[axis * 2 + (cross < 0)] = true;
			}
			Check (dot < 0, "isolated voxels and cavities have clockwise exterior winding");
			++checked;
		}
		Check (
			checked > 0 && std::all_of (faces.begin (), faces.end (), [] (bool present) { return present; }), "exterior winding checked on all six directions");
	}
}

void InvalidTests ()
{
	WorldConfig									 config;
	ChunkMesh									 mesh;
	std::array<std::shared_ptr<const Chunk>, 27> chunks;
	bool										 rejected = false;
	try
	{
		BuildSmoothTerrainMesh (config, chunks, mesh);
	}
	catch (const std::runtime_error &)
	{
		rejected = true;
	}
	Check (rejected && mesh.vertices.empty () && !mesh.smooth, "missing center rejected without publishing partial output");
}

void RegionTests ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	config.smoothTerrain = false;
	config.seed = 987654321;
	World		   world (config);
	const ChunkKey key{3, -2, 0};
	Check (world.Require (key), "load region test chunk");
	TerrainNeighborhood chunks{};
	chunks[13] = world.Find (key);
	const auto full = Smooth (world, key);
	size_t	   compared = 0;
	for (int x : {0, 16, 96, 112})
	{
		ChunkMesh patch;
		BuildSmoothTerrainRegion (config, chunks, {x, 0, 0}, {16, 16, 32}, patch);
		Validate (patch, true);
		Check (!patch.indices.empty (), "bounded terrain region contains its surface");
		for (const auto &vertex : patch.vertices)
			Check (
				vertex.position[0] >= x * VoxelSize && vertex.position[0] <= (x + 16) * VoxelSize && vertex.position[1] <= 128 && vertex.position[2] <= 256,
				"region vertices clipped to exact tile bounds");
		// An independent scalar-field collision oracle validates clipped
		// triangles, not just their similarity to this implementation's mesh.
		for (size_t i = 0; i < patch.indices.size (); i += 33)
		{
			std::array<double, 3> center{}, minimum{};
			TerrainDensityPoint	  cell;
			for (int axis = 0; axis < 3; ++axis)
			{
				for (int corner = 0; corner < 3; ++corner)
					center[axis] += double (patch.vertices[patch.indices[i + corner]].position[axis]) / 3;
				cell[axis] = int (std::floor ((center[axis] - 4) / 8));
				minimum[axis] = cell[axis] * 8 + 4;
			}
			std::array<TerrainDensitySample, 8> samples;
			for (int corner = 0; corner < 8; ++corner)
				samples[corner] =
					TerrainSampleDensity (config, key, {cell[0] + (corner & 1), cell[1] + ((corner >> 1) & 1), cell[2] + ((corner >> 2) & 1)}, chunks);
			auto start = center;
			start[2] = minimum[2] + 9;
			const auto trace = TraceTerrainCube (samples, minimum, start, {0, 0, -10});
			double	   enter = 1;
			for (size_t interval = 0; interval < trace.count; ++interval)
				enter = std::min (enter, trace.intervals[interval].enter);
			Check (trace.count && std::abs (start[2] - 10 * enter - center[2]) < 0.00004, "region triangles match exact marching-tetra collision planes");
			++compared;
		}
		ChunkMesh adjacent;
		if (x < 112)
		{
			BuildSmoothTerrainRegion (config, chunks, {x + 16, 0, 0}, {16, 16, 32}, adjacent);
			const auto seam = Seam (patch, 0, (x + 16) * VoxelSize);
			Check (!seam.empty () && seam == Seam (adjacent, 0, (x + 16) * VoxelSize), "adjacent collision tiles share exact seam edges");
		}
		if (x == 112)
		{
			const auto edge = Seam (patch, 0, ChunkSize), fullEdge = Seam (full, 0, ChunkSize);
			for (const auto &segment : edge)
				Check (fullEdge.contains (segment), "region preserves original full-chunk boundary segments");
		}
	}
	Check (compared > 100, "region oracle covers multiple clipped patches");
	Check (world.Require ({}), "load flat boundary fixture");
	chunks[13] = world.Find ({});
	ChunkMesh below, above;
	BuildSmoothTerrainRegion (config, chunks, {0, 0, 0}, {16, 16, 8}, below);
	BuildSmoothTerrainRegion (config, chunks, {0, 0, 8}, {16, 16, 8}, above);
	Check (below.indices.empty () && !above.indices.empty (), "coplanar floors belong to exactly one adjacent region");

	for (const std::array<int, 3> first : {std::array<int, 3>{-1, 0, 0}, {113, 0, 0}})
	{
		bool rejected = false;
		try
		{
			ChunkMesh invalid;
			BuildSmoothTerrainRegion (config, chunks, first, {16, 16, 16}, invalid);
		}
		catch (const std::runtime_error &)
		{
			rejected = true;
		}
		Check (rejected, "invalid region bounds rejected");
	}
}

void MaterialTests ()
{
	Check (TerrainClassifyMaterial (1, true) == TerrainClassifyMaterial (2, true), "procedural soil and grass IDs share natural cover");
	Check (TerrainClassifyMaterial (2, false).weights[1] == 255, "explicit soil remains available in empty worlds");
	Check (TerrainClassifyMaterial (3, true).weights[2] == 255 && TerrainClassifyMaterial (4, true).weights[3] == 255, "rock and sand remain distinct");
	for (int64_t chunk : {INT64_MIN, INT64_MAX - 1, int64_t (-9007199254740993LL), int64_t (-1), int64_t (0), int64_t (9007199254740993LL)})
	{
		const float phase = TerrainMaterialChunkPhase (chunk);
		Check (phase >= 0 && phase < 8192, "material phase is bounded at huge canonical coordinates");
		Check (std::fmod (phase + ChunkSize, 8192.0f) == TerrainMaterialChunkPhase (chunk + 1), "material phase matches across adjacent chunks");
	}
	WorldConfig config;
	config.generator = Generator::Empty;
	World world (config);
	Check (world.Require ({0, 0, 0}) && world.Require ({1, 0, 0}), "load material seam chunks");
	Check (world.Edit ({{0, 0, 0}, {1020, 132, 132}}, {3, Solid}), "place rock at seam");
	Check (world.Edit ({{1, 0, 0}, {4, 132, 132}}, {4, Solid}), "place sand across seam");
	const auto left = Smooth (world, {}), right = Smooth (world, {1, 0, 0});
	Validate (left, false);
	Validate (right, false);
	using BlendSeam = std::map<Position2, TerrainMaterialBlend>;
	auto seam = [] (const ChunkMesh &mesh, float boundary)
	{
		BlendSeam result;
		for (const auto &vertex : mesh.vertices)
			if (vertex.position[0] == boundary)
			{
				const Position2 key{std::llround (vertex.position[1] * 1024), std::llround (vertex.position[2] * 1024)};
				auto [found, inserted] = result.emplace (key, vertex.blend);
				Check (inserted || found->second == vertex.blend, "coincident seam vertices have identical material weights");
			}
		return result;
	};
	const auto west = seam (left, ChunkSize), east = seam (right, 0);
	Check (!west.empty () && west == east, "clipped rock/sand weights match on both sides of chunk boundary");
	bool mixed = false;
	for (const auto &[position, blend] : west)
		mixed |= blend.weights[2] > 0 && blend.weights[3] > 0;
	Check (mixed, "material boundary interpolates rock and sand rather than choosing a triangle ID");
}
} // namespace

int main ()
{
	try
	{
		ProceduralTests ();
		EditTests ();
		WindingTests ();
		MaterialTests ();
		RegionTests ();
		InvalidTests ();
		std::cout << "terrain mesher tests passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << "terrain mesher test failed: " << error.what () << '\n';
		return 1;
	}
}
