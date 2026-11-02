/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_lod_cache.h"

#include <chrono>
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

void CheckBounds (const TerrainLodCache &cache)
{
	const auto stats = cache.Stats ();
	Check (stats.selected <= TerrainLodCache::MaxTiles, "selected tile cap");
	Check (stats.cached <= TerrainLodCache::MaxTiles, "cached tile cap");
	Check (stats.pending <= TerrainLodCache::MaxJobs, "outstanding job cap");
	Check (stats.bytes <= TerrainLodCache::MaxBytes, "CPU byte cap");
	Check (stats.failed <= TerrainLodCache::MaxTiles, "failure bookkeeping cap");
}

void Settle (TerrainLodCache &cache, const std::vector<TerrainTileKey> &keys)
{
	const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
	for (;;)
	{
		cache.Update (keys);
		CheckBounds (cache);
		if (!cache.Stats ().pending)
			return;
		Check (std::chrono::steady_clock::now () < deadline, "tile jobs did not settle");
		std::this_thread::sleep_for (std::chrono::milliseconds (1));
	}
}

std::vector<TerrainTileKey> Keys (int64_t first, int count)
{
	std::vector<TerrainTileKey> keys;
	for (int i = 0; i < count; ++i)
		keys.push_back ({first + i, -17, 0});
	return keys;
}

void BudgetAndTravelTests ()
{
	TerrainLodCache cache (WorldConfig{});
	const auto		oversized = Keys (-320, 640);
	cache.Update (oversized);
	CheckBounds (cache);
	Check (cache.Stats ().selected == TerrainLodCache::MaxTiles, "selection truncates at cap");
	Settle (cache, oversized);
	Check (cache.Stats ().cached == TerrainLodCache::MaxTiles, "full bounded working set completes");
	Check (cache.Find (oversized.front ()) != nullptr, "first priority tile completes");
	Check (!cache.Find (oversized.back ()), "truncated selection is not generated");
	const auto retained = cache.Find (oversized.front ());
	const auto firstPosition = retained->vertices.front ().position;

	// Rapid travel changes interest while work is queued. Only the final set
	// needs to finish, and unrelated cached tiles must make room for it.
	cache.Update (Keys (1000, 64));
	cache.Update (Keys (-10000, 64));
	const auto destination = Keys (900000000000LL, 64);
	Settle (cache, destination);
	for (const auto &key : destination)
		Check (cache.Find (key) != nullptr, "new interest is not starved by stale jobs");
	Check (retained->vertices.front ().position == firstPosition, "eviction preserves immutable consumer snapshot");
	Settle (cache, {});
	Check (!cache.Stats ().selected && !cache.Stats ().pending, "clearing interest drains outstanding work");
}

void FailureAndLifetimeTests ()
{
	std::shared_ptr<const TerrainTileMesh> retained;
	{
		WorldConfig config;
		config.seed = 47;
		TerrainLodCache					  cache (config);
		const std::vector<TerrainTileKey> invalid = {{0, 0, 255}};
		Settle (cache, invalid);
		Check (cache.Stats ().failed == 1 && !cache.LastError ().empty (), "invalid requests report failure");
		const auto completed = cache.Stats ().completed;
		for (int i = 0; i < 20; ++i)
			cache.Update (invalid);
		Check (cache.Stats ().completed == completed && !cache.Stats ().pending, "failed tiles do not spin rebuilding");
		const std::vector<TerrainTileKey> valid = {{100, 100, 0}};
		Settle (cache, valid);
		Check (!cache.Stats ().failed, "old failure bookkeeping is released");
		retained = cache.Find (valid.front ());
		Check (retained && !retained->indices.empty (), "cache recovers after an invalid request");
		cache.Update (Keys (-100, 64)); // Destruction with queued/running work.
	}
	Check (retained && !retained->vertices.empty (), "consumer snapshot survives worker shutdown");
	WorldConfig next;
	next.seed = 99;
	TerrainLodCache cache (next);
	Settle (cache, {retained->key});
	const auto different = cache.Find (retained->key);
	bool	   changed = false;
	for (size_t i = 0; i < retained->vertices.size (); ++i)
		changed |= retained->vertices[i].position != different->vertices[i].position;
	Check (changed, "new world seed does not reuse old worker results");
}
} // namespace

int main ()
{
	try
	{
		BudgetAndTravelTests ();
		FailureAndLifetimeTests ();
		std::cout << "terrain LOD cache tests passed\n";
		return 0;
	}
	catch (const std::exception &error)
	{
		std::cerr << "terrain LOD cache test failed: " << error.what () << '\n';
		return 1;
	}
}
