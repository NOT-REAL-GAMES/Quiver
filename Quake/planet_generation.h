/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_PLANET_GENERATION_H
#define QUIVER_PLANET_GENERATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <span>

namespace quiver
{
// Generator identity: changing any value changes the procedural base world.
struct PlanetSettings
{
	uint32_t version = 2;			 // Version 1 remains available for existing worlds.
	int64_t	 extentChunks = 1250048; // 40,001,536 metres; divisible by 256-chunk regions.
	uint32_t globalResolution = 4096;
	uint32_t globalHydraulicIterations = 12;
	uint32_t globalThermalIterations = 8;
	uint32_t regionalHydraulicIterations = 12;
	uint32_t regionalThermalIterations = 8;
	int32_t	 seaLevel = 0; // Quake units, 32 units/metre.
	bool	 operator== (const PlanetSettings &) const = default;
};

int64_t CanonicalPlanetChunk (int64_t chunk, int64_t extentChunks);
int64_t NearestPlanetChunkDelta (int64_t chunk, int64_t origin, int64_t extentChunks);
bool	ValidatePlanetSettings (const PlanetSettings &settings, std::string *error = nullptr);

std::vector<uint8_t> EncodePlanetSettings (const PlanetSettings &settings);
bool				 DecodePlanetSettings (std::span<const uint8_t> bytes, PlanetSettings &settings, std::string *error = nullptr);

struct PlanetSample
{
	double				 height = 0;
	std::array<float, 3> normal{0, 0, 1};
	float				 moisture = 0, temperature = 0, sediment = 0;
	uint16_t			 material = 1;
};

// No engine or Vulkan dependencies. The global map is prepared explicitly,
// before publishing this generator to streaming workers. Samples of a cold
// region synchronously generate it; callers should prefetch on their workers.
// Global maps are immutable once prepared. Region generation/cache is thread safe.
// Cold generation runs outside the cache lock; cached queries do not wait for
// unrelated region generation. Active queries retain immutable region snapshots.
class PlanetGenerator
{
  public:
	using Progress = std::function<void (uint32_t completed, uint32_t total)>;
	explicit PlanetGenerator (PlanetSettings settings, uint64_t seed, size_t regionCacheLimit = 32);
	~PlanetGenerator ();
	PlanetGenerator (const PlanetGenerator &) = delete;
	PlanetGenerator		 &operator= (const PlanetGenerator &) = delete;
	bool				  Prepare (std::string *error = nullptr, Progress progress = {});
	bool				  Ready () const;
	const PlanetSettings &Settings () const;
	uint64_t			  Seed () const;
	PlanetSample		  Sample (int64_t chunkX, int64_t chunkY, double localX, double localY) const;
	double				  Height (int64_t chunkX, int64_t chunkY, double localX, double localY) const;
	// Far LOD approximation: samples global maps and detail without regional generation.
	PlanetSample		  SampleCoarse (int64_t chunkX, int64_t chunkY, double localX, double localY) const;
	double				  HeightCoarse (int64_t chunkX, int64_t chunkY, double localX, double localY) const;
	void				  Prefetch (int64_t chunkX, int64_t chunkY) const;
	void				  ClearRegionCache ();
	size_t				  ResidentBytes () const;
	size_t				  CachedRegions () const;
	// Disposable portable little-endian global-map cache, identity and payload
	// checksummed. A failed load leaves the generator state untouched.
	bool				  SaveCache (const std::string &path, std::string *error = nullptr) const;
	bool				  LoadCache (const std::string &path, std::string *error = nullptr);
	uint64_t			  GlobalHash () const;

  private:
	PlanetSample SampleInternal (int64_t chunkX, int64_t chunkY, double localX, double localY, bool coarse) const;
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver
#endif
