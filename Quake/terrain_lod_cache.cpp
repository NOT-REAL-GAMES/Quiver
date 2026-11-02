/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_lod_cache.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace quiver
{
struct TerrainLodCache::Impl
{
	struct Entry
	{
		std::shared_ptr<const TerrainTileMesh> mesh;
		size_t								   bytes;
		uint64_t							   used;
	};
	struct Completion
	{
		TerrainTileKey						   key;
		std::shared_ptr<const TerrainTileMesh> mesh;
		std::string							   error;
		double								   milliseconds;
	};
	WorldConfig										 config;
	std::map<TerrainTileKey, Entry, TerrainTileLess> cache;
	std::set<TerrainTileKey, TerrainTileLess>		 selected, pending, failed;
	std::mutex										 mutex;
	std::condition_variable							 ready;
	std::deque<TerrainTileKey>						 jobs;
	std::deque<Completion>							 completions;
	std::thread										 worker;
	bool											 stop = false;
	size_t											 bytes = 0;
	uint64_t										 tick = 0, completed = 0;
	double											 buildMilliseconds = 0;
	std::string										 error;

	explicit Impl (WorldConfig value) : config (std::move (value))
	{
		worker = std::thread (
			[this]
			{
				for (;;)
				{
					TerrainTileKey key;
					{
						std::unique_lock lock (mutex);
						ready.wait (lock, [this] { return stop || !jobs.empty (); });
						if (stop)
							return;
						key = jobs.front ();
						jobs.pop_front ();
					}
					Completion result{key, {}, {}, 0};
					const auto start = std::chrono::steady_clock::now ();
					try
					{
						result.mesh = BuildTerrainTile (config, key, &result.error);
					}
					catch (const std::exception &e)
					{
						result.error = e.what ();
					}
					catch (...)
					{
						result.error = "Terrain tile build failed";
					}
					result.milliseconds = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
					std::lock_guard lock (mutex);
					if (stop)
						return;
					completions.push_back (std::move (result));
				}
			});
	}
	~Impl ()
	{
		{
			std::lock_guard lock (mutex);
			stop = true;
			jobs.clear ();
		}
		ready.notify_one ();
		worker.join ();
	}
	bool Evict ()
	{
		auto victim = cache.end ();
		for (auto it = cache.begin (); it != cache.end (); ++it)
			if (!selected.contains (it->first) && (victim == cache.end () || it->second.used < victim->second.used))
				victim = it;
		if (victim == cache.end ())
			return false;
		bytes -= victim->second.bytes;
		cache.erase (victim);
		return true;
	}
};

TerrainLodCache::TerrainLodCache (WorldConfig config) : impl (std::make_unique<Impl> (std::move (config))) {}
TerrainLodCache::~TerrainLodCache () = default;
void TerrainLodCache::Update (const std::vector<TerrainTileKey> &keys)
{
	auto &state = *impl;
	++state.tick;
	state.selected.clear ();
	for (const auto &key : keys)
	{
		if (state.selected.size () == MaxTiles)
			break;
		state.selected.insert (key);
	}
	// Remove old failures as the camera moves, so even malformed requests have
	// bounded bookkeeping and can be retried on a later visit.
	for (auto it = state.failed.begin (); it != state.failed.end ();)
		if (!state.selected.contains (*it))
			it = state.failed.erase (it);
		else
			++it;
	std::deque<Impl::Completion> completions;
	{
		std::lock_guard lock (state.mutex);
		for (auto it = state.jobs.begin (); it != state.jobs.end ();)
			if (!state.selected.contains (*it))
			{
				state.pending.erase (*it);
				it = state.jobs.erase (it);
			}
			else
				++it;
		completions.swap (state.completions);
	}
	for (auto &result : completions)
	{
		state.pending.erase (result.key);
		++state.completed;
		state.buildMilliseconds += result.milliseconds;
		if (!state.selected.contains (result.key))
			continue;
		if (!result.mesh)
		{
			state.failed.insert (result.key);
			state.error = std::move (result.error);
			continue;
		}
		const size_t bytes = result.mesh->vertices.capacity () * sizeof (TerrainVertex) + result.mesh->indices.capacity () * sizeof (uint32_t);
		if (bytes > MaxBytes)
		{
			state.failed.insert (result.key);
			state.error = "Terrain tile exceeds CPU cache budget";
			continue;
		}
		while ((state.cache.size () >= MaxTiles || state.bytes + bytes > MaxBytes) && state.Evict ())
		{
		}
		if (state.cache.size () >= MaxTiles || state.bytes + bytes > MaxBytes)
			continue;
		state.bytes += bytes;
		state.cache.emplace (result.key, Impl::Entry{std::move (result.mesh), bytes, state.tick});
	}
	for (const auto &key : keys)
	{
		if (!state.selected.contains (key))
			continue;
		auto entry = state.cache.find (key);
		if (entry != state.cache.end ())
		{
			entry->second.used = state.tick;
			continue;
		}
		if (state.pending.size () == MaxJobs)
			break;
		if (state.pending.contains (key) || state.failed.contains (key))
			continue;
		state.pending.insert (key);
		{
			std::lock_guard lock (state.mutex);
			state.jobs.push_back (key);
		}
		state.ready.notify_one ();
	}
}
std::shared_ptr<const TerrainTileMesh> TerrainLodCache::Find (TerrainTileKey key) const
{
	auto entry = impl->cache.find (key);
	return entry == impl->cache.end () ? nullptr : entry->second.mesh;
}
TerrainLodCacheStats TerrainLodCache::Stats () const
{
	return {impl->selected.size (), impl->cache.size (), impl->pending.size (), impl->bytes, impl->failed.size (), impl->completed, impl->buildMilliseconds};
}
const std::string &TerrainLodCache::LastError () const
{
	return impl->error;
}
} // namespace quiver
