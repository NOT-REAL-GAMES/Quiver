/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_LOD_CACHE_H
#define QUIVER_TERRAIN_LOD_CACHE_H

#include "terrain_lod.h"

namespace quiver
{
struct TerrainTileLess
{
	bool operator() (const TerrainTileKey &a, const TerrainTileKey &b) const
	{
		if (a.level != b.level)
			return a.level < b.level;
		if (a.x != b.x)
			return a.x < b.x;
		return a.y < b.y;
	}
};

struct TerrainLodCacheStats
{
	size_t	 selected = 0;
	size_t	 cached = 0;
	size_t	 pending = 0;
	size_t	 bytes = 0;
	size_t	 failed = 0;
	uint64_t completed = 0;
	double	 buildMilliseconds = 0;
};

// Update, Find and Stats run on the owning thread. One worker builds immutable
// tile snapshots; destruction cancels queued jobs and joins its current build.
class TerrainLodCache
{
  public:
	static constexpr size_t MaxTiles = 512;
	static constexpr size_t MaxJobs = 8;
	static constexpr size_t MaxBytes = 32 * 1024 * 1024;
	explicit TerrainLodCache (WorldConfig config);
	~TerrainLodCache ();
	TerrainLodCache (const TerrainLodCache &) = delete;
	TerrainLodCache						  &operator= (const TerrainLodCache &) = delete;
	// Order is scheduling priority. At most MaxTiles distinct keys are selected.
	void								   Update (const std::vector<TerrainTileKey> &keys);
	std::shared_ptr<const TerrainTileMesh> Find (TerrainTileKey key) const;
	TerrainLodCacheStats				   Stats () const;
	const std::string					  &LastError () const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver
#endif
