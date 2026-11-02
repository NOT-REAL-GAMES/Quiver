/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "planet_generation.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace quiver
{
namespace
{
constexpr int	   Region = 256, Unit = 16;
constexpr uint64_t FnvOffset = 14695981039346656037ULL;
uint64_t		   Hash (uint64_t value)
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}
void Digest (uint64_t &hash, uint64_t value, int count)
{
	for (int i = 0; i < count; ++i)
	{
		hash = (hash ^ (value & 255)) * 1099511628211ULL;
		value >>= 8;
	}
}
bool Fail (std::string *error, const char *message)
{
	if (error)
		*error = message;
	return false;
}
int64_t Wrap (int64_t coordinate, int64_t period)
{
	int64_t result = coordinate % period;
	return result < 0 ? result + period : result;
}
// The floating estimate is corrected against exact integer products, so the
// result is independent of libm rounding and compiler contraction settings.
int64_t IntegerRoot (int64_t squared)
{
	int64_t result = int64_t (std::sqrt (double (squared)));
	while ((result + 1) * (result + 1) <= squared)
		++result;
	while (result * result > squared)
		--result;
	return result;
}
int32_t Mix (int32_t a, int32_t b, int64_t fraction)
{
	return int32_t (a + (int64_t (b) - a) * fraction / 65536);
}
int64_t Smooth (int64_t x)
{
	return x * x / 65536 * (196608 - 2 * x) / 65536;
}
// Integer periodic noise. Coordinates and periods are never converted to floats.
int32_t Noise (uint64_t seed, int64_t x, int64_t y, int64_t extent, int period)
{
	x = Wrap (x, extent) * period;
	y = Wrap (y, extent) * period;
	int64_t ix = x / extent, iy = y / extent;
	int64_t fx = Smooth (x % extent * 65536 / extent), fy = Smooth (y % extent * 65536 / extent);
	auto	value = [&] (int64_t a, int64_t b)
	{ return int32_t (Hash (seed ^ Hash (uint64_t (Wrap (a, period))) ^ (Hash (uint64_t (Wrap (b, period))) << 1)) & 131071) - 65536; };
	return Mix (Mix (value (ix, iy), value (ix + 1, iy), fx), Mix (value (ix, iy + 1), value (ix + 1, iy + 1), fx), fy);
}
struct Grid
{
	int					  size = 0;
	std::vector<int32_t>  height, sediment;
	std::vector<uint32_t> flow;
	explicit Grid (int n = 0) : size (n), height (size_t (n) * n), sediment (height.size ()), flow (height.size ()) {}
	size_t Bytes () const
	{
		return height.size () * 12;
	}
};
// Conservative integer water/sediment transport. Each iteration's decisions
// read only the preceding height/water state; all deposits are accumulated in
// separate buffers. Thus the exact dependency radius is two cells/iteration.
void Erode (
	Grid &grid, unsigned hydraulic, unsigned thermal, bool periodic, int talus, PlanetGenerator::Progress progress = {}, unsigned progressBase = 0,
	unsigned progressTotal = 0)
{
	int					 n = grid.size;
	const size_t		 count = grid.height.size ();
	std::vector<int32_t> water (count, 512), nextWater (count), delta (count), sedimentDelta (count);
	auto				 neighbors = [n, periodic] (int x, int y, size_t (&indices)[4])
	{
		if (!periodic && (x == 0 || y == 0 || x == n - 1 || y == n - 1))
			return false;
		indices[0] = size_t (y) * n + (x ? x - 1 : n - 1);
		indices[1] = size_t (y) * n + (x + 1 == n ? 0 : x + 1);
		indices[2] = size_t (y ? y - 1 : n - 1) * n + x;
		indices[3] = size_t (y + 1 == n ? 0 : y + 1) * n + x;
		return true;
	};
	for (unsigned iteration = 0; iteration < hydraulic + thermal; ++iteration)
	{
		std::fill (delta.begin (), delta.end (), 0);
		std::fill (sedimentDelta.begin (), sedimentDelta.end (), 0);
		std::fill (nextWater.begin (), nextWater.end (), 0);
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				size_t i = size_t (y) * n + x, adjacent[4];
				if (!neighbors (x, y, adjacent))
					continue;
				if (iteration >= hydraulic)
				{
					for (size_t j : adjacent)
					{
						int32_t transfer = std::max (0, grid.height[i] - grid.height[j] - talus) / 16;
						delta[i] -= transfer;
						delta[j] += transfer;
						sedimentDelta[j] += transfer;
					}
					continue;
				}
				size_t	target = i;
				int64_t level = int64_t (grid.height[i]) + water[i];
				for (size_t j : adjacent)
					if (int64_t (grid.height[j]) + water[j] < level)
					{
						target = j;
						level = int64_t (grid.height[j]) + water[j];
					}
				int32_t available = water[i] + 128;
				int32_t outgoing = target == i ? 0 : available * 3 / 4;
				nextWater[i] += (available - outgoing) * 7 / 8;
				nextWater[target] += outgoing * 7 / 8;
				grid.flow[i] += uint32_t (outgoing);
				int32_t slope = std::max (0, grid.height[i] - grid.height[target]);
				int32_t transfer = std::min (slope / 32, outgoing / 4);
				delta[i] -= transfer;
				delta[target] += transfer;
				sedimentDelta[target] += transfer;
			}
		for (size_t i = 0; i < count; ++i)
		{
			grid.height[i] += delta[i];
			grid.sediment[i] += sedimentDelta[i];
		}
		water.swap (nextWater);
		if (progress)
			progress (progressBase + iteration + 1, progressTotal);
	}
}
void Put (std::vector<uint8_t> &bytes, uint64_t value, int count)
{
	for (int i = 0; i < count; ++i)
	{
		bytes.push_back (uint8_t (value));
		value >>= 8;
	}
}
uint64_t Get (std::span<const uint8_t> bytes, size_t &cursor, int count)
{
	uint64_t value = 0;
	for (int i = 0; i < count; ++i)
		value |= uint64_t (bytes[cursor++]) << (i * 8);
	return value;
}
} // namespace

int64_t CanonicalPlanetChunk (int64_t chunk, int64_t extentChunks)
{
	return extentChunks > 0 ? Wrap (chunk, extentChunks) : chunk;
}
int64_t NearestPlanetChunkDelta (int64_t chunk, int64_t origin, int64_t extentChunks)
{
	if (extentChunks <= 0)
		throw std::invalid_argument ("Planet extent must be positive");
	int64_t delta = Wrap (chunk, extentChunks) - Wrap (origin, extentChunks);
	if (delta >= extentChunks / 2 + extentChunks % 2)
		delta -= extentChunks;
	if (delta < -extentChunks / 2)
		delta += extentChunks;
	return delta;
}
bool ValidatePlanetSettings (const PlanetSettings &s, std::string *error)
{
	if (s.version != 1 && s.version != 2)
		return Fail (error, "Unsupported planet generator version");
	if (s.extentChunks < 512 || s.extentChunks > 2097152 || s.extentChunks % Region != 0)
		return Fail (error, "Planet extent must be 512..2097152 chunks and a multiple of 256");
	if (s.globalResolution < 32 || s.globalResolution > 4096 || !std::has_single_bit (s.globalResolution))
		return Fail (error, "Planet global resolution must be a power of two in 32..4096");
	if (s.globalHydraulicIterations > 64 || s.globalThermalIterations > 64 || s.regionalHydraulicIterations > 32 || s.regionalThermalIterations > 32)
		return Fail (error, "Planet erosion iteration limit exceeded");
	if (s.seaLevel < -1048576 || s.seaLevel > 1048576)
		return Fail (error, "Planet sea level is out of range");
	return true;
}
std::vector<uint8_t> EncodePlanetSettings (const PlanetSettings &s)
{
	if (!ValidatePlanetSettings (s))
		return {};
	std::vector<uint8_t> bytes;
	Put (bytes, s.version, 4);
	Put (bytes, uint64_t (s.extentChunks), 8);
	Put (bytes, s.globalResolution, 4);
	Put (bytes, s.globalHydraulicIterations, 4);
	Put (bytes, s.globalThermalIterations, 4);
	Put (bytes, s.regionalHydraulicIterations, 4);
	Put (bytes, s.regionalThermalIterations, 4);
	Put (bytes, uint32_t (s.seaLevel), 4);
	return bytes;
}
bool DecodePlanetSettings (std::span<const uint8_t> bytes, PlanetSettings &s, std::string *error)
{
	if (bytes.size () != 36)
		return Fail (error, "Invalid planet settings size");
	size_t		   cursor = 0;
	PlanetSettings candidate;
	candidate.version = uint32_t (Get (bytes, cursor, 4));
	candidate.extentChunks = std::bit_cast<int64_t> (Get (bytes, cursor, 8));
	candidate.globalResolution = uint32_t (Get (bytes, cursor, 4));
	candidate.globalHydraulicIterations = uint32_t (Get (bytes, cursor, 4));
	candidate.globalThermalIterations = uint32_t (Get (bytes, cursor, 4));
	candidate.regionalHydraulicIterations = uint32_t (Get (bytes, cursor, 4));
	candidate.regionalThermalIterations = uint32_t (Get (bytes, cursor, 4));
	candidate.seaLevel = std::bit_cast<int32_t> (uint32_t (Get (bytes, cursor, 4)));
	if (!ValidatePlanetSettings (candidate, error))
		return false;
	s = candidate;
	return true;
}

struct PlanetGenerator::Impl
{
	PlanetSettings	  settings;
	uint64_t		  seed;
	size_t			  cacheLimit;
	Grid			  global;
	std::atomic<bool> ready{false};
	struct CachedRegion
	{
		int64_t x, y;
		Grid	grid;
	};
	mutable std::list<std::shared_ptr<const CachedRegion>> regions;
	mutable std::mutex									   mutex;
	Impl (PlanetSettings s, uint64_t seedValue, size_t limit) : settings (s), seed (seedValue), cacheLimit (std::clamp<size_t> (limit, 1, 128)) {}
	template <typename T> int32_t GlobalValue (const std::vector<T> &values, int64_t x, int64_t y) const
	{
		const int64_t extent = settings.extentChunks;
		x = Wrap (x, extent) * global.size;
		y = Wrap (y, extent) * global.size;
		int		ix = int (x / extent), iy = int (y / extent);
		int64_t fx = x % extent * 65536 / extent, fy = y % extent * 65536 / extent;
		auto	at = [&] (int a, int b) { return values[size_t (b % global.size) * global.size + a % global.size]; };
		return Mix (Mix (at (ix, iy), at (ix + 1, iy), fx), Mix (at (ix, iy + 1), at (ix + 1, iy + 1), fx), fy);
	}
	int32_t InitialRegionHeight (int64_t x, int64_t y) const
	{
		int32_t base = GlobalValue (global.height, x, y);
		// Accumulated global runoff guides local valleys before finer erosion.
		int32_t drainage = GlobalValue (global.flow, x, y);
		base -= std::min (512 * Unit, std::max (0, drainage - 8192) * Unit / 16);
		int32_t	  ridge = 65536 - std::abs (Noise (seed + 23, x, y, settings.extentChunks, int (settings.extentChunks / 64)));
		int32_t	  detail = Noise (seed + 24, x, y, settings.extentChunks, int (settings.extentChunks / 16));
		// Mountain roughness rises smoothly inland. Ridge powers create steep
		// escarpments rather than a uniformly rolling heightfield.
		int32_t	  inland = std::clamp (base - settings.seaLevel * Unit, 0, 40000 * Unit);
		int64_t	  peak = int64_t (ridge) * ridge / 65536;
		const int lowlandDetail = settings.version == 1 ? 1024 : 256;
		return base + int32_t (peak * inland / 65536 / 3) + int32_t (int64_t (detail) * (lowlandDetail * Unit + inland / 16) / 65536);
	}
	std::shared_ptr<const CachedRegion> GetRegion (int64_t x, int64_t y) const
	{
		x = Wrap (x, settings.extentChunks) / Region;
		y = Wrap (y, settings.extentChunks) / Region;
		auto find = [&] () -> std::shared_ptr<const CachedRegion>
		{
			for (auto it = regions.begin (); it != regions.end (); ++it)
				if ((*it)->x == x && (*it)->y == y)
				{
					regions.splice (regions.begin (), regions, it);
					return regions.front ();
				}
			return {};
		};
		{
			std::lock_guard lock (mutex);
			if (auto cached = find ())
				return cached;
		}
		// A cold render/streaming query must not hold up collision queries for
		// unrelated cached regions. Generate an immutable snapshot outside the
		// short cache lock. Concurrent misses may do duplicate bounded work;
		// publication below reuses the first completed identical result.
		const int halo = int (settings.regionalHydraulicIterations * 2 + settings.regionalThermalIterations * 2) + 2;
		Grid	  expanded (Region + 1 + halo * 2);
		for (int j = 0; j < expanded.size; ++j)
			for (int i = 0; i < expanded.size; ++i)
				expanded.height[size_t (j) * expanded.size + i] = InitialRegionHeight (x * Region + i - halo, y * Region + j - halo);
		Erode (expanded, settings.regionalHydraulicIterations, settings.regionalThermalIterations, false, 1024 * Unit);
		auto cached = std::make_shared<CachedRegion> (CachedRegion{x, y, Grid (Region + 1)});
		for (int j = 0; j <= Region; ++j)
			for (int i = 0; i <= Region; ++i)
			{
				size_t from = size_t (j + halo) * expanded.size + i + halo, to = size_t (j) * (Region + 1) + i;
				cached->grid.height[to] = expanded.height[from];
				cached->grid.sediment[to] = expanded.sediment[from];
				cached->grid.flow[to] = expanded.flow[from];
			}
		std::lock_guard lock (mutex);
		if (auto previous = find ())
			return previous;
		regions.push_front (std::move (cached));
		while (regions.size () > cacheLimit)
			regions.pop_back ();
		return regions.front ();
	}
	// Canonical whole chunks plus 1/256-unit fractions avoid absolute float
	// cancellation and make equivalent wrapped/local coordinates bit-identical.
	std::pair<int64_t, int64_t> Coordinate (int64_t chunk, double local) const
	{
		if (!std::isfinite (local) || std::abs (local) > 1073741824.0)
			throw std::invalid_argument ("Invalid planet local coordinate");
		int64_t fixed = int64_t (std::floor (local * 256.0));
		int64_t whole = fixed / (1024 * 256), remainder = fixed % (1024 * 256);
		if (remainder < 0)
		{
			remainder += 1024 * 256;
			--whole;
		}
		return {Wrap (Wrap (chunk, settings.extentChunks) + whole, settings.extentChunks), remainder};
	}
	double HeightUnlocked (int64_t cx, int64_t cy, double lx, double ly, double *sediment = nullptr, bool coarse = false) const
	{
		auto [x, fx] = Coordinate (cx, lx);
		auto [y, fy] = Coordinate (cy, ly);
		int32_t base;
		if (coarse)
		{
			base = Mix (
				Mix (InitialRegionHeight (x, y), InitialRegionHeight (x + 1, y), fx / 4),
				Mix (InitialRegionHeight (x, y + 1), InitialRegionHeight (x + 1, y + 1), fx / 4), fy / 4);
			if (sediment)
				*sediment = GlobalValue (global.sediment, x, y) / double (Unit);
		}
		else
		{
			const auto &region = GetRegion (x, y);
			int			i = int (x % Region), j = int (y % Region);
			auto		interpolate = [&] (const std::vector<int32_t> &values)
			{
				size_t p = size_t (j) * (Region + 1) + i;
				return Mix (Mix (values[p], values[p + 1], fx / 4), Mix (values[p + Region + 1], values[p + Region + 2], fx / 4), fy / 4);
			};
			base = interpolate (region->grid.height);
			if (sediment)
				*sediment = interpolate (region->grid.sediment) / double (Unit);
		}
		// Sub-grid stone detail is evaluated at fixed point coordinates; periods
		// divide the wrapping extent exactly, including across the world seam.
		int64_t	  extent = settings.extentChunks * 1024 * 256;
		int64_t	  px = x * 1024 * 256 + fx, py = y * 1024 * 256 + fy;
		int32_t	  fine = Noise (seed + 25, px, py, extent, int (settings.extentChunks * 4));
		// Lowland microdetail must not manufacture steep normals which then
		// amplify the separate 3-D rock displacement into a cliff at spawn.
		const int amplitude = settings.version == 1 ? 32 : 4 + 28 * std::clamp (base - settings.seaLevel * Unit, 0, 40000 * Unit) / (40000 * Unit);
		int64_t	  height = int64_t (base) * 4096 + int64_t (fine) * amplitude;
		int64_t	  sx = x >= settings.extentChunks / 2 ? x - settings.extentChunks : x;
		int64_t	  sy = y >= settings.extentChunks / 2 ? y - settings.extentChunks : y;
		if (std::abs (sx) <= 17 && std::abs (sy) <= 17)
		{
			int64_t dx = sx * 1024 * 256 + fx - 128 * 256, dy = sy * 1024 * 256 + fy - 128 * 256;
			int64_t distance = IntegerRoot (dx * dx + dy * dy);
			int64_t blend = Smooth (std::clamp<int64_t> ((distance - 256 * 256) * 65536 / ((16384 - 256) * 256), 0, 65536));
			int64_t plateau = int64_t (settings.seaLevel + 128) * 65536;
			height = plateau + (height - plateau) * blend / 65536;
		}
		return double (height) / 65536;
	}
};

PlanetGenerator::PlanetGenerator (PlanetSettings settings, uint64_t seed, size_t regionCacheLimit)
	: impl (std::make_unique<Impl> (settings, seed, regionCacheLimit))
{
}
PlanetGenerator::~PlanetGenerator () = default;
bool PlanetGenerator::Prepare (std::string *error, Progress progress)
{
	std::lock_guard lock (impl->mutex);
	if (impl->ready.load ())
		return true;
	if (!ValidatePlanetSettings (impl->settings, error))
		return false;
	try
	{
		const auto	  &s = impl->settings;
		Grid		   generated (int (s.globalResolution));
		const uint32_t total = (s.version == 1 ? 1 : 2) + s.globalHydraulicIterations + s.globalThermalIterations;
		if (progress)
			progress (0, total);
		for (int y = 0; y < generated.size; ++y)
			for (int x = 0; x < generated.size; ++x)
			{
				int32_t continent = Noise (impl->seed, x, y, generated.size, 4) + Noise (impl->seed + 1, x, y, generated.size, 8) / 3;
				int32_t belts = 65536 - std::abs (Noise (impl->seed + 2, x, y, generated.size, 16));
				int32_t elevation = int32_t (int64_t (continent + 9000) * (60000 * Unit) / 65536);
				int64_t mountain = int64_t (belts) * belts / 65536;
				mountain = mountain * mountain / 65536;
				elevation += int32_t (mountain * std::clamp (elevation, 0, 48000 * Unit) / 65536);
				int		radius = std::max (1, generated.size / 32);
				int64_t dx = std::min (x, generated.size - x), dy = std::min (y, generated.size - y);
				int		distance = int (IntegerRoot (dx * dx + dy * dy));
				if (s.version == 1 && distance < radius)
					elevation = Mix (4096 * Unit, elevation, Smooth (int64_t (distance) * 65536 / radius));
				generated.height[size_t (y) * generated.size + x] = elevation + s.seaLevel * Unit;
			}
		if (progress)
			progress (1, total);
		Erode (
			generated, s.globalHydraulicIterations, s.globalThermalIterations, true, int (s.extentChunks * 1024 / s.globalResolution / 2) * Unit, progress, 1,
			total);
		if (s.version >= 2)
		{
			// Put spawn in an actual seeded lowland rather than raising the same
			// 4096-unit mound at the origin of every world. Reorient the periodic
			// map after erosion, retaining its continent/drainage structure. The
			// small sea-relative recentering also handles low-resolution maps with
			// no vertex at exactly the chosen shoreline elevation.
			const int32_t target = (384 + int32_t (Hash (impl->seed + 31) % 384)) * Unit + s.seaLevel * Unit;
			size_t		  best = 0;
			int64_t		  bestScore = std::numeric_limits<int64_t>::max ();
			for (int y = 0; y < generated.size; ++y)
				for (int x = 0; x < generated.size; ++x)
				{
					const size_t  i = size_t (y) * generated.size + x;
					const int32_t h = generated.height[i];
					const int32_t hx = generated.height[size_t (y) * generated.size + (x + 1) % generated.size];
					const int32_t hy = generated.height[size_t ((y + 1) % generated.size) * generated.size + x];
					const int64_t score = 64 * std::abs (int64_t (h) - target) + std::abs (int64_t (hx) - h) + std::abs (int64_t (hy) - h);
					if (score < bestScore)
					{
						bestScore = score;
						best = i;
					}
				}
			const int32_t shift = generated.height[best] - target;
			const size_t  ox = best % generated.size, oy = best / generated.size;
			auto		  rotate = [&] (auto &values)
			{
				std::rotate (values.begin (), values.begin () + oy * generated.size, values.end ());
				for (int y = 0; y < generated.size; ++y)
				{
					auto first = values.begin () + size_t (y) * generated.size;
					std::rotate (first, first + ox, first + generated.size);
				}
			};
			rotate (generated.height);
			rotate (generated.sediment);
			rotate (generated.flow);
			for (int32_t &height : generated.height)
				height -= shift;
			if (progress)
				progress (total, total);
		}
		impl->global = std::move (generated);
		impl->ready.store (true);
		return true;
	}
	catch (const std::exception &e)
	{
		if (error)
			*error = e.what ();
		return false;
	}
}
bool PlanetGenerator::Ready () const
{
	return impl->ready.load ();
}
const PlanetSettings &PlanetGenerator::Settings () const
{
	return impl->settings;
}
uint64_t PlanetGenerator::Seed () const
{
	return impl->seed;
}
double PlanetGenerator::Height (int64_t x, int64_t y, double lx, double ly) const
{
	if (!Ready ())
		throw std::logic_error ("Planet global map has not been prepared");
	return impl->HeightUnlocked (x, y, lx, ly);
}
double PlanetGenerator::HeightCoarse (int64_t x, int64_t y, double lx, double ly) const
{
	if (!Ready ())
		throw std::logic_error ("Planet global map has not been prepared");
	return impl->HeightUnlocked (x, y, lx, ly, nullptr, true);
}
PlanetSample PlanetGenerator::Sample (int64_t x, int64_t y, double lx, double ly) const
{
	return SampleInternal (x, y, lx, ly, false);
}
PlanetSample PlanetGenerator::SampleCoarse (int64_t x, int64_t y, double lx, double ly) const
{
	return SampleInternal (x, y, lx, ly, true);
}
PlanetSample PlanetGenerator::SampleInternal (int64_t x, int64_t y, double lx, double ly, bool coarse) const
{
	if (!Ready ())
		throw std::logic_error ("Planet global map has not been prepared");
	PlanetSample sample;
	double		 sediment;
	sample.height = impl->HeightUnlocked (x, y, lx, ly, &sediment, coarse);
	double dx = (impl->HeightUnlocked (x, y, lx + 8, ly, nullptr, coarse) - impl->HeightUnlocked (x, y, lx - 8, ly, nullptr, coarse)) / 16;
	double dy = (impl->HeightUnlocked (x, y, lx, ly + 8, nullptr, coarse) - impl->HeightUnlocked (x, y, lx, ly - 8, nullptr, coarse)) / 16;
	double inverse = 1.0 / std::sqrt (dx * dx + dy * dy + 1);
	sample.normal = {float (-dx * inverse), float (-dy * inverse), float (inverse)};
	auto [cx, unusedX] = impl->Coordinate (x, lx);
	auto [cy, unusedY] = impl->Coordinate (y, ly);
	sample.moisture = std::clamp (0.5f + Noise (impl->seed + 3, cx, cy, impl->settings.extentChunks, 12) / 131072.0f, 0.0f, 1.0f);
	sample.temperature = std::clamp (
		0.65f + Noise (impl->seed + 4, cx, cy, impl->settings.extentChunks, 4) / 262144.0f - float (sample.height - impl->settings.seaLevel) / 192000.0f, 0.0f,
		1.0f);
	sample.sediment = float (std::clamp (sediment / 128.0, 0.0, 1.0));
	sample.material = sample.height < impl->settings.seaLevel + 256 || sample.moisture < 0.25f ? 4 : sample.normal[2] < 0.65f ? 3 : 1;
	return sample;
}
void PlanetGenerator::Prefetch (int64_t x, int64_t y) const
{
	(void)Height (x, y, 0, 0);
}
void PlanetGenerator::ClearRegionCache ()
{
	std::lock_guard lock (impl->mutex);
	impl->regions.clear ();
}
size_t PlanetGenerator::CachedRegions () const
{
	std::lock_guard lock (impl->mutex);
	return impl->regions.size ();
}
size_t PlanetGenerator::ResidentBytes () const
{
	std::lock_guard lock (impl->mutex);
	return impl->global.Bytes () + impl->regions.size () * size_t (Region + 1) * (Region + 1) * 12;
}
uint64_t PlanetGenerator::GlobalHash () const
{
	if (!Ready ())
		throw std::logic_error ("Planet global map has not been prepared");
	uint64_t hash = FnvOffset;
	for (auto b : EncodePlanetSettings (impl->settings))
		Digest (hash, b, 1);
	Digest (hash, impl->seed, 8);
	for (size_t i = 0; i < impl->global.height.size (); ++i)
	{
		Digest (hash, uint32_t (impl->global.height[i]), 4);
		Digest (hash, uint32_t (impl->global.sediment[i]), 4);
		Digest (hash, impl->global.flow[i], 4);
	}
	return hash;
}
bool PlanetGenerator::SaveCache (const std::string &path, std::string *error) const
{
	if (!Ready ())
		return Fail (error, "Cannot cache an unprepared planet");
	// Global state is immutable after publication. Use a temporary sibling and
	// rename so interruption never publishes a partial cache.
	std::string	  temporary = path + ".tmp." + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ());
	std::ofstream out (temporary, std::ios::binary | std::ios::trunc);
	if (!out)
		return Fail (error, "Cannot open planet cache for writing");
	uint64_t hash = FnvOffset;
	auto	 write = [&] (uint64_t value, int count)
	{
		Digest (hash, value, count);
		for (int i = 0; i < count; ++i)
		{
			out.put (char (value & 255));
			value >>= 8;
		}
	};
	out.write ("QVPLAN01", 8);
	for (auto b : EncodePlanetSettings (impl->settings))
		write (b, 1);
	write (impl->seed, 8);
	for (size_t i = 0; i < impl->global.height.size (); ++i)
	{
		write (uint32_t (impl->global.height[i]), 4);
		write (uint32_t (impl->global.sediment[i]), 4);
		write (impl->global.flow[i], 4);
	}
	write (hash, 8);
	out.flush ();
	bool good = out.good ();
	out.close ();
	std::error_code ec;
	if (good)
		std::filesystem::rename (temporary, path, ec);
	if (!good || ec)
	{
		std::filesystem::remove (temporary, ec);
		return Fail (error, "Cannot finish planet cache write");
	}
	return true;
}
bool PlanetGenerator::LoadCache (const std::string &path, std::string *error)
{
	std::lock_guard lock (impl->mutex);
	if (impl->ready.load ())
		return Fail (error, "Planet is already prepared");
	if (!ValidatePlanetSettings (impl->settings, error))
		return false;
	std::ifstream in (path, std::ios::binary);
	if (!in)
		return Fail (error, "Planet cache is unavailable");
	char magic[8]{};
	in.read (magic, 8);
	if (std::string (magic, 8) != "QVPLAN01")
		return Fail (error, "Invalid planet cache header");
	uint64_t hash = FnvOffset;
	auto	 read = [&] (int count)
	{
		uint64_t value = 0;
		for (int i = 0; i < count; ++i)
		{
			int byte = in.get ();
			if (byte == EOF)
				return uint64_t (0);
			value |= uint64_t (uint8_t (byte)) << (8 * i);
		}
		Digest (hash, value, count);
		return value;
	};
	for (auto b : EncodePlanetSettings (impl->settings))
		if (read (1) != b)
			return Fail (error, "Planet cache settings mismatch");
	if (read (8) != impl->seed)
		return Fail (error, "Planet cache seed mismatch");
	try
	{
		Grid candidate (int (impl->settings.globalResolution));
		for (size_t i = 0; i < candidate.height.size (); ++i)
		{
			candidate.height[i] = std::bit_cast<int32_t> (uint32_t (read (4)));
			candidate.sediment[i] = std::bit_cast<int32_t> (uint32_t (read (4)));
			candidate.flow[i] = uint32_t (read (4));
		}
		uint64_t expected = hash, actual = read (8);
		if (!in.good () || actual != expected || in.peek () != EOF)
			return Fail (error, "Planet cache is truncated or corrupt");
		impl->global = std::move (candidate);
		impl->ready.store (true);
		return true;
	}
	catch (const std::exception &e)
	{
		if (error)
			*error = e.what ();
		return false;
	}
}
} // namespace quiver
