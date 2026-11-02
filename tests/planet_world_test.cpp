/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "octree_world.h"
#include "planet_generation.h"
#include "planet_surface.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace quiver;
namespace
{
void Check (bool pass, const char *message)
{
	if (!pass)
		throw std::runtime_error (message);
}
WorldConfig Config ()
{
	WorldConfig c;
	c.generator = Generator::Planet;
	c.planetSettings.version = 1; // Existing-world regression fixtures retain their original terrain.
	c.seed = 123;
	c.planetSettings.globalResolution = 64;
	c.residentLimit = 128;
	c.planet = std::make_shared<PlanetGenerator> (c.planetSettings, c.seed, 4);
	Check (c.planet->Prepare (), "prepare fixture");
	return c;
}
void WorldTests (const std::filesystem::path &directory)
{
	auto config = Config ();
	config.databasePath = (directory / "planet.sqlite").string ();
	ContentHash			 durable;
	std::vector<uint8_t> descriptor;
	const int64_t		 extent = config.planetSettings.extentChunks;
	const WorldPosition	 marker{{-1, extent, 512}, {24, 40, 80}};
	{
		World world (config);
		Check (world.LastError ().empty (), "open planet database");
		Check (world.Config ().generator == Generator::Planet && world.Config ().planet->Ready (), "prepared planet in world config");
		descriptor = world.DescriptorBytes ();
		Check (descriptor.size () == 88, "versioned planet descriptor size");
		WorldConfig decoded;
		Check (
			ApplyWorldDescriptor (decoded, descriptor) && decoded.generator == Generator::Planet && decoded.planetSettings == config.planetSettings &&
				decoded.seed == config.seed,
			"descriptor reconstructs generator identity");
		auto invalid = descriptor;
		invalid[52] = 255;
		Check (!ApplyWorldDescriptor (decoded, invalid) && decoded.planetSettings == config.planetSettings, "unsupported generator rejected transactionally");
		Check (world.CurrentRoot () == ContentHash{}, "pristine planet stores descriptor only");
		Check (CanonicalWorldKey (config, marker.chunk) == ChunkKey{extent - 1, 0, 512}, "horizontal wrapping preserves vertical coordinate");
		std::array<double, 3> delta;
		Check (
			WorldDelta (config, {{0, 0, 0}, {8, 8, 8}}, {{extent - 1, extent - 1, 0}, {1000, 1000, 8}}, delta) && delta == std::array<double, 3>{32, 32, 0},
			"nearest image across seam intersection");
		Check (world.Require (marker.chunk) && world.Find (marker.chunk) == world.Find ({extent - 1, 0, 512}), "canonical chunk resident identity");
		Check (world.Edit (marker, {7, Solid}), "edit wrapped high-altitude chunk");
		Check (world.Sample ({{extent - 1, 0, 512}, marker.local}) == Cell{7, Solid}, "edit visible through wrapped alias");
		TerrainProof proof;
		Check (world.GetTerrainProof (marker.chunk, proof) && ValidateTerrainProof (proof), "canonical planet proof");
		decoded.planet = config.planet;
		World client (decoded);
		Check (client.InstallTerrainProof (proof) && client.Sample (marker) == Cell{7, Solid}, "peer reconstructs planet and edit");
		Check (world.CreateSnapshot ("before", {1, 2, 3}), "planet named snapshot");
		auto original = world.CurrentRoot ();
		Check (world.Edit (marker, {2, Solid}) && world.CurrentRoot () != original, "planet edit changes content root");
		Check (
			world.RestoreSnapshot ("before") && world.CurrentRoot () == original && world.Require (marker.chunk) && world.Sample (marker) == Cell{7, Solid},
			"planet snapshot restores edit");
		World				reverse (decoded);
		const WorldPosition other{{15, 16, 512}, {16, 16, 16}};
		Check (world.Edit (other, {3, Solid}) && reverse.Edit (other, {3, Solid}) && reverse.Edit (marker, {7, Solid}), "opposite insertion order");
		Check (reverse.CurrentRoot () == world.CurrentRoot (), "content root independent of edit order");
		durable = world.CurrentRoot ();
		Check (world.Flush (), "flush planet world");
		// The plateau is deliberately horizontal, providing an independent
		// analytic check of generated geometry and collision at z=128.
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x)
					Check (world.Require ({x, y, z}), "load spawn neighborhood");
		auto trace = world.Trace ({{0, 0, 0}, {128, 128, 200}}, {0, 0, -128});
		Check (
			!trace.hitUnknown && trace.fraction > 0 && trace.fraction < 1 && std::abs (trace.end.local[2] - 128) < 0.2,
			"terrain collision matches spawn surface");
		auto mesh = world.Mesh ({0, 0, 0}, true);
		Check (mesh && !mesh->indices.empty (), "planet surface mesh generated");
		bool plateau = false;
		for (const auto &v : mesh->vertices)
			if (v.position[0] > 64 && v.position[0] < 192 && v.position[1] > 64 && v.position[1] < 192)
			{
				Check (std::abs (v.position[2] - 128) < 0.2, "rendered plateau agrees with collision");
				plateau = true;
			}
		Check (plateau, "plateau vertices found");
		// Occupancy must extend through multiple positive and negative Z chunks.
		const int64_t cx = 5000, cy = 6000;
		double		  height = config.planet->Height (cx, cy, 512, 512);
		int64_t		  cz = int64_t (std::floor (height / ChunkSize));
		Check (cz != 0, "multi-z fixture");
		Check (world.Require ({cx, cy, cz}) && world.Require ({cx, cy, cz - 1}) && world.Require ({cx, cy, cz + 1}), "load multi-z terrain");
		Check (
			world.Sample ({{cx, cy, cz - 1}, {512, 512, 512}}).IsSolid () && !world.Sample ({{cx, cy, cz + 1}, {512, 512, 512}}).IsSolid (),
			"multi-z occupancy");
	}
	WorldConfig reopenedConfig;
	reopenedConfig.databasePath = config.databasePath; // Persisted metadata wins over caller defaults.
	{
		World reopened (reopenedConfig);
		Check (
			reopened.LastError ().empty () && reopened.DescriptorBytes () == descriptor && reopened.CurrentRoot () == durable,
			"reopen planet with old default config");
		Check (
			reopened.Config ().planetSettings == config.planetSettings && reopened.Require (marker.chunk) && reopened.Sample (marker) == Cell{7, Solid},
			"durable generator and edit");
		Check (std::filesystem::exists (config.databasePath + ".planetcache"), "disposable global cache written");
	}
	std::filesystem::remove (config.databasePath + ".planetcache");
	{
		World regenerated (reopenedConfig);
		Check (
			regenerated.DescriptorBytes () == descriptor && regenerated.CurrentRoot () == durable &&
				regenerated.Config ().planet->GlobalHash () == config.planet->GlobalHash (),
			"cache deletion preserves world identity");
	}
}
void NaturalRockTests ()
{
	auto config = Config ();
	config.seed = 1;
	config.planet = std::make_shared<PlanetGenerator> (config.planetSettings, config.seed);
	Check (config.planet->Prepare (), "prepare natural rock fixture");
	World				world (config);
	const int64_t		x = 927770, y = 283214;
	double				height = config.planet->Height (x, y, 512, 512);
	ChunkKey			key{x, y, int64_t (std::floor (height / ChunkSize))};
	TerrainNeighborhood chunks;
	for (int z = -1; z <= 1; ++z)
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				ChunkKey neighbor{x + dx, y + dy, key.z + z};
				Check (world.Require (neighbor), "load natural rock neighborhood");
				chunks[size_t (z + 1) * 9 + size_t (dy + 1) * 3 + dx + 1] = world.Find (neighbor);
			}
	bool overhang = false;
	for (int py = 64; py < 960 && !overhang; py += 16)
		for (int px = 64; px < 960 && !overhang; px += 16)
		{
			double base = config.planet->Height (x, y, px, py) - key.z * double (ChunkSize);
			if (base < 80 || base > ChunkSize - 80)
				continue;
			double previous = SamplePlanetSurface (config, chunks, {double (px), double (py), base - 64}).density;
			for (int z = -60; z <= 64; z += 4)
			{
				auto sample = SamplePlanetSurface (config, chunks, {double (px), double (py), base + z});
				if (previous < 0 && sample.density > 0 && sample.normal[2] < 0)
					overhang = true;
				previous = sample.density;
			}
		}
	Check (overhang, "natural steep terrain contains downward-facing overhangs");
}
} // namespace
int main ()
{
	const auto directory =
		std::filesystem::temp_directory_path () / ("quiver-planet-world-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
	try
	{
		std::filesystem::create_directory (directory);
		WorldTests (directory);
		NaturalRockTests ();
		std::filesystem::remove_all (directory);
		std::cout << "planet world tests passed\n";
		return 0;
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what () << " (artifacts " << directory << ")\n";
		return 1;
	}
}
