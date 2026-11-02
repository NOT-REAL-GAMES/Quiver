/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "octree_world.h"
#include "world_solids.h"
#include "world_protocol.h"
#include "planet_surface.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <cmath>
#include <sqlite3.h>
using namespace quiver;
static void check (bool v, const char *what)
{
	if (!v)
		throw std::runtime_error (what);
}
int main ()
{
	WorldConfig config;
	config.generator = Generator::Empty;
	auto path = std::filesystem::temp_directory_path () /
				("quiver-solid-store-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()) + ".sqlite");
	std::string	 error;
	TerrainProof proof;
	{
		config.databasePath = path.string ();
		World			world (config);
		SolidDefinition box;
		check (MakeSolidBox ({{0, 0, 0}, {1020, 128, 128}}, {64, 64, 64}, {}, 7, box, &error), error.c_str ());
		check (world.AddSolid (box, &error), error.c_str ());
		check (world.GetTerrainProof ({0, 0, 0}, proof, &error), error.c_str ());
		check (ValidateTerrainProof (proof, &error), error.c_str ());
		check (proof.overrideObject && proof.overrideObject->bytes[0] == 3 && proof.assets.size () == 1, "Version3 content-addressed geometry");
		TerrainProof next;
		check (world.GetTerrainProof ({1, 0, 0}, next, &error), error.c_str ());
		check (next.assets.size () == 1 && next.assets[0].hash == proof.assets[0].hash, "Crosschunk asset reuse");
		box.origin = {{2, 0, 0}, {128, 128, 128}};
		check (world.AddSolid (box, &error), error.c_str ());
		check (world.GetTerrainProof ({2, 0, 0}, next, &error), error.c_str ());
		check (next.assets.size () == 1 && next.assets[0].hash == proof.assets[0].hash, "Repeated placement reuses geometry");
		auto bytes = wire::EncodeTerrainProof (proof);
		check (!bytes.empty (), "Proof encoding");
		TerrainProof decoded;
		check (wire::DecodeTerrainProof (bytes, decoded) && ValidateTerrainProof (decoded), "Proof wire roundtrip");
		auto clientConfig = config;
		clientConfig.databasePath.clear ();
		World client (clientConfig);
		check (client.InstallTerrainProof (decoded, &error), error.c_str ());
		check (client.Require ({0, 0, 0}), "client require");
		auto loaded = client.Find ({0, 0, 0});
		check (loaded && loaded->solids && loaded->solids->size () == 1, "Proof reconstructs solid definitions");
		auto bad = decoded;
		bad.assets.clear ();
		check (!ValidateTerrainProof (bad) && !client.InstallTerrainProof (bad), "Missing asset rejected");
		bad = decoded;
		bad.assets[0].bytes.back () ^= 1;
		check (!ValidateTerrainProof (bad), "Corrupt asset rejected");
		bad = decoded;
		bad.assets.push_back (bad.assets[0]);
		check (!ValidateTerrainProof (bad), "Duplicate asset rejected");
		// Carve a minimal voxel-sized brush from the top of the placed solid.
		check (world.Require ({2, 0, 0}), "carve target resident");
		const WorldPosition carve{{2, 0, 0}, {132, 132, 156}};
		const uint64_t		oldRevision = world.Find (carve.chunk)->revision;
		auto				beforeMesh = world.Mesh (carve.chunk, true);
		check (beforeMesh && !beforeMesh->indices.empty (), "solid mesh before carve");
		auto beforeHit = SweepFeatureMesh (*beforeMesh, {132, 132, 200}, {0, 0, -100});
		check (beforeHit.hit && std::abs (beforeHit.fraction - .4) < .002, "uncarved roof height");
		unsigned callbacks = 0;
		uint64_t callbackRevision = 0;
		bool	 callbackSuccess = false;
		check (
			world.EditSurfaceAsync (
				carve, {},
				[&] (bool success, uint64_t revision, const std::string &message)
				{
					++callbacks;
					callbackSuccess = success && message.empty ();
					callbackRevision = revision;
				},
				&error),
			error.c_str ());
		check (callbacks == 0, "surface edit callback is deferred");
		world.Pump (64);
		check (callbacks == 1 && callbackSuccess && callbackRevision == oldRevision + 1, "surface callback reports committed next revision once");
		world.Pump (64);
		check (callbacks == 1, "surface callback does not repeat");
		check (world.Require (carve.chunk), "carved chunk resident");
		auto carved = world.Find (carve.chunk);
		check (carved && carved->revision == oldRevision + 1 && carved->solids && carved->solids->size () == 2, "carve persists ordered subtractive operation");
		TerrainNeighborhood carvedNeighborhood{};
		carvedNeighborhood[13] = carved;
		check (SamplePlanetSurface (config, carvedNeighborhood, {132, 132, 156}).density < 0, "carve removes solid density");
		auto afterMesh = world.Mesh (carve.chunk, true);
		check (afterMesh && !afterMesh->indices.empty (), "solid mesh after carve");
		auto afterHit = SweepFeatureMesh (*afterMesh, {132, 132, 200}, {0, 0, -100});
		check (afterHit.hit && std::abs (afterHit.fraction - .48) < .002, "carve mesh exposes eight-unit lower cavity floor");
		check (
			world.GetTerrainProof (carve.chunk, next) && ValidateTerrainProof (next) && next.assets.size () == 2,
			"carve proof contains source and cut geometry assets");
		check (client.InstallTerrainProof (next, &error), error.c_str ());
		auto				remote = client.Find (carve.chunk);
		TerrainNeighborhood remoteNeighborhood{};
		remoteNeighborhood[13] = remote;
		check (remote && SamplePlanetSurface (config, remoteNeighborhood, {132, 132, 156}).density < 0, "peer proof reconstructs carved density");
		check (world.CreateSnapshot ("solid", {}, &error), error.c_str ());
		check (world.CollectGarbage (&error), error.c_str ());
		check (world.GetTerrainProof ({0, 0, 0}, next) && ValidateTerrainProof (next), "GC retains referenced asset");
	}
	{
		World reopened (config);
		check (reopened.Require ({0, 0, 0}), "reopen require");
		auto chunk = reopened.Find ({0, 0, 0});
		check (chunk && chunk->solids && chunk->solids->size () == 1, "Reopen restores solids");
		check (reopened.Require ({2, 0, 0}), "reopen carved chunk");
		auto				carved = reopened.Find ({2, 0, 0});
		TerrainNeighborhood loadedNeighborhood{};
		loadedNeighborhood[13] = carved;
		check (
			carved && carved->solids && carved->solids->size () == 2 && SamplePlanetSurface (config, loadedNeighborhood, {132, 132, 156}).density < 0,
			"reopen preserves carve and assets");
		check (reopened.RestoreSnapshot ("solid", &error), error.c_str ());
		check (reopened.GetTerrainProof ({0, 0, 0}, proof) && ValidateTerrainProof (proof), "Snapshot retains asset");
	}
	std::filesystem::remove (path);
	std::filesystem::remove (path.string () + "-wal");
	std::filesystem::remove (path.string () + "-shm");
	// Imported geometry is charged to resident memory before durable commit.
	{
		WorldConfig constrained;
		constrained.generator = Generator::Empty;
		constrained.residentBytes = 4096;
		World bounded (constrained);
		check (bounded.Require ({0, 0, 0}), "budget fixture resident");
		const auto		emptyBytes = bounded.Stats ().residentBytes;
		SolidDefinition fitting;
		check (MakeSolidBox ({{0, 0, 0}, {128, 128, 128}}, {32, 32, 32}, {}, 3, fitting), "fitting geometry");
		check (bounded.AddSolid (fitting, &error) && bounded.Require ({0, 0, 0}), "small solid fits budget");
		check (
			bounded.Stats ().residentBytes >= emptyBytes + fitting.vertices.size () * sizeof (SolidPoint) + fitting.faces.size () * sizeof (SolidFace),
			"resident stats include geometry arrays");
		SolidDefinition large;
		large.origin = {{0, 0, 0}, {256, 512, 512}};
		for (int i = 0; i < 16; ++i)
		{
			SolidDefinition piece;
			check (MakeSolidBox ({}, {16, 16, 16}, {}, 3, piece), "large asset component");
			const uint32_t base = uint32_t (large.vertices.size ());
			for (auto point : piece.vertices)
			{
				point[0] += i * 32;
				large.vertices.push_back (point);
			}
			for (auto face : piece.faces)
			{
				for (auto &v : face.vertices)
					v += base;
				large.faces.push_back (face);
			}
		}
		check (ValidateSolid (large, &error), error.c_str ());
		const auto root = bounded.CurrentRoot ();
		const auto revision = bounded.Find ({0, 0, 0})->revision;
		check (!bounded.AddSolid (large, &error), "oversized solid rejected before commit");
		check (
			bounded.CurrentRoot () == root && bounded.Require ({0, 0, 0}) && bounded.Find ({0, 0, 0})->revision == revision,
			"oversized rejection preserves durable root and resident revision");
		check (bounded.Stats ().residentBytes <= constrained.residentBytes, "resident geometry remains within budget");
	}
	{
		WorldConfig raceConfig;
		raceConfig.generator = Generator::Empty;
		World race (raceConfig);
		check (race.Require ({0, 0, 0}), "pending edit fixture");
		bool completed = false;
		check (
			race.EditAsync ({{0, 0, 0}, {64, 64, 64}}, {3, Solid}, [&] (bool ok, uint64_t, const std::string &) { completed = ok; }, &error), error.c_str ());
		SolidDefinition brush;
		check (MakeSolidBox ({{0, 0, 0}, {128, 128, 128}}, {32, 32, 32}, {}, 3, brush), "pending brush");
		check (!race.AddSolid (brush, &error) && error.find ("pending") != std::string::npos, "overlapping pending edit rejects solid transaction");
		check (!completed, "pending edit callback remains deferred");
		race.Pump (64);
		check (completed, "pending edit can finish");
		check (race.AddSolid (brush, &error) && race.Require ({0, 0, 0}), "solid retry after edit completion");
		check (
			race.Sample ({{0, 0, 0}, {64, 64, 64}}) == Cell{3, Solid} && race.Find ({0, 0, 0})->solids->size () == 1,
			"retry preserves both voxel and solid edit");
	}
	{
		WorldConfig rollbackConfig;
		rollbackConfig.generator = Generator::Empty;
		rollbackConfig.residentBytes = 800;
		World rollback (rollbackConfig);
		check (rollback.Require ({0, 0, 0}) && rollback.Require ({1, 0, 0}) && rollback.SetInterest ({{0, 0, 0}, {1, 0, 0}}), "pin two rollback chunks");
		SolidDefinition crossing;
		check (MakeSolidBox ({{0, 0, 0}, {1020, 128, 128}}, {64, 64, 64}, {}, 3, crossing), "crosschunk rollback brush");
		const auto root = rollback.CurrentRoot ();
		check (!rollback.AddSolid (crossing, &error), "aggregate pinned budget rejects multichunk transaction");
		check (
			rollback.CurrentRoot () == root && rollback.Find ({0, 0, 0})->revision == 0 && rollback.Find ({1, 0, 0})->revision == 0,
			"multichunk rollback retains both revisions");
		TerrainProof absent;
		check (
			rollback.GetTerrainProof ({0, 0, 0}, absent) && !absent.overrideObject && rollback.GetTerrainProof ({1, 0, 0}, absent) && !absent.overrideObject,
			"multichunk rollback leaves no partial durable overrides");
	}
	TerrainNeighborhood neighbors;
	for (int z = -1; z <= 1; ++z)
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
			{
				auto c = std::make_shared<Chunk> ();
				c->key = {x, y, z};
				neighbors[(z + 1) * 9 + (y + 1) * 3 + x + 1] = c;
			}
	SolidDefinition rotated;
	check (MakeSolidBox ({{0, 0, 0}, {512, 512, 512}}, {64, 64, 64}, {0, 31, 0}, 3, rotated), "benchmark box");
	auto center = std::make_shared<Chunk> ();
	center->key = {};
	center->solids = std::make_shared<const std::vector<SolidDefinition>> (std::vector<SolidDefinition>{rotated});
	neighbors[13] = center;
	ChunkMesh  mesh;
	const auto started = std::chrono::steady_clock::now ();
	BuildPlanetSurface (config, neighbors, mesh);
	const double milliseconds = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
	check (!mesh.indices.empty (), "Integrated solid feature surface");
	auto collision = SweepFeatureMesh (mesh, {512, 512, 600}, {0, 0, -200});
	check (collision.hit && std::abs (collision.fraction - .28) < .001, "Integrated feature collision matches roof");
	std::cout << "Rotated solid chunk mesh: " << milliseconds << " ms, " << mesh.indices.size () / 3 << " triangles\n";
	std::cout << "solid persistence/proof tests passed\n";
}
