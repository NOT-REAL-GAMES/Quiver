/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "planet_generation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace quiver;
namespace
{
void Check (bool pass, const char *message)
{
	if (!pass)
		throw std::runtime_error (message);
}
void SettingsTests ()
{
	PlanetSettings s, decoded;
	Check (s.version == 2 && ValidatePlanetSettings (s), "new worlds default to generator version 2");
	auto bytes = EncodePlanetSettings (s);
	Check (bytes.size () == 36 && DecodePlanetSettings (bytes, decoded) && decoded == s, "settings roundtrip");
	bytes[0] = 255;
	Check (!DecodePlanetSettings (bytes, decoded) && decoded == s, "unknown version rejected transactionally");
	bytes.pop_back ();
	Check (!DecodePlanetSettings (bytes, decoded), "truncated settings rejected");
	s.extentChunks++;
	Check (!ValidatePlanetSettings (s), "misaligned extent rejected");
	s = {};
	s.regionalHydraulicIterations = 100;
	Check (!ValidatePlanetSettings (s), "unbounded erosion rejected");
	Check (CanonicalPlanetChunk (-1, 1024) == 1023 && CanonicalPlanetChunk (1024, 1024) == 0, "canonical wrap");
	Check (NearestPlanetChunkDelta (0, 1023, 1024) == 1 && NearestPlanetChunkDelta (1023, 0, 1024) == -1, "nearest periodic image");
	Check (
		NearestPlanetChunkDelta (std::numeric_limits<int64_t>::max () - 1, 0, std::numeric_limits<int64_t>::max ()) == -1, "maximum positive wrapping period");
	Check (CanonicalPlanetChunk (std::numeric_limits<int64_t>::min (), 1024) == 0, "int64 minimum canonicalization");
	Check (NearestPlanetChunkDelta (std::numeric_limits<int64_t>::max (), std::numeric_limits<int64_t>::min (), 1024) == -1, "extreme nearest delta");
}
void GeneratorTests ()
{
	PlanetSettings s;
	s.version = 1;
	s.globalResolution = 64;
	PlanetGenerator a (s, 123, 2), b (s, 123, 2), changed (s, 124, 1);
	bool			unprepared = false;
	try
	{
		(void)a.Height (0, 0, 0, 0);
	}
	catch (const std::logic_error &)
	{
		unprepared = true;
	}
	Check (unprepared, "sampling requires explicit preparation");
	unsigned callbacks = 0;
	Check (
		a.Prepare (
			nullptr,
			[&] (uint32_t done, uint32_t total)
			{
				Check (done <= total, "progress bounds");
				++callbacks;
			}),
		"prepare a");
	Check (callbacks == 22, "preparation progress reported");
	Check (b.Prepare () && changed.Prepare (), "prepare peers");
	Check (a.GlobalHash () == b.GlobalHash () && a.GlobalHash () != changed.GlobalHash (), "deterministic maps and distinct seeds");
	Check (a.GlobalHash () == 14816413636722609600ULL, "generator version 1 golden hash");
	Check (
		a.Height (0, 0, 512, 512) * 65536 == 8616879 && a.Height (0, 0, 2048, 2048) * 65536 == 24171403 && a.Height (0, 0, 8192, 4096) * 65536 == 156012463 &&
			a.Height (0, 0, -1024, 2048) * 65536 == 18798416,
		"version 1 spawn and regional heights remain unchanged");
	auto noErosion = s;
	noErosion.globalHydraulicIterations = noErosion.globalThermalIterations = 0;
	PlanetGenerator uneroded (noErosion, 123);
	Check (uneroded.Prepare () && a.GlobalHash () != uneroded.GlobalHash (), "erosion changes map");
	Check (a.Height (256, 777, 512, 512) != uneroded.Height (256, 777, 512, 512), "erosion changes sampled elevations");
	a.ClearRegionCache ();
	for (int i = 0; i < 20; ++i)
	{
		auto coarse = a.SampleCoarse (i * 1000, i * 3000, 512, 512);
		Check (std::isfinite (coarse.height), "finite far approximation");
		Check (coarse.height == a.HeightCoarse (i * 1000 + s.extentChunks, i * 3000, 512, 512), "periodic far approximation");
	}
	Check (a.CachedRegions () == 0, "far samples never allocate regional maps");
	auto localUnmodified = s;
	localUnmodified.regionalHydraulicIterations = localUnmodified.regionalThermalIterations = 0;
	PlanetGenerator coarseReference (localUnmodified, 123);
	Check (coarseReference.Prepare (), "prepare unmodified regional reference");
	Check (
		coarseReference.Height (5000, 6000, 734.25, 44.5) == coarseReference.HeightCoarse (5000, 6000, 734.25, 44.5),
		"coarse samples reproduce pre-regional terrain");
	Check (a.Height (0, 0, 128, 128) == 128 && a.Sample (0, 0, 128, 128).normal[2] == 1, "dry flat spawn plateau");
	std::vector<std::array<int64_t, 2>> points{{256, 777}, {511, -1}, {1024, 400}, {-1, -1}, {s.extentChunks - 1, s.extentChunks - 1}};
	std::vector<double>					expected;
	for (auto p : points)
		expected.push_back (a.Height (p[0], p[1], 731.25, 23.5));
	for (size_t i = points.size (); i-- > 0;)
	{
		auto p = points[i];
		Check (b.Height (p[0], p[1], 731.25, 23.5) == expected[i], "generation order independence");
		Check (b.Height (p[0] + s.extentChunks, p[1] - s.extentChunks, 731.25, 23.5) == expected[i], "periodic height identity");
		Check (b.Height (p[0] + 1, p[1], -292.75, 23.5) == expected[i], "equivalent local coordinate identity");
	}
	Check (a.CachedRegions () == 2 && b.CachedRegions () == 2, "bounded LRU");
	a.ClearRegionCache ();
	Check (a.CachedRegions () == 0 && a.Height (points[0][0], points[0][1], 731.25, 23.5) == expected[0], "cache eviction regeneration identity");
	for (int boundary : {0, 256, 512})
		for (double y : {0.0, 101.5, 1023.5})
		{
			double left = a.Height (boundary, 777, -1.0 / 256, y), right = a.Height (boundary, 777, 0, y);
			Check (std::abs (left - right) < 0.5, "erosion region seam continuity");
			Check (right == a.Height (boundary - 1, 777, 1024, y), "chunk shared endpoint identity");
		}
	auto   sample = a.Sample (300, 300, 512, 512);
	double norm = 0;
	for (float v : sample.normal)
		norm += v * v;
	Check (std::abs (norm - 1) < 1e-5 && sample.moisture >= 0 && sample.moisture <= 1 && sample.material != 0, "surface normal and material bounds");
	std::vector<std::thread> workers;
	for (int i = 0; i < 4; ++i)
		workers.emplace_back (
			[&]
			{
				for (int j = 0; j < 20; ++j)
					Check (a.Height (0, 0, 128, 128) == 128, "parallel sampling");
			});
	for (auto &worker : workers)
		worker.join ();
	const auto path =
		std::filesystem::temp_directory_path () / ("quiver-planet-test-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
	Check (a.SaveCache (path.string ()), "save cache");
	PlanetGenerator loaded (s, 123), wrong (s, 124), corrupt (s, 123);
	Check (!wrong.LoadCache (path.string ()) && !wrong.Ready (), "cache identity rejects wrong seed");
	Check (loaded.LoadCache (path.string ()) && loaded.GlobalHash () == a.GlobalHash (), "portable cache identity");
	Check (loaded.Height (300, 300, 512, 512) == sample.height, "cache sample identity");
	std::fstream file (path, std::ios::in | std::ios::out | std::ios::binary);
	file.seekp (100);
	file.put ('x');
	file.close ();
	Check (!corrupt.LoadCache (path.string ()) && !corrupt.Ready (), "corrupt cache rejected");
	std::filesystem::remove (path);
	std::cout << "global_hash=" << a.GlobalHash () << " resident_bytes=" << a.ResidentBytes () << '\n';
}
void SpawnTests ()
{
	PlanetSettings settings;
	settings.globalResolution = 64;
	std::vector<double> seededHeights;
	double				worstSlope = 0, worstRock = 0, worstRockSlope = 0;
	for (uint64_t seed : std::array<uint64_t, 6>{0, 1, 2, 123, 777, UINT64_MAX})
	{
		PlanetGenerator planet (settings, seed, 4);
		Check (planet.Prepare (), "prepare seeded lowland spawn");
		Check (planet.Height (0, 0, 128, 128) == 128 && planet.Sample (0, 0, 128, 128).normal[2] == 1, "new spawn remains dry and level");
		seededHeights.push_back (planet.Height (0, 0, 4096, 2048));
		for (int y = -16384; y <= 16384; y += 256)
			for (int x = -16384; x <= 16384; x += 256)
			{
				const auto	 sample = planet.Sample (0, 0, 128 + x, 128 + y);
				const double slope = std::hypot (sample.normal[0], sample.normal[1]) / sample.normal[2];
				const double rock = 160.0 * (1.0 - sample.normal[2]);
				// RockNoise interpolates [-1,1] on 64-unit cells: each gradient
				// component is bounded by 2*1.5/64. Account for its effect on both
				// horizontal and vertical derivatives, rather than checking only
				// the heightfield normal which originally hid the spawn obstacle.
				const double derivative = rock * 3.0 / 64.0;
				const double displacedSlope = (slope + std::sqrt (2.0) * derivative) / (1.0 - derivative);
				Check (slope < .2 && derivative < .1 && displacedSlope < .4, "walkable spawn exit including 3-D rock displacement");
				worstSlope = std::max (worstSlope, slope);
				worstRock = std::max (worstRock, rock);
				worstRockSlope = std::max (worstRockSlope, displacedSlope);
				Check (sample.height == planet.Height (settings.extentChunks, -settings.extentChunks, 128 + x, 128 + y), "new spawn wraps exactly");
			}
		if (seed == 123)
		{
			Check (planet.GlobalHash () == 7171805055037505853ULL, "generator version 2 golden hash");
			const auto path = std::filesystem::temp_directory_path () /
							  ("quiver-planet-v2-test-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
			PlanetGenerator restored (settings, seed);
			auto			legacySettings = settings;
			legacySettings.version = 1;
			PlanetGenerator legacy (legacySettings, seed);
			Check (planet.SaveCache (path.string ()) && restored.LoadCache (path.string ()), "version 2 cache roundtrip");
			Check (
				restored.GlobalHash () == planet.GlobalHash () && restored.Height (0, 0, 4096, 2048) == seededHeights.back (),
				"restored lowland map is not transformed twice");
			Check (!legacy.LoadCache (path.string ()), "generator cache versions remain distinct");
			std::filesystem::remove (path);
		}
	}
	Check (
		std::adjacent_find (seededHeights.begin (), seededHeights.end (), std::not_equal_to<double> ()) != seededHeights.end (),
		"spawn surroundings vary by seed");
	std::cout << "spawn_max_slope=" << worstSlope << " rock_amplitude=" << worstRock << " conservative_displaced_slope=" << worstRockSlope << '\n';
}
void ConcurrentRegionTests ()
{
	PlanetSettings settings;
	settings.globalResolution = 64;
	PlanetGenerator planet (settings, 1, 2), reference (settings, 1, 2);
	Check (planet.Prepare () && reference.Prepare (), "prepare concurrent region fixtures");
	std::array<double, 4>	 actual{}, expected{};
	std::vector<std::thread> workers;
	for (size_t i = 0; i < actual.size (); ++i)
	{
		expected[i] = reference.Height (int64_t (i) * 1024, 5000, 777, 333);
		workers.emplace_back ([&, i] { actual[i] = planet.Height (int64_t (i) * 1024, 5000, 777, 333); });
	}
	for (auto &worker : workers)
		worker.join ();
	Check (actual == expected && planet.CachedRegions () == 2, "concurrent cold generation is deterministic and cache remains bounded");
}
} // namespace
int main ()
{
	try
	{
		SettingsTests ();
		GeneratorTests ();
		SpawnTests ();
		ConcurrentRegionTests ();
		std::cout << "planet generation tests passed\n";
		return 0;
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what () << '\n';
		return 1;
	}
}
