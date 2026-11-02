/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "octree_world.h"
#include "world_solids.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace quiver;
namespace
{
void Check (bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error (message);
}
WorldPosition At (float x, float y, float z)
{
	WorldPosition result{{}, {x, y, z}};
	Check (Normalize (result), "normalize fixture");
	return result;
}
void Load (World &world, ChunkKey key = {})
{
	for (int z = -1; z <= 1; ++z)
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
				Check (world.Require ({key.x + x, key.y + y, key.z + z}), "load immutable fixture snapshots");
}
std::vector<CollisionPatchSnapshot>
Ready (World &world, const WorldPosition &at, const std::array<float, 3> &mins = {-16, -16, -24}, const std::array<float, 3> &maxs = {16, 16, 32})
{
	const auto							deadline = std::chrono::steady_clock::now () + std::chrono::seconds (20);
	std::vector<CollisionPatchSnapshot> patches;
	do
	{
		Check (world.CollisionPatchesAsync (at, {}, mins, maxs, patches), "bounded async query");
		if (std::all_of (patches.begin (), patches.end (), [] (const auto &p) { return !p.unknown && !p.pending && p.mesh; }))
			return patches;
		std::this_thread::yield ();
	} while (std::chrono::steady_clock::now () < deadline);
	throw std::runtime_error ("collision worker preparation deadline: " + world.LastError ());
}
void Compare (const ChunkMesh &actual, const ChunkMesh &expected)
{
	Check (actual.indices == expected.indices && actual.vertices.size () == expected.vertices.size (), "async extraction preserves surface topology");
	for (size_t i = 0; i < actual.vertices.size (); ++i)
		Check (
			actual.vertices[i].position == expected.vertices[i].position && actual.vertices[i].normal == expected.vertices[i].normal,
			"async extraction preserves exact positions and normals");
}
void GeometryAndOccupancy (Generator generator)
{
	WorldConfig config;
	config.generator = generator;
	config.planetSettings.globalResolution = 32;
	config.planetSettings.extentChunks = 512;
	World world (config), reference (config);
	Load (world);
	Load (reference);
	const auto							at = At (128, 128, generator == Generator::Planet ? 160 : 96);
	std::vector<CollisionPatchSnapshot> first;
	Check (world.CollisionPatchesAsync (at, {}, {-16, -16, -24}, {16, 16, 32}, first), "enqueue initial patches");
	Check (
		!first.empty () && std::all_of (first.begin (), first.end (), [] (const auto &p) { return p.pending && !p.unknown && !p.mesh && p.serial; }),
		"cold resident patches report pending, not unloaded");
	Check (
		world.Stats ().collisionBuilds == 0 && world.Stats ().collisionSyncBuilds == 0,
		"cold async request does not extract on the owner thread or publish worker data implicitly");
	Check (world.Stats ().collisionPending <= 32 && world.Stats ().collisionPendingBytes <= 256 * 1024 * 1024, "worker input budget bounded");
	const auto							ready = Ready (world, at);
	std::vector<CollisionPatchSnapshot> oracle;
	Check (reference.CollisionPatches (at, {}, {-16, -16, -24}, {16, 16, 32}, oracle), "offline synchronous reference");
	Check (ready.size () == oracle.size (), "same bounded query coverage");
	for (size_t i = 0; i < ready.size (); ++i)
		Compare (*ready[i].mesh, *oracle[i].mesh);
	const auto builds = world.Stats ().collisionWorkerBuilds;
	for (int i = 0; i < 50; ++i)
		Ready (world, at);
	Check (world.Stats ().collisionWorkerBuilds == builds && world.Stats ().collisionSyncBuilds == 0, "warm async queries reuse ready geometry");
	if (generator != Generator::Empty)
	{
		const float floor = generator == Generator::Planet ? 128 : 64;
		Ready (world, At (128, 128, floor), {-8, -8, -32}, {8, 8, 32});
		Check (world.CollisionContentsCached (At (128, 128, floor - 12)).IsSolid (), "prepared occupancy identifies buried center");
		Check (world.CollisionContentsCached (At (128, 128, floor + 12)).contents == Empty, "prepared occupancy identifies empty center");
	}
	const auto count = world.Stats ().collisionSyncBuilds;
	Check (world.CollisionContentsCached (WorldPosition{{500, 500, 0}, {128, 128, 128}}).contents == Unknown, "unloaded occupancy fails closed");
	Check (world.Stats ().collisionSyncBuilds == count, "cached occupancy never calls the synchronous mesher");
	std::vector<CollisionPatchSnapshot> missing;
	Check (world.CollisionPatchesAsync (WorldPosition{{500, 500, 0}, {128, 128, 128}}, {}, {}, {}, missing), "query unloaded chunks");
	Check (
		!missing.empty () && std::all_of (missing.begin (), missing.end (), [] (const auto &p) { return p.unknown && !p.pending && !p.serial; }),
		"unloaded chunks remain distinct from worker-pending data");
}
void RevisionsAndShutdown ()
{
	WorldConfig config;
	config.generator = Generator::Terrain;
	World world (config);
	Load (world);
	const auto							at = At (704, 704, 64);
	std::vector<CollisionPatchSnapshot> pending;
	Check (world.CollisionPatchesAsync (at, {}, {-16, -16, -16}, {16, 16, 32}, pending), "queue old revision");
	const auto old = world.Find ({});
	const auto oldSerial = world.CollisionSerial ({});
	Chunk	   replacement = *old;
	++replacement.revision;
	replacement.cells.Set (88, 88, 60, {3, Solid});
	Check (world.Install (replacement), "publish replacement while extraction is pending");
	Check (
		world.CollisionSerial ({}) != oldSerial && old->cells.Get (88, 88, 60) != replacement.cells.Get (88, 88, 60),
		"replication replaces immutable snapshots and invalidates neighborhood serials");
	const auto current = Ready (world, at, {-16, -16, -16}, {16, 16, 32});
	for (const auto &patch : current)
		Check (world.CollisionSerial (patch.key) == patch.serial, "stale asynchronous geometry is never published as current");
	Check (
		world.Stats ().collisionStaleResults > 0 && world.Stats ().collisionSyncBuilds == 0,
		"superseded jobs are canceled or rejected without synchronous fallback");
	Ready (world, At (708, 708, 484));
	Check (world.CollisionContentsCached (At (708, 708, 484)).IsSolid (), "replacement occupancy reaches publication");
	for (int round = 0; round < 3; ++round)
	{
		World transient (config);
		Load (transient);
		std::vector<CollisionPatchSnapshot> patches;
		Check (transient.CollisionPatchesAsync (At (512, 512, 512), {}, {-512, -512, -512}, {512, 512, 512}, patches), "saturate bounded queue");
		Check (
			transient.Stats ().collisionPending <= 32 && transient.Stats ().collisionPendingBytes <= 256 * 1024 * 1024,
			"large prefetch cannot exceed queued-work budgets");
		// Destruction cancels queued work and joins the active extractor. Ready
		// futures and immutable snapshots must not outlive their worker owner.
	}
}
void SolidOccupancy ()
{
	WorldConfig config;
	config.generator = Generator::Empty;
	World			world (config), reference (config);
	SolidDefinition floor;
	Check (MakeSolidBox (At (512, 512, 992.03125f), {512, 512, 64}, {}, 3, floor), "prepare non-lattice-aligned floor");
	Check (world.AddSolid (floor) && reference.AddSolid (floor), "install solid fixture");
	Load (world, {0, 0, 1});
	Load (reference, {0, 0, 1});
	const auto							ready = Ready (world, At (512, 512, 1024), {-32, -32, -32}, {32, 32, 32});
	std::vector<CollisionPatchSnapshot> oracle;
	Check (reference.CollisionPatches (At (512, 512, 1024), {}, {-32, -32, -32}, {32, 32, 32}, oracle), "solid oracle query");
	for (size_t i = 0; i < ready.size (); ++i)
		Compare (*ready[i].mesh, *oracle[i].mesh);
	Check (world.CollisionContentsCached (At (512, 512, 1024.015625f)).IsSolid (), "cached solid density preserves subvoxel plane position");
	Check (world.CollisionContentsCached (At (512, 512, 1024.125f)).contents == Empty, "cached solid density preserves air above plane");
	Check (world.Stats ().collisionSyncBuilds == 0, "construction preparation stays off owner thread");
}
} // namespace
int main ()
{
	try
	{
		GeometryAndOccupancy (Generator::Empty);
		GeometryAndOccupancy (Generator::Terrain);
		GeometryAndOccupancy (Generator::Planet);
		RevisionsAndShutdown ();
		SolidOccupancy ();
		std::cout << "Asynchronous world collision checks passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what () << '\n';
		return 1;
	}
}
