#include "octree_world.h"
#include "world_sha256.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

#include <sqlite3.h>

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace quiver;

namespace
{
void Check (bool condition, const char *description)
{
	if (!condition)
		throw std::runtime_error (description);
}

WorldPosition At (ChunkKey key, float x, float y, float z)
{
	return {key, {x, y, z}};
}

WorldConfig EmptyConfig (size_t resident = 32)
{
	WorldConfig config;
	config.generator = Generator::Empty;
	config.residentLimit = resident;
	return config;
}

void PositionTests ()
{
	WorldPosition position = At ({7, -5, INT64_MAX - 2}, -0.25f, -1024.0f, 2048.0f);
	Check (Normalize (position), "normalize negative offsets");
	Check (position.chunk == ChunkKey{6, -6, INT64_MAX}, "normalization uses floor rather than truncation");
	Check (position.local == std::array<float, 3>{1023.75f, 0, 0}, "normalized local offsets");
	WorldPosition untouched = position;
	position.local[2] = 1024;
	Check (!Normalize (position), "normalization rejects positive integer overflow");
	Check (position.chunk == untouched.chunk && position.local[2] == 1024, "failed normalization leaves input intact");
	position = At ({INT64_MIN, 0, 0}, -1, 0, 0);
	Check (!Normalize (position), "normalization rejects negative integer overflow");
	position = At ({0, 0, 0}, std::numeric_limits<float>::infinity (), 0, 0);
	Check (!Normalize (position), "normalization rejects infinity");
	position.local[0] = std::numeric_limits<float>::quiet_NaN ();
	Check (!Normalize (position), "normalization rejects NaN");

	WorldPosition origin = At ({INT64_MAX - 2, INT64_MIN + 2, 900000000000000000LL}, 1023, 1, 0.25f);
	WorldPosition moved;
	Check (Add (origin, {2.25, -2.25, -0.5}, moved), "far coordinates support precise local addition");
	Check (moved.chunk == ChunkKey{INT64_MAX - 1, INT64_MIN + 1, 899999999999999999LL}, "far chunk carry");
	Check (moved.local == std::array<float, 3>{1.25f, 1022.75f, 1023.75f}, "far local precision");
	std::array<double, 3> delta{};
	Check (Delta (moved, origin, delta), "local delta near integer limits");
	Check (delta == std::array<double, 3>{2.25, -2.25, -0.5}, "precise near-neighbor delta");
	Check (!Delta (At ({INT64_MIN, 0, 0}, 0, 0, 0), At ({INT64_MAX, 0, 0}, 0, 0, 0), delta), "far displacement does not wrap");
	Check (!Add (origin, {std::numeric_limits<double>::infinity (), 0, 0}, moved), "nonfinite addition rejected");

	std::mt19937 rng (1234);
	for (int i = 0; i < 10000; ++i)
	{
		int			  value = static_cast<int> (rng () % 200000) - 100000;
		WorldPosition p = At ({0, 0, 0}, static_cast<float> (value) / 4, 0, 0);
		Check (Normalize (p), "random normalization");
		Check (p.local[0] >= 0 && p.local[0] < ChunkSize, "local invariant");
		Check (static_cast<double> (p.chunk.x) * ChunkSize + p.local[0] == static_cast<double> (value) / 4, "normalization conserves position");
	}
}

void OctreeTests ()
{
	Octree tree;
	Check (tree.NodeCount () == 1, "empty tree is one node");
	Check (tree.Set (127, 0, 63, {4, Solid}), "edit splits leaf");
	Check (tree.Get (127, 0, 63) == Cell{4, Solid}, "split leaf sampled");
	Check (tree.Get (126, 0, 63).contents == Empty, "neighbor remains empty");
	Check (tree.NodeCount () == 57, "single edit creates only seven ancestor levels");
	Octree copy = tree;
	Check (tree.Set (127, 0, 63, {}), "erase solid voxel");
	Check (tree.NodeCount () == 1, "homogeneous siblings collapse to root");
	Check (copy.Get (127, 0, 63) == Cell{4, Solid}, "snapshots own independent trees");
	Check (!tree.Set (-1, 0, 0, {1, Solid}), "invalid voxel edit rejected");
	Check (tree.Get (128, 0, 0).contents == Unknown, "invalid voxel query blocks");

	std::mt19937 rng (991);
	for (int i = 0; i < 1000; ++i)
		copy.Set (rng () % 128, rng () % 128, rng () % 128, {static_cast<uint16_t> (1 + i % 5), i % 2 ? Solid : Water});
	auto   bytes = copy.Encode ();
	Octree decoded;
	Check (decoded.Decode (bytes), "octree binary roundtrip");
	Check (decoded.Encode () == bytes, "binary roundtrip preserves all leaves");
	for (size_t cut : {size_t (0), size_t (4), bytes.size () - 1})
	{
		auto truncated = bytes;
		truncated.resize (cut);
		Check (!decoded.Decode (truncated), "truncated encoding rejected");
		Check (decoded.Encode () == bytes, "invalid decode preserves previous tree");
	}
	auto trailing = bytes;
	trailing.push_back (0);
	Check (!decoded.Decode (trailing), "trailing encoding bytes rejected");
	std::vector<uint8_t> invalidDepth = {'Q', 'O', 'T', 1, 1, 1, 1, 1, 1, 1, 1, 1};
	Check (!decoded.Decode (invalidDepth), "excessively deep malformed tree rejected");
	tree.Fill ({7, Lava});
	Check (tree.Get (0, 127, 64) == Cell{7, Lava}, "liquid contents retained");
}

void CollisionTests ()
{
	World world (EmptyConfig ());
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			for (int z = -1; z <= 1; ++z)
				Check (world.Require ({x, y, z}), "load empty collision neighborhood");
	Check (world.Sample (At ({0, 0, 0}, 0, 0, 0)).contents == Empty, "empty generator has no floor");
	Check (world.Edit (At ({0, 0, 0}, 64, 64, 64), {1, Solid}), "place collision voxel");
	auto trace = world.Trace (At ({0, 0, 0}, 32, 68, 68), {64, 0, 0});
	Check (!trace.startSolid && trace.fraction > 0.49 && trace.fraction < 0.5, "ray stops just before voxel face");
	Check (trace.normal == std::array<float, 3>{-1, 0, 0}, "ray returns outward normal");
	Check (trace.end.local[0] < 64 && trace.end.local[0] > 63.9f, "ray epsilon does not penetrate");
	trace = world.Trace (At ({0, 0, 0}, 32, 68, 68), {64, 0, 0}, {-4, -4, -4}, {4, 4, 4});
	Check (trace.end.local[0] < 60 && trace.end.local[0] > 59.9f, "swept bounds stop before box contact");
	trace = world.Trace (At ({0, 0, 0}, 68, 68, 68), {1, 0, 0});
	Check (trace.startSolid && trace.allSolid && trace.fraction == 0, "stationary interior detected");
	trace = world.Trace (At ({0, 0, 0}, 68, 68, 68), {32, 0, 0});
	Check (trace.startSolid && !trace.allSolid, "startsolid can leave solid region");
	trace = world.Trace (At ({0, 0, 0}, 60, 68, 68), {0, 32, 0}, {-4, -4, -4}, {4, 4, 4});
	Check (!trace.startSolid && trace.fraction == 1, "touching a wall permits parallel sliding");
	Check (world.Edit (At ({0, 0, 0}, 72, 64, 64), {2, Solid}), "create a material boundary between solid leaves");
	trace = world.Trace (At ({0, 0, 0}, 72, 32, 68), {0, 64, 0});
	Check (trace.fraction > 0.49 && trace.fraction < 0.5 && trace.normal[1] == -1, "axis-aligned ray cannot slip between adjoining solid leaves");
	trace = world.Trace (At ({0, 0, 0}, 72, 68, 68), {0, 1, 0});
	Check (trace.startSolid && trace.allSolid, "ray on internal solid leaf boundary starts solid");
	Check (world.Edit (At ({0, 0, 0}, 72, 64, 64), {}), "remove adjacent test voxel");
	Check (world.Edit (At ({0, 0, 0}, 64, 64, 64), {3, Water}), "replace with liquid");
	trace = world.Trace (At ({0, 0, 0}, 32, 68, 68), {64, 0, 0});
	Check (trace.fraction == 1 && world.Sample (At ({0, 0, 0}, 68, 68, 68)).contents == Water, "water contents do not block movement");
	Check (world.Edit (At ({-1, 0, 0}, 1016, 64, 64), {1, Solid}), "edit negative neighboring chunk");
	trace = world.Trace (At ({0, 0, 0}, 16, 68, 68), {-32, 0, 0});
	Check (trace.normal[0] == 1 && trace.end.chunk.x == 0 && trace.end.local[0] > 0 && trace.end.local[0] < 0.1f, "negative seam collision");
	trace = world.Trace (At ({1, 0, 0}, 1000, 68, 68), {64, 0, 0});
	Check (trace.hitUnknown && trace.fraction < 1 && !trace.startSolid, "unloaded chunks block traversal");
	trace = world.Trace (At ({20, 0, 0}, 100, 100, 100), {});
	Check (trace.startSolid && trace.allSolid && trace.hitUnknown, "unknown starting region is blocked");
	trace = world.Trace (At ({0, 0, 0}, 100, 100, 100), {1e9, 0, 0});
	Check (trace.hitUnknown && trace.fraction == 0, "unbounded malicious trace rejected");

	ChunkKey distant{INT64_MAX - 5, INT64_MIN + 7, 900000000000000000LL};
	Check (world.Require (distant), "far chunk loads");
	Check (world.Edit (At (distant, 64, 64, 64), {1, Solid}), "far voxel edit");
	auto near = world.Trace (At (distant, 32, 68, 68), {64, 0, 0});
	Check (near.fraction > 0.49 && near.fraction < 0.5 && near.end.chunk == distant, "far collision retains local precision");
}

void MeshAndResidencyTests ()
{
	World world (EmptyConfig (3));
	Check (world.Require ({0, 0, 0}), "load mesh origin");
	Check (world.Edit (At ({0, 0, 0}, 1016, 64, 64), {1, Solid}), "boundary voxel");
	auto mesh = world.Mesh ({0, 0, 0}, true);
	Check (mesh && mesh->quads.size () == 6, "isolated voxel has six faces");
	uint64_t original = mesh->serial;
	Check (world.Require ({1, 0, 0}), "load neighboring chunk");
	Check (world.Edit (At ({1, 0, 0}, 0, 64, 64), {2, Solid}), "adjacent solid");
	mesh = world.Mesh ({0, 0, 0}, true);
	Check (mesh && mesh->serial != original && mesh->quads.size () == 5, "neighbor edit invalidates mesh and suppresses hidden face");
	for (const auto &quad : mesh->quads)
	{
		Check (quad.size == VoxelSize && quad.axis < 3 && std::abs (quad.sign) == 1, "face geometry contract");
		for (float value : quad.origin)
			Check (std::fmod (value, VoxelSize) == 0, "surface vertices conform across chunks");
	}
	Check (world.SetInterest ({{0, 0, 0}}), "pin origin chunk");
	Check (!world.SetInterest ({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}}), "oversized interest set refused");
	for (int x = 2; x != 20; ++x)
		Check (world.Require ({x, 0, 0}), "stream a long journey");
	Check (world.Stats ().residentChunks <= 3 && world.Find ({0, 0, 0}), "bounded cache preserves pinned chunks");
	Check (!world.Find ({1, 0, 0}), "distant unpinned chunk evicted");
	Check (world.Require ({1, 0, 0}), "reload transient edited chunk");
	Check (world.Sample (At ({1, 0, 0}, 0, 64, 64)) == Cell{2, Solid}, "transient edits survive cache eviction");

	Chunk network;
	network.key = {1, 0, 0};
	network.revision = 10;
	network.cells.Fill ({6, Water});
	Check (world.Install (network), "install authoritative snapshot");
	network.revision = 9;
	network.cells.Fill ({5, Lava});
	Check (world.Install (network), "older snapshot ignored without error");
	Check (world.Find ({1, 0, 0})->revision == 10 && world.Sample (At ({1, 0, 0}, 128, 128, 128)).contents == Water, "old snapshot cannot roll revision back");

	WorldConfig capped = EmptyConfig (1);
	capped.pendingLimit = 1;
	World queue (capped);
	Check (queue.Request ({0, 0, 0}), "first async request accepted");
	Check (!queue.Request ({1, 0, 0}), "pending queue is bounded");
	Check (queue.Require ({0, 0, 0}), "pending request completes");
}

void PersistenceTests (const std::filesystem::path &directory)
{
	const auto	   path = directory / "world.sqlite";
	const ChunkKey far{INT64_MIN + 1, INT64_MAX - 2, -900000000000000000LL};
	{
		World world (EmptyConfig (2));
		Check (world.Edit (At (far, 1023, 128, 16), {42, Slime}), "edit before first save");
		std::string error;
		Check (!world.SaveAs ((directory / "missing" / "failure.sqlite").string (), &error), "failed save reports error");
		Check (!error.empty (), "storage failure has actionable error");
		Check (world.Sample (At (far, 1023, 128, 16)) == Cell{42, Slime}, "failed save preserves active world");
		Check (world.SaveAs (path.string ()), "save transient database");
		Check (world.Edit (At (far, 1016, 136, 16), {43, Lava}), "named edit commits");
	}
	{
		WorldConfig config;
		config.databasePath = path.string ();
		World world (config);
		Check (world.Config ().generator == Generator::Empty, "reload metadata preserves generator");
		Check (world.Require (far), "reload persisted far chunk");
		Check (world.Find (far)->revision == 2, "durable revision restored");
		Check (world.Sample (At (far, 1023, 128, 16)) == Cell{42, Slime}, "pre-save edits survive reload");
		Check (world.Sample (At (far, 1016, 136, 16)) == Cell{43, Lava}, "post-save edits survive reload");
		Check (world.Flush (), "explicit checkpoint succeeds");
	}
#if defined(__unix__)
	const auto crashPath = directory / "crash.sqlite";
	pid_t	   child = fork ();
	Check (child >= 0, "spawn crash durability test");
	if (child == 0)
	{
		try
		{
			WorldConfig config = EmptyConfig ();
			config.databasePath = crashPath.string ();
			World world (config);
			if (!world.Edit (At ({9, -9, 0}, 100, 100, 100), {99, Solid}))
				_exit (2);
			_exit (0); // Deliberately skip the World destructor and WAL checkpoint.
		}
		catch (...)
		{
			_exit (3);
		}
	}
	int status = 0;
	Check (waitpid (child, &status, 0) == child && WIFEXITED (status) && WEXITSTATUS (status) == 0, "crash writer committed edit");
	WorldConfig config = EmptyConfig ();
	config.databasePath = crashPath.string ();
	World recovered (config);
	Check (recovered.Require ({9, -9, 0}), "recover WAL after abrupt exit");
	Check (recovered.Sample (At ({9, -9, 0}, 100, 100, 100)) == Cell{99, Solid}, "acknowledged edit survives abrupt exit");
#endif
}

void BudgetTests ()
{
	WorldConfig config = EmptyConfig (32);
	config.residentBytes = 4096;
	config.meshBytes = 384;
	World world (config);
	Check (world.SetInterest ({{0, 0, 0}}), "pin budgeted chunk");
	Check (world.Edit (At ({0, 0, 0}, 64, 64, 64), {1, Solid}), "small edit fits byte budget");
	Check (world.Mesh ({0, 0, 0}, true) != nullptr, "small mesh fits byte budget");
	Check (world.Stats ().residentBytes <= config.residentBytes && world.Stats ().meshBytes <= config.meshBytes, "resident byte accounting");
	for (int i = 0; i < 50; ++i)
	{
		Check (world.Require ({i + 1, 0, 0}), "byte limited streaming");
		Check (world.Stats ().residentBytes <= config.residentBytes, "tree cache stays within byte budget while travelling");
	}
	Chunk large;
	large.key = {100, 0, 0};
	large.revision = 1;
	for (int i = 0; i < 128; ++i)
		large.cells.Set (i, i, i, {1, Solid});
	Check (!world.Install (large), "individual oversized decoded chunk rejected");
	Check (!world.Find (large.key), "oversized chunk never published");
	Check (world.Find ({0, 0, 0}) != nullptr, "memory pressure does not evict pinned collision data");

	config.meshBytes = sizeof (MeshQuad);
	World tinyMesh (config);
	Check (tinyMesh.Edit (At ({0, 0, 0}, 64, 64, 64), {1, Solid}), "tiny mesh cache still accepts collision data");
	Check (!tinyMesh.Mesh ({0, 0, 0}, true), "mesh exceeding cache budget rejected");
	Check (!tinyMesh.LastError ().empty () && tinyMesh.Stats ().meshBytes == 0, "mesh memory limit reports explicit error");
	Check (!tinyMesh.Mesh ({0, 0, 0}) && tinyMesh.Stats ().pendingMeshes == 0, "failed mesh does not resubmit endlessly");
	Check (tinyMesh.MeshFootprint ({}).value_or (0) > config.meshBytes, "rejected footprint prevents repeated renderer admission");
	Check (tinyMesh.Edit (At ({}, 64, 64, 64), {}), "remove oversized geometry");
	Check (!tinyMesh.MeshFootprint ({}), "edit permits retry of a previously rejected footprint");
	Check (tinyMesh.Mesh ({}, true) && tinyMesh.MeshFootprint ({}) == 0, "built empty mesh has a known zero footprint");

	// Keep two meshes, touch the first, then add a third. The admission estimate
	// must survive eviction so a renderer can select a stable working set.
	config = EmptyConfig (8);
	config.meshBytes = 16 * sizeof (MeshQuad);
	World cache (config);
	for (int x : {0, 3, 6})
		Check (cache.Edit (At ({x, 0, 0}, 64, 64, 64), {1, Solid}), "prepare separated cache chunks");
	Check (!cache.MeshFootprint ({}) && !cache.MeshFootprint ({100, 0, 0}), "unknown meshes have no footprint");
	auto firstMesh = cache.Mesh ({}, true);
	auto secondMesh = cache.Mesh ({3, 0, 0}, true);
	Check (firstMesh && secondMesh, "initial mesh working set fits cache");
	const size_t				   secondBytes = secondMesh->Bytes ();
	std::weak_ptr<const ChunkMesh> evicted = secondMesh;
	secondMesh.reset ();
	Check (cache.Mesh ({}) == firstMesh, "mesh lookup refreshes cache recency");
	Check (cache.Mesh ({6, 0, 0}, true) != nullptr && evicted.expired (), "least recently used mesh is evicted");
	Check (cache.MeshFootprint ({3, 0, 0}) == secondBytes, "eviction retains footprint for renderer admission");
	Check (cache.Mesh ({}) == firstMesh && !cache.Stats ().pendingMeshes, "active mesh survives cache pressure without a rebuild");
}

void AsyncEditTests (const std::filesystem::path &directory)
{
	const auto	path = directory / "async.sqlite";
	WorldConfig config = EmptyConfig (8);
	config.databasePath = path.string ();
	World		  world (config);
	WorldPosition position = At ({1000000000, -1000000000, 0}, 64, 64, 64);
	Check (world.Edit (position, {1, Solid}), "prepare durable asynchronous edit baseline");
	const auto ownerThread = std::this_thread::get_id ();
	int		   callbacks = 0;
	Check (
		world.EditAsync (
			position, {2, Solid},
			[&] (bool success, uint64_t revision, const std::string &error)
			{
				Check (std::this_thread::get_id () == ownerThread, "async completion runs on owning thread");
				Check (success && revision == 2 && error.empty (), "async success reports durable revision");
				Check (world.Sample (position) == Cell{2, Solid}, "async publication precedes completion callback");
				++callbacks;
			}),
		"queue asynchronous durable edit");
	Check (!world.EditAsync (position, {3, Solid}, {}), "same-chunk asynchronous edits conflict until publication");
	Check (!world.Edit (position, {3, Solid}), "synchronous edit cannot overwrite pending asynchronous edit");
	Chunk conflict;
	conflict.key = position.chunk;
	conflict.revision = 50;
	Check (!world.Install (conflict), "network install cannot overwrite pending asynchronous edit");
	Check (world.Sample (position) == Cell{1, Solid}, "queued edit remains invisible before publication");
	Check (world.Stats ().pendingEdits == 1 && world.Stats ().pendingEditBytes > 0, "pending edit memory is accounted");
	Check (world.SetInterest ({{999, 999, 999}}), "change interest while edit commits");
	Check (world.Flush (), "wait for durable storage without pumping completions");
	Check (callbacks == 0 && world.Sample (position) == Cell{1, Solid}, "worker never publishes or calls completion directly");
	{
		World observer (config);
		Check (observer.Require (position.chunk), "read committed edit from independent database connection");
		Check (observer.Sample (position) == Cell{2, Solid}, "edit is durable before callback acknowledgment");
	}
	world.Pump (0);
	Check (callbacks == 0, "zero completion budget does not invoke callbacks");
	world.Pump (1);
	Check (callbacks == 1 && world.Stats ().pendingEdits == 0 && world.Stats ().pendingEditBytes == 0, "Pump completes asynchronous edit exactly once");
	world.Pump ();
	Check (callbacks == 1, "completed callback never repeats");

	Check (
		world.EditAsync (
			position, {3, Solid},
			[&] (bool success, uint64_t revision, const std::string &)
			{
				Check (success && revision == 3, "first reentrant edit committed");
				++callbacks;
				Check (
					world.EditAsync (
						position, {4, Solid},
						[&] (bool nextSuccess, uint64_t nextRevision, const std::string &)
						{
							Check (nextSuccess && nextRevision == 4, "callback can queue a subsequent edit");
							++callbacks;
						}),
					"reentrant enqueue sees released chunk reservation");
			}),
		"queue edit whose callback enqueues another");
	Check (world.Flush (), "commit reentrant first edit");
	world.Pump ();
	Check (callbacks == 2 && world.Stats ().pendingEdits == 1, "reentrant callback leaves next edit queued");
	Check (world.Flush (), "commit reentrant second edit");
	world.Pump ();
	Check (callbacks == 3 && world.Sample (position) == Cell{4, Solid}, "reentrant edit publication completes");

	sqlite3 *database = nullptr;
	Check (sqlite3_open (path.string ().c_str (), &database) == SQLITE_OK, "open failure injection database");
	Check (
		sqlite3_exec (
			database, "CREATE TRIGGER reject_edits BEFORE INSERT ON terrain_revisions BEGIN SELECT RAISE(ABORT,'injected write failure'); END", nullptr,
			nullptr, nullptr) == SQLITE_OK,
		"inject transactional write failure");
	bool failed = false;
	Check (
		world.EditAsync (
			position, {5, Solid},
			[&] (bool success, uint64_t revision, const std::string &error)
			{
				Check (
					!success && revision == 4 && error.find ("injected write failure") != std::string::npos,
					"async failure reports unchanged revision and storage error");
				failed = true;
			}),
		"queue edit before storage rejects it");
	Check (world.Flush (), "checkpoint after rejected transaction");
	Check (!failed, "storage failure callback is also deferred to Pump");
	world.Pump ();
	Check (failed && world.Sample (position) == Cell{4, Solid} && world.Find (position.chunk)->revision == 4, "failed commit never changes resident terrain");
	Check (sqlite3_exec (database, "DROP TRIGGER reject_edits", nullptr, nullptr, nullptr) == SQLITE_OK, "remove failure injection");
	sqlite3_close (database);
	{
		World observer (config);
		Check (observer.Require (position.chunk) && observer.Sample (position) == Cell{4, Solid}, "failed transaction preserves durable terrain");
	}

	WorldConfig boundedConfig = EmptyConfig (2);
	boundedConfig.pendingLimit = 2;
	World bounded (boundedConfig);
	Check (bounded.Require ({0, 0, 0}) && bounded.Require ({1, 0, 0}), "load bounded edit cache");
	int	 boundedCallbacks = 0;
	auto count = [&] (bool success, uint64_t, const std::string &)
	{
		Check (success, "bounded async edit commits");
		++boundedCallbacks;
	};
	Check (bounded.EditAsync (At ({0, 0, 0}, 64, 64, 64), {1, Solid}, count), "first bounded edit queued");
	Check (bounded.EditAsync (At ({1, 0, 0}, 64, 64, 64), {1, Solid}, count), "second bounded edit queued");
	Check (!bounded.EditAsync (At ({2, 0, 0}, 64, 64, 64), {1, Solid}, count), "bounded edit queue rejects excess work");
	Check (bounded.SetInterest ({}), "clear interests while edits are pending");
	Chunk incoming;
	incoming.key = {2, 0, 0};
	Check (!bounded.Install (incoming), "incoming chunks cannot evict pending edited chunks");
	Check (bounded.Find ({0, 0, 0}) && bounded.Find ({1, 0, 0}), "pending chunks stay resident without interest pins");
	Check (bounded.Flush (), "bounded writes finish");
	bounded.Pump (1);
	Check (boundedCallbacks == 1 && bounded.Stats ().pendingEdits == 1, "completion budget bounds edit publication");
	bounded.Pump (1);
	Check (boundedCallbacks == 2 && bounded.Stats ().pendingEdits == 0, "remaining edit completes next pump");

	WorldConfig manyConfig = EmptyConfig (65);
	manyConfig.pendingLimit = 128;
	World many (manyConfig);
	for (int x = 0; x < 65; ++x)
		Check (many.Require ({x, 0, 0}), "load 64-client edit test chunks");
	int manyCallbacks = 0;
	for (int x = 0; x < 64; ++x)
		Check (
			many.EditAsync (
				At ({x, 0, 0}, 64, 64, 64), {1, Solid},
				[&] (bool success, uint64_t revision, const std::string &)
				{
					Check (success && revision == 1, "64-client durable edit revision");
					++manyCallbacks;
				}),
			"accept 64 independent asynchronous edits");
	Check (many.Stats ().pendingEdits == 64 && manyCallbacks == 0, "64 edits stay queued until owning thread pumps");
	Check (!many.EditAsync (At ({64, 0, 0}, 64, 64, 64), {1, Solid}, {}), "hard 64-edit queue cap applies even with larger load limit");
	Check (many.Flush (), "finish 64 independent commits");
	for (int batch = 0; batch < 4; ++batch)
		many.Pump (16);
	Check (
		manyCallbacks == 64 && many.Stats ().pendingEdits == 0 && many.Stats ().pendingEditBytes == 0, "all 64 callbacks complete within bounded pump batches");

	bool destroyedCallback = false;
	config.databasePath = (directory / "async-destroy.sqlite").string ();
	{
		World destroyed (config);
		Check (destroyed.Require (position.chunk), "load destruction test chunk");
		Check (
			destroyed.EditAsync (position, {7, Solid}, [&] (bool, uint64_t, const std::string &) { destroyedCallback = true; }),
			"queue edit before destruction");
	}
	Check (!destroyedCallback, "destruction drains worker without invoking callbacks");
	World observer (config);
	Check (observer.Require (position.chunk) && observer.Sample (position) == Cell{7, Solid}, "destruction drains accepted durable writes");
}

void CheckpointTests (const std::filesystem::path &directory)
{
	WorldConfig config = EmptyConfig ();
	config.databasePath = (directory / "checkpoint.sqlite").string ();
	WorldPosition position = At ({900000000000000000LL, -900000000000000000LL, 0}, 64, 64, 64);
	{
		World legacy (config);
		Check (legacy.Edit (position, {1, Solid}), "prepare world predating checkpoint table");
	}
	sqlite3 *database = nullptr;
	Check (sqlite3_open (config.databasePath.c_str (), &database) == SQLITE_OK, "open checkpoint schema fixture");
	Check (sqlite3_exec (database, "DROP TABLE world_checkpoint", nullptr, nullptr, nullptr) == SQLITE_OK, "simulate old world without checkpoint schema");
	sqlite3_close (database);
	World				 world (config);
	std::vector<uint8_t> loaded = {99};
	Check (world.LoadCheckpoint (loaded) && loaded.empty (), "old world naturally has no checkpoint");
	Check (world.Require (position.chunk) && world.Sample (position) == Cell{1, Solid}, "checkpoint schema migration preserves terrain");
	const std::vector<uint8_t> standard = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	Check (world.SaveCheckpoint (standard), "save binary world checkpoint");
	Check (world.LoadCheckpoint (loaded) && loaded == standard, "binary checkpoint roundtrip");
	Check (sqlite3_open (config.databasePath.c_str (), &database) == SQLITE_OK, "open checkpoint inspection connection");
	sqlite3_stmt *statement = nullptr;
	Check (
		sqlite3_prepare_v2 (database, "SELECT checksum FROM world_checkpoint WHERE id=1", -1, &statement, nullptr) == SQLITE_OK, "inspect checkpoint checksum");
	Check (sqlite3_step (statement) == SQLITE_ROW && sqlite3_column_int64 (statement, 0) == 0xcbf43926LL, "checkpoint uses standard CRC32 checksum");
	sqlite3_finalize (statement);
	{
		std::vector<uint8_t> oversized (MaxCheckpointBytes + 1);
		Check (!world.SaveCheckpoint (oversized), "reject checkpoint above 64 MiB before saving");
		Check (world.LoadCheckpoint (loaded) && loaded == standard, "rejected oversized save preserves old checkpoint");
	}
	const std::vector<uint8_t> replacement = {0, 255, 0, 128, 64};
	Check (world.SaveCheckpoint (replacement), "atomically overwrite checkpoint with shorter binary payload");
	Check (world.LoadCheckpoint (loaded) && loaded == replacement, "overwrite preserves binary NUL bytes without trailing data");
	bool callback = false;
	Check (
		world.EditAsync (
			position, {2, Solid},
			[&] (bool success, uint64_t revision, const std::string &)
			{
				Check (success && revision == 2 && world.Sample (position) == Cell{2, Solid}, "terrain publication precedes checkpoint commit");
				std::vector<uint8_t> before;
				Check (world.LoadCheckpoint (before) && before == replacement, "checkpoint has not changed during terrain completion callback");
				callback = true;
			}),
		"queue terrain edit before explicit checkpoint save");
	Check (world.SaveCheckpoint (standard) && callback && world.Stats ().pendingEdits == 0, "checkpoint save drains and publishes terrain edits");
	Check (world.LoadCheckpoint (loaded) && loaded == standard, "checkpoint committed after terrain callback");
	Check (
		sqlite3_exec (
			database, "CREATE TRIGGER reject_checkpoint BEFORE INSERT ON world_checkpoint BEGIN SELECT RAISE(ABORT,'checkpoint failure'); END", nullptr,
			nullptr, nullptr) == SQLITE_OK,
		"inject checkpoint storage failure");
	Check (!world.SaveCheckpoint (replacement), "checkpoint transaction reports storage failure");
	Check (world.LoadCheckpoint (loaded) && loaded == standard, "failed checkpoint transaction preserves previous payload");
	Check (sqlite3_exec (database, "DROP TRIGGER reject_checkpoint", nullptr, nullptr, nullptr) == SQLITE_OK, "remove checkpoint failure injection");
	for (const char *corruption :
		 {"UPDATE world_checkpoint SET format=99", "UPDATE world_checkpoint SET bytes=-1", "UPDATE world_checkpoint SET bytes=67108865",
		  "UPDATE world_checkpoint SET bytes=1", "UPDATE world_checkpoint SET checksum=checksum+1", "UPDATE world_checkpoint SET checksum=-1",
		  "UPDATE world_checkpoint SET data='123456789'", "UPDATE world_checkpoint SET data=zeroblob(67108865),bytes=1"})
	{
		Check (sqlite3_exec (database, corruption, nullptr, nullptr, nullptr) == SQLITE_OK, "inject malformed checkpoint");
		loaded = {9, 8, 7};
		std::string error;
		Check (!world.LoadCheckpoint (loaded, &error) && !error.empty (), "malformed checkpoint rejected with error");
		Check (loaded == std::vector<uint8_t> ({9, 8, 7}), "failed checkpoint load leaves destination untouched");
		Check (world.SaveCheckpoint (standard), "repair malformed checkpoint through atomic save");
	}
	sqlite3_close (database);
	Check (world.SaveCheckpoint ({}), "empty checkpoint saves as a binary blob");
	Check (world.LoadCheckpoint (loaded) && loaded.empty (), "empty checkpoint roundtrip");
	Check (world.SaveCheckpoint (replacement), "restore checkpoint for backup test");
	const auto backupPath = directory / "checkpoint-copy.sqlite";
	Check (world.SaveAs (backupPath.string ()), "SaveAs copies terrain and checkpoint atomically");
	{
		WorldConfig copiedConfig = config;
		copiedConfig.databasePath = backupPath.string ();
		World copied (copiedConfig);
		Check (copied.LoadCheckpoint (loaded) && loaded == replacement, "SaveAs retains checkpoint");
		Check (copied.Require (position.chunk) && copied.Sample (position) == Cell{2, Solid}, "SaveAs retains committed terrain alongside checkpoint");
	}
}

void CheckpointCrashTest (const std::filesystem::path &directory)
{
#if defined(__unix__)
	const auto path = directory / "checkpoint-crash.sqlite";
	pid_t	   child = fork ();
	Check (child >= 0, "spawn checkpoint crash durability test");
	if (child == 0)
	{
		try
		{
			WorldConfig config = EmptyConfig ();
			config.databasePath = path.string ();
			World world (config);
			if (!world.Edit (At ({-9000000000LL, 7, -8}, 8, 16, 24), {3, Solid}) || !world.SaveCheckpoint ({0, 1, 255, 0, 2, 128}) ||
				!world.CreateSnapshot ("crash", {0, 1, 255, 0, 2, 128}))
				_exit (2);
			_exit (0); // No destructor or WAL checkpoint after the committed save.
		}
		catch (...)
		{
			_exit (3);
		}
	}
	int status = 0;
	Check (waitpid (child, &status, 0) == child && WIFEXITED (status) && WEXITSTATUS (status) == 0, "checkpoint writer committed before abrupt exit");
	WorldConfig config = EmptyConfig ();
	config.databasePath = path.string ();
	World				 recovered (config);
	std::vector<uint8_t> loaded;
	Check (recovered.LoadCheckpoint (loaded) && loaded == std::vector<uint8_t> ({0, 1, 255, 0, 2, 128}), "checkpoint recovers from WAL after abrupt exit");
	Check (
		recovered.RestoreSnapshot ("crash") && recovered.Require ({-9000000000LL, 7, -8}) &&
			recovered.Sample (At ({-9000000000LL, 7, -8}, 8, 16, 24)) == Cell{3, Solid},
		"snapshot Merkle root and objects recover after abrupt exit");
#else
	(void)directory;
#endif
}

void EntityPageTests (const std::filesystem::path &directory)
{
	WorldConfig config = EmptyConfig ();
	config.databasePath = (directory / "entity-pages.sqlite").string ();
	World					   world (config);
	const ChunkKey			   minimum{INT64_MIN, INT64_MIN, INT64_MIN}, maximum{INT64_MAX, INT64_MAX, INT64_MAX};
	const ChunkKey			   far{INT64_MIN, INT64_MAX, -900000000000000000LL};
	const std::vector<uint8_t> original{0, 1, 255, 0, 127}, changed{8, 6, 4};
	uint64_t				   id = world.AllocateEntityPageId (), revision = 999;
	Check (id == 1 && world.AllocateEntityPageId () == 2, "page IDs allocate monotonically and reserve unused IDs");
	Check (world.StoreEntityPage (id, far, original, 0, &revision) && revision == 1, "store first entity page with allocated ID");
	EntityPage page;
	Check (
		world.LoadEntityPage (id, page) && page.info.id == id && page.info.key == far && page.info.revision == 1 && page.bytes == original,
		"entity page retains binary bytes and exact extreme chunk coordinates");
	Check (!world.StoreEntityPage (id, {}, changed, 0, &revision) && revision == 1, "stale create fails without changing output revision");
	Check (!world.StoreEntityPage (0, {}, changed, 0), "zero page ID is reserved");
	Check (!world.StoreEntityPage (2, {}, std::vector<uint8_t> (MaxEntityPageBytes + 1), 0), "entity page size limited before storage");
	Check (world.StoreEntityPage (2, {}, {}, 0), "empty page stores as a binary blob");
	std::string error;
	auto		listed = world.ListEntityPages (minimum, maximum, 1, &error);
	Check (error.empty () && listed.size () == 1 && listed[0].id == 1 && listed[0].key == far, "bounded page list includes extreme signed coordinates");
	listed = world.ListEntityPages (minimum, maximum, 1, &error, 1);
	Check (error.empty () && listed.size () == 1 && listed[0].id == 2, "page list cursor advances in numeric ID order");
	Check (world.ListEntityPages (far, far, 256, &error).size () == 1, "inclusive exact chunk query at int64 limits");
	Check (world.ListEntityPages ({1, 0, 0}, {0, 0, 0}, 16, &error).empty () && !error.empty (), "reversed page bounds rejected");
	Check (world.ListEntityPages (minimum, maximum, 257, &error).empty () && !error.empty (), "page query cap enforced");
	Check (world.ListEntityPages (minimum, maximum, 0, &error).empty () && !error.empty (), "zero page query limit rejected");
	Check (world.SaveCheckpoint ({1, 2, 3}), "checkpoint pins current entity page versions");
	Check (world.StoreEntityPage (id, {5, 6, 7}, changed, 1, &revision) && revision == 2, "page update moves spatial index and increments revision");
	Check (world.ListEntityPages (far, far, 16).empty (), "moved page leaves old spatial index");
	Check (world.DeleteEntityPage (id, 2, &revision) && revision == 3, "page deletion creates a durable revision tombstone");
	page.bytes = {19};
	Check (!world.LoadEntityPage (id, page) && page.bytes == std::vector<uint8_t>{19}, "missing page leaves output unchanged");
	Check (!world.StoreEntityPage (id, far, original, 0), "tombstone prevents stale create ABA");
	uint64_t lateId = world.AllocateEntityPageId ();
	Check (lateId == 3 && world.StoreEntityPage (lateId, {}, changed, 0), "new page created after checkpoint");
	Check (world.RestoreEntityPagesCheckpoint (), "restore checkpoint page manifest");
	Check (
		world.LoadEntityPage (id, page) && page.info.revision == 1 && page.info.key == far && page.bytes == original,
		"checkpoint restores deleted page and its exact pinned version");
	Check (!world.LoadEntityPage (lateId, page), "checkpoint removes page created afterward");
	Check (
		world.StoreEntityPage (lateId, {}, changed, 0, &revision) && revision == 2,
		"restored live entity with no checkpoint page head receives a fresh revision after paging again");
	Check (world.DeleteEntityPage (lateId, revision), "remove restored-epoch page fixture");
	Check (world.AllocateEntityPageId () == 4, "checkpoint restore never rewinds ID allocator");
	Check (world.StoreEntityPage (id, far, changed, 1, &revision) && revision == 4, "revision clock never rewinds across checkpoint restore");
	Check (world.SaveCheckpoint ({4, 5, 6}), "replace checkpoint page manifest");
	sqlite3 *database = nullptr;
	Check (sqlite3_open (config.databasePath.c_str (), &database) == SQLITE_OK, "open page inspection database");
	auto countVersions = [&]
	{
		sqlite3_stmt *statement = nullptr;
		Check (sqlite3_prepare_v2 (database, "SELECT count(*) FROM entity_page_versions", -1, &statement, nullptr) == SQLITE_OK, "count page versions");
		Check (sqlite3_step (statement) == SQLITE_ROW, "read page version count");
		int count = sqlite3_column_int (statement, 0);
		sqlite3_finalize (statement);
		return count;
	};
	Check (countVersions () == 3, "checkpoint pruning retains only reachable current versions");
	for (int i = 0; i < 10; ++i)
		Check (world.StoreEntityPage (id, far, original, revision, &revision), "repeated page write succeeds");
	Check (countVersions () == 4, "repeated writes retain at most current and pinned checkpoint page versions");
	Check (
		sqlite3_exec (
			database, "CREATE TRIGGER reject_page BEFORE INSERT ON entity_page_versions BEGIN SELECT RAISE(ABORT,'page failure'); END", nullptr, nullptr,
			nullptr) == SQLITE_OK,
		"inject page transaction failure");
	uint64_t untouched = 812;
	Check (!world.StoreEntityPage (id, far, changed, revision, &untouched) && untouched == 812, "failed page transaction preserves revision output");
	Check (world.LoadEntityPage (id, page) && page.info.revision == revision && page.bytes == original, "failed transaction preserves durable page");
	Check (sqlite3_exec (database, "DROP TRIGGER reject_page", nullptr, nullptr, nullptr) == SQLITE_OK, "remove page failure injection");
	Check (world.StoreEntityPage (id, far, changed, revision, &revision), "page write works after rollback");
	Check (world.SaveAs ((directory / "entity-pages-copy.sqlite").string ()), "SaveAs includes current and checkpoint page versions and ID clocks");
	WorldConfig copiedConfig = config;
	copiedConfig.databasePath = (directory / "entity-pages-copy.sqlite").string ();
	World copied (copiedConfig);
	Check (copied.LoadEntityPage (id, page) && page.bytes == changed && page.info.revision == revision, "copied world has latest page");
	Check (
		copied.RestoreEntityPagesCheckpoint () && copied.LoadEntityPage (id, page) && page.info.revision == 4 && page.bytes == changed,
		"copied world also retains old checkpoint manifest");
	Check (copied.AllocateEntityPageId () == 5, "copied allocator continues from durable counter");
	sqlite3_close (database);

	World highIds (EmptyConfig ());
	Check (
		highIds.StoreEntityPage (1, {}, original, 0) && highIds.StoreEntityPage (UINT64_C (1) << 63, {}, original, 0) &&
			highIds.StoreEntityPage (UINT64_MAX, {}, original, 0),
		"full unsigned64 page IDs are accepted");
	listed = highIds.ListEntityPages (minimum, maximum, 256);
	Check (
		listed.size () == 3 && listed[0].id == 1 && listed[1].id == (UINT64_C (1) << 63) && listed[2].id == UINT64_MAX,
		"big-endian ID blobs sort in unsigned numeric order");
	Check (!highIds.AllocateEntityPageId (&error) && !error.empty (), "ID exhaustion rejects without wrapping to zero");
}

void EntityPageAsyncTests (const std::filesystem::path &directory)
{
	WorldConfig config = EmptyConfig ();
	config.databasePath = (directory / "entity-pages-async.sqlite").string ();
	World					   world (config);
	const ChunkKey			   minimum{INT64_MIN, INT64_MIN, INT64_MIN}, maximum{INT64_MAX, INT64_MAX, INT64_MAX};
	const std::vector<uint8_t> bytes{9, 0, 8};
	const auto				   owner = std::this_thread::get_id ();
	int						   callbacks = 0;
	Check (
		world.StoreEntityPageAsync (
			1, {5, -7, 9}, bytes, 0,
			[&] (bool success, uint64_t revision, const std::string &error)
			{
				Check (success && revision == 1 && error.empty () && std::this_thread::get_id () == owner, "page write callback runs on owner after commit");
				++callbacks;
			}),
		"queue asynchronous page store");
	Check (!world.StoreEntityPage (1, {}, bytes, 0) && !world.DeleteEntityPageAsync (1, 1, {}), "pending page rejects conflicting owner writes");
	Check (world.Flush () && callbacks == 0 && world.Stats ().pendingEntityPages == 1, "Flush commits pages without publishing callbacks");
	{
		World	   observer (config);
		EntityPage page;
		Check (observer.LoadEntityPage (1, page) && page.bytes == bytes, "page is durable before success callback");
	}
	world.Pump (0);
	Check (!callbacks, "zero Pump budget does not publish page callback");
	world.Pump (1);
	Check (callbacks == 1 && world.Stats ().pendingEntityPages == 0 && world.Stats ().pendingEntityPageBytes == 0, "page callback completes exactly once");
	Check (
		world.LoadEntityPageAsync (
			1,
			[&] (std::optional<EntityPage> page, const std::string &error)
			{
				Check (
					page && page->bytes == bytes && page->info.revision == 1 && error.empty () && std::this_thread::get_id () == owner,
					"async page load returns bounded data on owning thread");
				++callbacks;
			}),
		"queue async page load");
	Check (
		world.LoadEntityPageAsync (
			2,
			[&] (std::optional<EntityPage> page, const std::string &error)
			{
				Check (!page && error.empty (), "async missing page is an empty optional without failure");
				++callbacks;
			}),
		"queue missing page load");
	Check (
		world.ListEntityPagesAsync (
			minimum, maximum, 16,
			[&] (std::vector<EntityPageInfo> pages, const std::string &error)
			{
				Check (pages.size () == 1 && pages[0].id == 1 && error.empty (), "async spatial query lists durable page");
				++callbacks;
			}),
		"queue async page list");
	Check (world.Flush () && callbacks == 1, "async reads also wait for Pump publication");
	world.Pump (2);
	Check (callbacks == 3 && world.Stats ().pendingEntityPages == 1, "page read completions respect Pump count");
	world.Pump (1);
	Check (callbacks == 4, "remaining page read completes");
	Check (
		world.DeleteEntityPageAsync (
			1, 1,
			[&] (bool success, uint64_t revision, const std::string &error)
			{
				Check (success && revision == 2 && error.empty (), "async delete commits tombstone");
				++callbacks;
				Check (
					world.StoreEntityPageAsync (
						1, {}, bytes, revision,
						[&] (bool stored, uint64_t next, const std::string &failure)
						{
							Check (stored && next == 3 && failure.empty (), "page callback can enqueue next revision");
							++callbacks;
						}),
					"reentrant page write queues");
			}),
		"queue asynchronous deletion");
	Check (world.Flush (), "wait for async delete");
	world.Pump ();
	Check (callbacks == 5 && world.Stats ().pendingEntityPages == 1, "callback enqueues next page write after lock release");
	Check (
		world.SaveCheckpoint ({4}) && callbacks == 6 && world.Stats ().pendingEntityPages == 0, "checkpoint drains page commits and callbacks before pinning");
	bool failed = false;
	Check (
		world.StoreEntityPageAsync (
			1, {}, bytes, 1,
			[&] (bool success, uint64_t revision, const std::string &error)
			{
				Check (!success && revision == 1 && !error.empty (), "stale async CAS fails on storage worker");
				failed = true;
			}),
		"queue stale CAS request");
	Check (world.Flush (), "wait for stale page request");
	world.Pump ();
	Check (failed, "CAS error callback published");
	WorldConfig boundedConfig = EmptyConfig ();
	boundedConfig.pendingLimit = 128;
	World				 bounded (boundedConfig);
	std::vector<uint8_t> maximumPage (MaxEntityPageBytes, 7);
	for (uint64_t id = 1; id <= 64; ++id)
		Check (bounded.StoreEntityPageAsync (id, {}, maximumPage, 0, {}), "enqueue bounded page batch");
	Check (
		bounded.Stats ().pendingEntityPages == 64 && bounded.Stats ().pendingEntityPageBytes == 64 * MaxEntityPageBytes,
		"page queue caps retained payloads at64MiB");
	Check (!bounded.StoreEntityPageAsync (65, {}, maximumPage, 0, {}) && !bounded.LoadEntityPageAsync (1, {}), "hard64 page job cap includes reads");
	Check (bounded.Flush (), "commit maximum page queue");
	bounded.Pump (64);
	Check (!bounded.Stats ().pendingEntityPages && !bounded.Stats ().pendingEntityPageBytes, "page queue releases memory after completion");
	bool destroyedCallback = false;
	config.databasePath = (directory / "entity-pages-destroy.sqlite").string ();
	{
		World destroyed (config);
		Check (
			destroyed.StoreEntityPageAsync (1, {}, bytes, 0, [&] (bool, uint64_t, const std::string &) { destroyedCallback = true; }),
			"queue page before destruction");
	}
	World	   observer (config);
	EntityPage page;
	Check (!destroyedCallback && observer.LoadEntityPage (1, page) && page.bytes == bytes, "destruction drains page writes without callbacks");
}

void EntityPageCorruptionTests (const std::filesystem::path &directory)
{
	WorldConfig config = EmptyConfig ();
	config.databasePath = (directory / "entity-pages-corruption.sqlite").string ();
	const std::vector<uint8_t> bytes{1, 2, 0, 3};
	{
		World	 world (config);
		uint64_t revision = 0;
		Check (world.StoreEntityPage (1, {}, bytes, revision, &revision), "prepare corrupt page fixture");
		sqlite3 *database = nullptr;
		Check (sqlite3_open (config.databasePath.c_str (), &database) == SQLITE_OK, "open page corruption fixture");
		for (const char *assignment :
			 {"format=99", "bytes=-1", "bytes=1048577", "bytes=1", "checksum=checksum+1", "checksum=-1", "x='invalid'", "data='text'",
			  "data=zeroblob(2097152),bytes=4", "deleted=2"})
		{
			std::string sql = "UPDATE entity_page_versions SET " + std::string (assignment);
			Check (sqlite3_exec (database, sql.c_str (), nullptr, nullptr, nullptr) == SQLITE_OK, "inject page corruption");
			EntityPage page;
			page.bytes = {99};
			std::string error;
			Check (
				!world.LoadEntityPage (1, page, &error) && !error.empty () && page.bytes == std::vector<uint8_t>{99},
				"malformed page rejected before publication");
			Check (world.StoreEntityPage (1, {}, bytes, revision, &revision), "repair malformed data through new page revision");
		}
		Check (
			sqlite3_exec (
				database, "CREATE TRIGGER reject_manifest BEFORE INSERT ON entity_page_checkpoint BEGIN SELECT RAISE(ABORT,'manifest failure'); END", nullptr,
				nullptr, nullptr) == SQLITE_OK,
			"inject checkpoint manifest failure");
		Check (!world.SaveCheckpoint ({1, 2}), "manifest and checkpoint commit fail together");
		std::vector<uint8_t> checkpoint{5};
		Check (world.LoadCheckpoint (checkpoint) && checkpoint.empty (), "failed manifest transaction leaves checkpoint unchanged");
		Check (sqlite3_exec (database, "DROP TRIGGER reject_manifest", nullptr, nullptr, nullptr) == SQLITE_OK, "remove manifest failure injection");
		sqlite3_close (database);
	}
#if defined(__unix__)
	config.databasePath = (directory / "entity-pages-crash.sqlite").string ();
	pid_t child = fork ();
	Check (child >= 0, "fork page durability writer");
	if (child == 0)
	{
		try
		{
			World	 durable (config);
			uint64_t id = durable.AllocateEntityPageId ();
			if (!id || !durable.StoreEntityPage (id, {INT64_MIN, INT64_MAX, 0}, bytes, 0) || !durable.SaveCheckpoint ({8}) || !durable.DeleteEntityPage (id, 1))
				_exit (2);
			_exit (0);
		}
		catch (...)
		{
			_exit (3);
		}
	}
	int status = 0;
	Check (waitpid (child, &status, 0) == child && WIFEXITED (status) && !WEXITSTATUS (status), "page writer committed before abrupt exit");
	World	   recovered (config);
	EntityPage page;
	Check (
		!recovered.LoadEntityPage (1, page) && recovered.RestoreEntityPagesCheckpoint () && recovered.LoadEntityPage (1, page) && page.bytes == bytes,
		"WAL recovers page tombstone and retained checkpoint data after abrupt exit");
	Check (recovered.AllocateEntityPageId () == 2, "ID reservation survives abrupt exit");
#endif
}

void TerrainTests ()
{
	WorldConfig config;
	config.seed = 987654321;
	World first (config);
	World second (config);
	Check (first.Require ({-1000000000, 1000000000, 0}) && second.Require ({-1000000000, 1000000000, 0}), "generate distant terrain");
	Check (
		first.Find ({-1000000000, 1000000000, 0})->cells.Encode () == second.Find ({-1000000000, 1000000000, 0})->cells.Encode (),
		"terrain deterministic from canonical chunk and seed");
	Check (first.Require ({0, 0, 0}), "generate starting platform");
	Check (first.Sample (At ({0, 0, 0}, 128, 128, 63)).IsSolid (), "starting platform floor exists");
	Check (first.Sample (At ({0, 0, 0}, 128, 128, 64)).contents == Empty, "starting platform surface is 64 units");
	auto floor = first.Trace (At ({0, 0, 0}, 128, 128, 128), {0, 0, -100}, {-16, -16, -24}, {16, 16, 32});
	Check (!floor.startSolid && floor.normal[2] == 1 && floor.end.local[2] > 88 && floor.end.local[2] < 88.1f, "standing player rests on generated floor");
	auto mesh = first.Mesh ({0, 0, 0}, true);
	Check (mesh && mesh->smooth && !mesh->indices.empty () && mesh->quads.empty (), "terrain async mesh uses marching tetrahedra");
	Check (first.Stats ().meshBytes == mesh->Bytes () && first.Stats ().meshTriangles == mesh->indices.size () / 3, "smooth mesh arrays are fully accounted");
}

void SmoothMeshLifecycleTests (const std::filesystem::path &directory)
{
	WorldConfig config;
	config.seed = 4242;
	config.databasePath = (directory / "smooth.sqlite").string ();
	std::vector<MeshVertex> savedVertices;
	std::vector<uint32_t>	savedIndices;
	{
		World world (config);
		Check (world.Require ({}), "smooth origin loads");
		auto mesh = world.Mesh ({}, true);
		Check (mesh && mesh->smooth && !mesh->indices.empty (), "default terrain is smooth");
		uint64_t serial = mesh->serial;
		Check (world.Require ({1, 1, 1}), "diagonal halo arrives");
		mesh = world.Mesh ({}, true);
		Check (mesh && mesh->serial != serial, "corner neighbor arrival invalidates smooth mesh");
		serial = mesh->serial;
		Check (world.Edit (At ({1, 1, 1}, 0, 0, 0), {3, Solid}), "corner neighbor edit commits");
		mesh = world.Mesh ({}, true);
		Check (mesh && mesh->serial != serial, "corner neighbor edit invalidates smooth mesh");
		world.SetSmoothTerrain (false);
		Check (!world.MeshFootprint ({}), "mesher toggle clears previous size estimate");
		mesh = world.Mesh ({}, true);
		Check (mesh && !mesh->smooth && !mesh->quads.empty () && mesh->vertices.empty () && mesh->indices.empty (), "live toggle restores block mesh");
		world.SetSmoothTerrain (true);
		Check (world.Edit (At ({}, 128, 128, 32), {}), "underground excavation commits");
		mesh = world.Mesh ({}, true);
		Check (mesh && mesh->smooth && !mesh->indices.empty (), "live toggle rebuilds edited smooth mesh");
		bool cave = false;
		for (const auto &v : mesh->vertices)
			cave |= v.position[0] >= 120 && v.position[0] <= 144 && v.position[1] >= 120 && v.position[1] <= 144 && v.position[2] < 48;
		Check (cave, "excavation creates a closed underground surface");
		savedVertices = mesh->vertices;
		savedIndices = mesh->indices;
		Check (world.Flush (), "edited smooth world flushes");
	}
	{
		World restored (config);
		Check (restored.Require ({}) && restored.Require ({1, 1, 1}), "saved density source reloads");
		auto mesh = restored.Mesh ({}, true);
		Check (mesh && mesh->indices == savedIndices && mesh->vertices.size () == savedVertices.size (), "smooth topology survives save/load");
		for (size_t i = 0; i < savedVertices.size (); ++i)
			Check (
				mesh->vertices[i].position == savedVertices[i].position && mesh->vertices[i].normal == savedVertices[i].normal &&
					mesh->vertices[i].blend == savedVertices[i].blend,
				"smooth vertices survive save/load");
	}
	config.databasePath.clear ();
	config.meshBytes = sizeof (MeshQuad);
	World limited (config);
	Check (limited.Require ({}) && !limited.Mesh ({}, true), "oversized smooth mesh is rejected by CPU budget");
	Check (
		!limited.Mesh ({}) && !limited.Stats ().pendingMeshes && !limited.Stats ().meshBytes && !limited.LastError ().empty (),
		"failed smooth mesh does not retry or retain unaccounted arrays");
}

void TerrainContinuityTests ()
{
	auto height = [] (const Chunk &chunk, int x, int y)
	{
		int low = 0, high = 17;
		while (low + 1 < high)
		{
			int middle = (low + high) / 2;
			if (chunk.cells.Get (x, y, middle - 1).IsSolid ())
				low = middle;
			else
				high = middle;
		}
		Check (low >= 8 && low <= 16, "generated height stays in the intended 64..128 unit range");
		return low;
	};
	for (uint64_t seed : {uint64_t (1), uint64_t (987654321), UINT64_MAX})
	{
		WorldConfig config;
		config.seed = seed;
		World world (config);
		for (const ChunkKey &key :
			 {ChunkKey{0, 0, 0}, ChunkKey{-1, -1, 0}, ChunkKey{-2, -3, 0}, ChunkKey{-1000000000, 1000000000, 0}, ChunkKey{INT64_MIN, INT64_MAX - 1, 0},
			  ChunkKey{INT64_MAX - 1, INT64_MIN, 0}})
		{
			Check (
				world.Require (key) && world.Require ({key.x + 1, key.y, 0}) && world.Require ({key.x, key.y + 1, 0}), "load terrain continuity neighborhood");
			auto chunk = world.Find (key);
			auto east = world.Find ({key.x + 1, key.y, 0});
			auto north = world.Find ({key.x, key.y + 1, 0});
			int	 heights[VoxelsPerChunk][VoxelsPerChunk];
			for (int x = 0; x < VoxelsPerChunk; ++x)
				for (int y = 0; y < VoxelsPerChunk; ++y)
					heights[x][y] = height (*chunk, x, y);
			for (int x = 0; x < VoxelsPerChunk; ++x)
				for (int y = 0; y < VoxelsPerChunk; ++y)
				{
					int nextX = x + 1 < VoxelsPerChunk ? heights[x + 1][y] : height (*east, 0, y);
					int nextY = y + 1 < VoxelsPerChunk ? heights[x][y + 1] : height (*north, x, 0);
					Check (
						std::abs (heights[x][y] - nextX) <= 1 && std::abs (heights[x][y] - nextY) <= 1,
						"adjacent terrain columns never exceed an 8-unit step, including negative and distant chunk seams");
				}
		}
		Check (world.Require ({-1, 0, 0}) && world.Require ({0, 0, 0}), "load west spawn boundary");
		auto west = world.Find ({-1, 0, 0});
		auto spawn = world.Find ({0, 0, 0});
		Check (height (*west, 127, 16) == 8 && height (*spawn, 0, 16) == 8, "spawn plateau crosses the west chunk seam without a cliff");
		auto crossing = world.Trace (At ({0, 0, 0}, 24, 128, 88), {-64, 0, 0}, {-16, -16, -24}, {16, 16, 32});
		Check (!crossing.startSolid && crossing.fraction == 1 && crossing.end.chunk.x == -1, "standing player crosses west seam without hitting a ledge");
	}
}

void MeshCoverageTests ()
{
	World world (EmptyConfig ());
	Chunk chunk;
	chunk.key = {0, 0, 0};
	std::array<uint16_t, 32 * 32 * 32> cells{};
	auto							   index = [] (int x, int y, int z) { return (z * 32 + y) * 32 + x; };
	for (int z = 8; z < 16; ++z)
		for (int y = 8; y < 16; ++y)
			for (int x = 8; x < 16; ++x)
				cells[index (x, y, z)] = 1;
	std::mt19937 rng (72832);
	for (int i = 0; i < 1000; ++i)
		cells[index (1 + rng () % 30, 1 + rng () % 30, 1 + rng () % 30)] = uint16_t (1 + rng () % 3);
	for (int z = 0; z < 32; ++z)
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x)
				if (cells[index (x, y, z)])
					chunk.cells.Set (x, y, z, {cells[index (x, y, z)], Solid});
	Check (world.Install (chunk), "install mesh oracle chunk");
	auto mesh = world.Mesh (chunk.key, true);
	Check (mesh != nullptr, "coalesced mesh generated");
	using Face = std::tuple<int, int, int, int, int, uint16_t>;
	std::set<Face> expected, actual;
	for (int z = 0; z < 32; ++z)
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x)
			{
				uint16_t material = cells[index (x, y, z)];
				if (!material)
					continue;
				for (int axis = 0; axis < 3; ++axis)
					for (int sign : {-1, 1})
					{
						std::array<int, 3> p{x, y, z}, adjacent = p;
						adjacent[axis] += sign;
						if (adjacent[axis] >= 0 && adjacent[axis] < 32 && cells[index (adjacent[0], adjacent[1], adjacent[2])])
							continue;
						expected.emplace (axis, sign, p[axis] + (sign > 0), p[(axis + 1) % 3], p[(axis + 2) % 3], material);
					}
			}
	for (const auto &quad : mesh->quads)
	{
		int size = int (quad.size) / VoxelSize;
		Check (size >= 1 && size <= VoxelsPerChunk && (size & (size - 1)) == 0, "merged face has a dyadic cell size");
		int u = int (quad.origin[(quad.axis + 1) % 3]) / VoxelSize;
		int v = int (quad.origin[(quad.axis + 2) % 3]) / VoxelSize;
		int plane = int (quad.origin[quad.axis]) / VoxelSize;
		for (int a = 0; a < size; ++a)
			for (int b = 0; b < size; ++b)
				Check (actual.emplace (quad.axis, quad.sign, plane, u + a, v + b, quad.material).second, "coalesced faces never overlap");
	}
	Check (expected == actual, "coalesced faces exactly cover brute-force exposed voxel faces and materials");
	Check (mesh->quads.size () < expected.size (), "coplanar surfaces are coalesced");

	chunk.cells.Fill ({1, Solid});
	chunk.revision = 1;
	Check (world.Install (chunk), "install homogeneous full chunk");
	mesh = world.Mesh (chunk.key, true);
	Check (mesh && mesh->quads.size () == 6, "full homogeneous chunk collapses to six coalesced faces");
	for (const auto &quad : mesh->quads)
		Check (quad.size == ChunkSize, "full chunk uses maximal face size");
}

void SweepOracleTests ()
{
	// This oracle samples motion against a flat occupancy grid then bisects the
	// first contact. It does not reuse the tree traversal or slab intersection.
	World						   world (EmptyConfig ());
	Chunk						   chunk;
	std::array<bool, 32 * 32 * 32> grid{};
	auto						   index = [] (int x, int y, int z) { return (z * 32 + y) * 32 + x; };
	std::mt19937				   rng (19091);
	for (int i = 0; i < 700; ++i)
	{
		int x = 8 + rng () % 16, y = 8 + rng () % 16, z = 8 + rng () % 16;
		grid[index (x, y, z)] = true;
		chunk.cells.Set (x + 32, y + 32, z + 32, {uint16_t (1 + i % 4), Solid});
	}
	Check (world.Install (chunk), "install randomized collision terrain");
	for (int trial = 0; trial < 600; ++trial)
	{
		std::array<double, 3> start, offset;
		std::array<float, 3>  mins, maxs;
		for (int axis = 0; axis < 3; ++axis)
		{
			start[axis] = 280.25 + (rng () % 1800) * 0.125;
			offset[axis] = int (rng () % 257) - 128;
			mins[axis] = -float (2 + rng () % 6);
			maxs[axis] = float (2 + rng () % 6);
		}
		auto overlap = [&] (double t)
		{
			std::array<int, 3> low, high;
			for (int axis = 0; axis < 3; ++axis)
			{
				double p = start[axis] + offset[axis] * t - 256;
				low[axis] = std::max (0, int (std::floor ((p + mins[axis]) / VoxelSize)));
				high[axis] = std::min (31, int (std::ceil ((p + maxs[axis]) / VoxelSize)) - 1);
			}
			for (int x = low[0]; x <= high[0]; ++x)
				for (int y = low[1]; y <= high[1]; ++y)
					for (int z = low[2]; z <= high[2]; ++z)
						if (grid[index (x, y, z)])
							return true;
			return false;
		};
		auto hit = world.Trace (At ({}, float (start[0]), float (start[1]), float (start[2])), offset, mins, maxs);
		Check (hit.startSolid == overlap (0), "randomized swept box startsolid matches grid oracle");
		if (hit.startSolid)
			continue;
		int step = 1;
		for (; step <= 2048; ++step)
			if (overlap (step / 2048.0))
				break;
		if (step > 2048)
		{
			Check (hit.fraction == 1, "unobstructed randomized sweep matches grid oracle");
			continue;
		}
		double low = (step - 1) / 2048.0, high = step / 2048.0;
		for (int iteration = 0; iteration < 24; ++iteration)
		{
			double middle = (low + high) / 2;
			if (overlap (middle))
				high = middle;
			else
				low = middle;
		}
		double normalSpeed = 0;
		for (int axis = 0; axis < 3; ++axis)
			normalSpeed += std::abs (offset[axis] * hit.normal[axis]);
		Check (hit.fraction <= high && high - hit.fraction <= 0.03126 / normalSpeed + 1e-7, "randomized contact matches oracle within collision epsilon");
	}
}
void MerkleStorageTests (const std::filesystem::path &directory)
{
	Check (
		TerrainHeightVoxels (1, 123, -456, 17, 101) == 13 && TerrainHeightVoxels (42, -1234, 5678, 0, 0) == 16 &&
			TerrainHeightVoxels (UINT64_MAX, INT64_MAX, INT64_MIN, 127, 127) == 11 &&
			TerrainHeightVoxels (123, 1000000000000000LL, -1000000000000000LL, 64, 96) == 14,
		"version 1 independent integer generator golden fixtures");
	// SHA-256 published empty/abc vectors independently test the hashing primitive.
	Check (
		WorldSHA256 ({}) == ContentHash{0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
										0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55},
		"SHA256 empty vector");
	const std::array<uint8_t, 3> abc{'a', 'b', 'c'};
	Check (
		WorldSHA256 (abc) == ContentHash{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
										 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad},
		"SHA256 abc vector");
	WorldConfig config = EmptyConfig ();
	config.databasePath = (directory / "merkle.sqlite").string ();
	World	 world (config), client (EmptyConfig ());
	sqlite3 *database = nullptr;
	Check (sqlite3_open (config.databasePath.c_str (), &database) == SQLITE_OK, "inspect Merkle fixture");
	auto count = [&] (const char *sql)
	{
		sqlite3_stmt *s = nullptr;
		Check (sqlite3_prepare_v2 (database, sql, -1, &s, nullptr) == SQLITE_OK, "prepare object count");
		Check (sqlite3_step (s) == SQLITE_ROW, "read object count");
		auto n = sqlite3_column_int64 (s, 0);
		sqlite3_finalize (s);
		return n;
	};
	Check (world.CurrentRoot () == ContentHash{} && count ("SELECT count(*) FROM world_objects") == 0, "pristine terrain stores descriptor only");
	Check (world.DescriptorBytes () == client.DescriptorBytes (), "generator descriptors agree");
	const auto	 position = At ({INT64_MIN, INT64_MAX, -999999999999999LL}, 64, 64, 64);
	TerrainProof absent;
	Check (
		world.GetTerrainProof (position.chunk, absent) && ValidateTerrainProof (absent) && absent.nodes.empty () && !absent.overrideObject,
		"pristine absence proof");
	Check (client.InstallTerrainProof (absent) && client.Sample (position) == Cell{}, "verified pristine client generation");
	Check (world.Edit (position, {2, Solid}), "edit extreme-coordinate terrain");
	const auto	 first = world.CurrentRoot ();
	TerrainProof proof;
	Check (world.GetTerrainProof (position.chunk, proof) && ValidateTerrainProof (proof), "present content proof");
	Check (
		proof.nodes.size () == 1 && proof.overrideObject && proof.overrideObject->bytes.size () < 100,
		"one voxel uses sparse override and compressed spatial leaf");
	Check (client.InstallTerrainProof (proof) && client.Sample (position) == Cell{2, Solid}, "peer object reconstructs terrain");
	for (int mode = 0; mode < 4; ++mode)
	{
		auto corrupt = proof;
		if (mode == 0)
			corrupt.root[0] ^= 1;
		if (mode == 1)
			corrupt.nodes[0].bytes[0] ^= 1;
		if (mode == 2)
			corrupt.overrideObject->bytes.back () ^= 1;
		if (mode == 3)
			corrupt.nodes.clear ();
		Check (!ValidateTerrainProof (corrupt) && !client.InstallTerrainProof (corrupt), "corrupt or incomplete proof rejected");
	}
	const uint64_t id = world.AllocateEntityPageId ();
	Check (world.StoreEntityPage (id, position.chunk, {1, 2, 3}, 0), "snapshot page fixture");
	Check (world.CreateSnapshot ("before", {7, 8, 9}), "pin complete snapshot");
	{
		Check (
			sqlite3_exec (database, "UPDATE world_snapshots SET root=zeroblob(32) WHERE name='before'", nullptr, nullptr, nullptr) == SQLITE_OK,
			"corrupt snapshot root metadata");
		const auto oldEpoch = world.AuthorityEpoch ();
		Check (
			!world.RestoreSnapshot ("before") && world.AuthorityEpoch () == oldEpoch && world.CurrentRoot () == first,
			"whole snapshot hash rejects altered terrain root before mutation");
		sqlite3_stmt *repair = nullptr;
		Check (
			sqlite3_prepare_v2 (database, "UPDATE world_snapshots SET root=? WHERE name='before'", -1, &repair, nullptr) == SQLITE_OK,
			"prepare root metadata repair");
		sqlite3_bind_blob (repair, 1, first.data (), 32, SQLITE_TRANSIENT);
		Check (sqlite3_step (repair) == SQLITE_DONE, "repair root metadata");
		sqlite3_finalize (repair);
	}
	const auto privateCount = count ("SELECT count(*) FROM private_objects");
	Check (
		world.CreateSnapshot ("duplicate_payload", {7, 8, 9}) && count ("SELECT count(*) FROM private_objects") == privateCount,
		"identical snapshot checkpoint and page payloads deduplicate by SHA256");
	Check (world.DeleteSnapshot ("duplicate_payload"), "delete duplicate payload pin");
	std::vector<uint8_t> repeated (65536 * 3 + 17, 42);
	const auto			 beforePieces = count ("SELECT count(*) FROM private_objects");
	Check (world.CreateSnapshot ("pieces", repeated), "create checkpoint spanning private pieces");
	Check (count ("SELECT count(*) FROM private_objects") - beforePieces == 3, "repeated64KiB pieces deduplicate within payload");
	std::vector<uint8_t> joined;
	Check (world.LoadSnapshot ("pieces", joined) && joined == repeated, "private pieces reconstruct ordered checkpoint");
	Check (
		world.DeleteSnapshot ("pieces") && count ("SELECT count(*) FROM private_objects") == beforePieces,
		"automatic snapshot deletion GC releases private pieces");
	ContentHash			 privateHash;
	std::vector<uint8_t> privateBytes;
	{
		sqlite3_stmt *s = nullptr;
		Check (
			sqlite3_prepare_v2 (
				database, "SELECT hash,data FROM private_objects WHERE hash=(SELECT checkpoint FROM world_snapshots WHERE name='before')", -1, &s, nullptr) ==
					SQLITE_OK &&
				sqlite3_step (s) == SQLITE_ROW,
			"inspect private manifest");
		std::copy_n (static_cast<const uint8_t *> (sqlite3_column_blob (s, 0)), 32, privateHash.begin ());
		const auto *p = static_cast<const uint8_t *> (sqlite3_column_blob (s, 1));
		privateBytes.assign (p, p + sqlite3_column_bytes (s, 1));
		sqlite3_finalize (s);
		Check (
			sqlite3_exec (
				database, "UPDATE private_objects SET data=X'01' WHERE hash=(SELECT checkpoint FROM world_snapshots WHERE name='before')", nullptr, nullptr,
				nullptr) == SQLITE_OK,
			"corrupt private manifest");
		std::vector<uint8_t> untouched{99};
		const auto			 previousEpoch = world.AuthorityEpoch ();
		Check (
			!world.LoadSnapshot ("before", untouched) && untouched == std::vector<uint8_t>{99} && !world.RestoreSnapshot ("before") &&
				world.AuthorityEpoch () == previousEpoch,
			"private hash corruption rejected before restore mutation");
		Check (sqlite3_prepare_v2 (database, "UPDATE private_objects SET data=? WHERE hash=?", -1, &s, nullptr) == SQLITE_OK, "prepare manifest repair");
		sqlite3_bind_blob (s, 1, privateBytes.data (), int (privateBytes.size ()), SQLITE_TRANSIENT);
		sqlite3_bind_blob (s, 2, privateHash.data (), 32, SQLITE_TRANSIENT);
		Check (sqlite3_step (s) == SQLITE_DONE, "repair private manifest");
		sqlite3_finalize (s);
	}

	Check (!world.CreateSnapshot ("before", {0}) && !world.CreateSnapshot ("latest", {0}), "snapshot name collision and reservation");
	Check (world.Edit (position, {3, Solid}) && world.StoreEntityPage (id, {}, {4, 5}, 1), "modify after snapshot");
	Check (world.DeleteEntityPage (id, 2), "delete after snapshot");
	Check (world.CollectGarbage (), "collect retaining pinned snapshot");
	uint64_t epoch = world.AuthorityEpoch ();
	Check (world.RestoreSnapshot ("before") && world.AuthorityEpoch () == epoch + 1, "restore advances authority epoch");
	Check (world.CurrentRoot () == first && world.Require (position.chunk) && world.Sample (position) == Cell{2, Solid}, "restores terrain content root");
	EntityPage			 page;
	std::vector<uint8_t> checkpoint;
	Check (world.LoadEntityPage (id, page) && page.bytes == std::vector<uint8_t> ({1, 2, 3}), "restores page version");
	Check (world.LoadCheckpoint (checkpoint) && checkpoint == std::vector<uint8_t> ({7, 8, 9}), "restores opaque engine checkpoint");
	Check (world.Find (position.chunk)->revision > proof.revision, "terrain revision clock never rewinds");
	Check (world.Edit (position, {}), "restore generated voxel removes override");
	Check (world.CurrentRoot () == ContentHash{}, "restored generation returns identical empty root");
	Check (world.DeleteSnapshot ("before"), "delete named snapshot");
	Check (world.SaveCheckpoint ({}), "replace latest root pin");
	Check (world.CollectGarbage () && count ("SELECT count(*) FROM world_objects") == 0, "GC removes unreachable historical terrain");
	// Tree updates and proofs remain bounded even across the signed 64-bit coordinate range.
	for (int i = 0; i < 72; ++i)
		Check (world.Edit (At ({i * 100000000003LL, -i * 70000001LL, i - 36}, 0, 0, 0), {1, Solid}), "spatially distributed edit");
	World reverse (EmptyConfig ());
	for (int i = 71; i >= 0; --i)
		Check (reverse.Edit (At ({i * 100000000003LL, -i * 70000001LL, i - 36}, 0, 0, 0), {1, Solid}), "reverse insertion fixture");
	Check (reverse.CurrentRoot () == world.CurrentRoot (), "canonical world root independent of insertion order and revision history");
	for (int i = -1; i < 73; ++i)
	{
		TerrainProof p;
		ChunkKey	 key{i * 100000000003LL, -i * 70000001LL, i - 36};
		Check (world.GetTerrainProof (key, p) && ValidateTerrainProof (p) && p.nodes.size () <= 65, "bounded path-compressed proof");
		Check (bool (p.overrideObject) == (i >= 0 && i < 72), "Merkle membership and absence across branches");
	}
	Check (world.CollectGarbage (), "collect branching Merkle tree");
	const auto beforeCount = count ("SELECT count(*) FROM world_objects");
	Check (world.Edit (At ({0, 0, -36}, 8, 0, 0), {3, Solid}), "single leaf update in large tree");
	Check (count ("SELECT count(*) FROM world_objects") - beforeCount <= 66, "single edit changes at most one bounded path");
	const auto beforeFailure = world.CurrentRoot ();
	Check (
		sqlite3_exec (
			database, "CREATE TRIGGER reject_root BEFORE UPDATE ON terrain_state BEGIN SELECT RAISE(ABORT,'root failure'); END", nullptr, nullptr, nullptr) ==
			SQLITE_OK,
		"inject atomic root failure");
	Check (!world.Edit (At ({0, 0, -36}, 16, 0, 0), {2, Solid}) && world.CurrentRoot () == beforeFailure, "failed commit publishes no new root");
	Check (sqlite3_exec (database, "DROP TRIGGER reject_root", nullptr, nullptr, nullptr) == SQLITE_OK, "remove root failure");
	sqlite3_close (database);
	WorldConfig terrainConfig;
	terrainConfig.smoothTerrain = false;
	World		  terrain (terrainConfig), terrainClient (terrainConfig);
	WorldPosition underground = At ({-9999999, 8888888, -1}, 16, 16, 16);
	Check (terrain.Require (underground.chunk) && terrain.Edit (underground, {}), "excavate generated solid");
	Check (
		terrain.GetTerrainProof (underground.chunk, proof) && terrainClient.InstallTerrainProof (proof) && terrainClient.Sample (underground) == Cell{},
		"explicit empty survives regeneration");
	Check (terrain.Edit (underground, {1, Solid}) && terrain.CurrentRoot () == ContentHash{}, "restore generated solid clears override");
}

void MerkleCurrentHeadTests (const std::filesystem::path &directory)
{
	WorldConfig config = EmptyConfig ();
	config.databasePath = (directory / "current-head.sqlite").string ();
	const auto	position = At ({111, -222, 333}, 16, 24, 32);
	ContentHash durable;
	uint64_t	epoch = 0;
	{
		World world (config);
		Check (world.Edit (position, {1, Solid}) && world.SaveCheckpoint ({4, 5}), "pin older snapshot");
		Check (world.Edit (position, {2, Solid}), "acknowledge post-save edit");
		durable = world.CurrentRoot ();
		epoch = world.AuthorityEpoch ();
	}
	World reopened (config);
	Check (
		reopened.CurrentRoot () == durable && reopened.RestoreEntityPagesCheckpoint () && reopened.AuthorityEpoch () == epoch &&
			reopened.CurrentRoot () == durable && reopened.Require (position.chunk) && reopened.Sample (position) == Cell{2, Solid},
		"ordinary load preserves acknowledged post-save edit");
	Check (
		reopened.RestoreSnapshot ("latest") && reopened.AuthorityEpoch () == epoch + 1 && reopened.Require (position.chunk) &&
			reopened.Sample (position) == Cell{1, Solid},
		"only explicit snapshot restore rolls current terrain back");
}

void MerkleMigrationTests (const std::filesystem::path &directory)
{
	const auto path = (directory / "legacy.sqlite").string ();
	sqlite3	  *db = nullptr;
	Check (sqlite3_open (path.c_str (), &db) == SQLITE_OK, "open legacy migration fixture");
	Check (
		sqlite3_exec (
			db,
			"CREATE TABLE world_metadata(id INTEGER PRIMARY KEY,format INTEGER,generator INTEGER,seed TEXT);INSERT INTO world_metadata "
			"VALUES(1,1,0,'42');CREATE TABLE chunks(x INTEGER,y INTEGER,z INTEGER,revision INTEGER,data BLOB,PRIMARY KEY(x,y,z));",
			nullptr, nullptr, nullptr) == SQLITE_OK,
		"create legacy schema");
	Octree cells;
	cells.Set (1, 2, 3, {7, Solid});
	auto		  encoded = cells.Encode ();
	sqlite3_stmt *s = nullptr;
	Check (sqlite3_prepare_v2 (db, "INSERT INTO chunks VALUES(-999,888,0,9,?)", -1, &s, nullptr) == SQLITE_OK, "prepare legacy chunk");
	sqlite3_bind_blob (s, 1, encoded.data (), int (encoded.size ()), SQLITE_TRANSIENT);
	Check (sqlite3_step (s) == SQLITE_DONE, "write legacy chunk");
	sqlite3_finalize (s);
	sqlite3_close (db);
	WorldConfig config = EmptyConfig ();
	config.databasePath = path;
	ContentHash migratedRoot;
	{
		World world (config);
		Check (world.Config ().databasePath == path + ".merkle", "migration uses separate database");
		Check (
			world.Config ().seed == 42 && world.Require ({-999, 888, 0}) && world.Sample (At ({-999, 888, 0}, 8, 16, 24)) == Cell{7, Solid},
			"migration preserves terrain and descriptor");
		migratedRoot = world.CurrentRoot ();
		Check (world.Edit (At ({-999, 888, 0}, 8, 16, 24), {8, Solid}), "new writes go to migrated database");
	}
	Check (sqlite3_open_v2 (path.c_str (), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "inspect preserved original");
	Check (
		sqlite3_prepare_v2 (db, "SELECT revision,data FROM chunks", -1, &s, nullptr) == SQLITE_OK && sqlite3_step (s) == SQLITE_ROW,
		"original legacy chunk remains");
	Check (sqlite3_column_int64 (s, 0) == 9 && sqlite3_column_bytes (s, 1) == int (encoded.size ()), "original revision/data unchanged");
	sqlite3_finalize (s);
	sqlite3_close (db);
	World reopened (config);
	Check (
		reopened.CurrentRoot () != migratedRoot && reopened.Require ({-999, 888, 0}) && reopened.Sample (At ({-999, 888, 0}, 8, 16, 24)) == Cell{8, Solid},
		"original path reopens migrated current world");
}
} // namespace

int main ()
{
	auto directory =
		std::filesystem::temp_directory_path () / ("quiver-octree-test-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
	try
	{
		std::filesystem::create_directories (directory);
		MerkleStorageTests (directory);
		MerkleMigrationTests (directory);
		MerkleCurrentHeadTests (directory);
		PositionTests ();
		OctreeTests ();
		CollisionTests ();
		MeshAndResidencyTests ();
		PersistenceTests (directory);
		BudgetTests ();
		AsyncEditTests (directory);
		CheckpointTests (directory);
		CheckpointCrashTest (directory);
		EntityPageTests (directory);
		EntityPageAsyncTests (directory);
		EntityPageCorruptionTests (directory);
		TerrainTests ();
		SmoothMeshLifecycleTests (directory);
		TerrainContinuityTests ();
		MeshCoverageTests ();
		SweepOracleTests ();
		std::filesystem::remove_all (directory);
		std::cout << "octree world tests passed\n";
		return 0;
	}
	catch (const std::exception &error)
	{
		std::filesystem::remove_all (directory);
		std::cerr << "octree world test failed: " << error.what () << '\n';
		return 1;
	}
}
