/* SPDX-License-Identifier: GPL-2.0-or-later */
// Include the generator so the regression can check each private octave's
// periodicity as well as the exact R8 tile consumed by Vulkan.
#include "../Quake/noise_gen.c"
#include "cloud_coordinates.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// CPU-only engine services. Execute all slices before returning so this also
// tests the generator's real indexing and quantization without a Vulkan device.
void *Mem_Alloc (const size_t size)
{
	void *result = std::calloc (1, size);
	if (!result)
		throw std::bad_alloc ();
	return result;
}

void Mem_Free (const void *pointer)
{
	std::free (const_cast<void *> (pointer));
}

task_handle_t Task_Allocate (void)
{
	return 1;
}

void Task_AssignIndexedFunc (task_handle_t, task_indexed_func_t function, uint32_t limit, void *payload, size_t)
{
	std::atomic<uint32_t>	 next{0};
	std::vector<std::thread> workers;
	const unsigned			 count = q_min (8u, q_max (1u, std::thread::hardware_concurrency ()));
	for (unsigned i = 0; i < count; ++i)
		workers.emplace_back (
			[&]
			{
				for (uint32_t slice; (slice = next.fetch_add (1)) < limit;)
					function (slice, payload);
			});
	for (auto &worker : workers)
		worker.join ();
}

void	 Task_Submit (task_handle_t) {}
qboolean Task_Join (task_handle_t, uint32_t)
{
	return true;
}

namespace
{
void Check (bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error (message);
}

template <class Noise> void CheckPeriodic (Noise noise, int period)
{
	constexpr std::array<float, 5> fractions = {-0.0078125f, 0.0f, 0.0625f, 0.375f, 0.9921875f};
	for (float x : fractions)
		for (float y : fractions)
			for (float z : fractions)
			{
				const float a = x * period, b = y * period, c = z * period;
				const float original = noise (a, b, c);
				Check (std::isfinite (original), "finite periodic noise");
				for (int mask = 1; mask < 8; ++mask)
					for (int sign : {-1, 1})
					{
						const float translated =
							noise (a + ((mask & 1) ? sign * period : 0), b + ((mask & 2) ? sign * period : 0), c + ((mask & 4) ? sign * period : 0));
						Check (std::abs (original - translated) < 0.000002f, "noise repeats across faces, edges, corners, and negative cells");
					}
			}

	// Values immediately on either side of all seven tile boundaries must be
	// nearby, not merely equal after an exact whole-tile translation.
	constexpr float epsilon = 1.0f / 65536.0f;
	for (int mask = 1; mask < 8; ++mask)
	{
		std::array<float, 3> before = {0.375f, 0.25f, 0.625f}, after = before;
		for (int axis = 0; axis < 3; ++axis)
			if (mask & (1 << axis))
			{
				before[axis] = -epsilon;
				after[axis] = epsilon;
			}
		Check (
			std::abs (noise (before[0], before[1], before[2]) - noise (after[0], after[1], after[2])) < 0.002f,
			"noise is continuous across periodic faces, edges, and corners");
	}
}

void CheckTexture (int size)
{
	Check (size >= 2 && size <= 256, "texture measurement size is between 2 and 256");
	byte				   *data = Noise_Generate3DTexture (size);
	const size_t			count = size_t (size) * size * size;
	uint64_t				sum = 0, squares = 0;
	int						minimum = 255, maximum = 0;
	std::array<uint64_t, 3> seam{}, interior{};
	const auto				value = [&] (int x, int y, int z) { return int (data[(size_t (z) * size + y) * size + x]); };
	for (int z = 0; z < size; ++z)
		for (int y = 0; y < size; ++y)
			for (int x = 0; x < size; ++x)
			{
				const int sample = value (x, y, z);
				minimum = q_min (minimum, sample);
				maximum = q_max (maximum, sample);
				Check (sample >= 5, "R8 tile retains its nonzero density floor");
				sum += sample;
				squares += sample * sample;
				if (x + 1 < size)
					interior[0] += std::abs (sample - value (x + 1, y, z));
				if (y + 1 < size)
					interior[1] += std::abs (sample - value (x, y + 1, z));
				if (z + 1 < size)
					interior[2] += std::abs (sample - value (x, y, z + 1));
			}
	for (int a = 0; a < size; ++a)
		for (int b = 0; b < size; ++b)
		{
			seam[0] += std::abs (value (0, a, b) - value (size - 1, a, b));
			seam[1] += std::abs (value (a, 0, b) - value (a, size - 1, b));
			seam[2] += std::abs (value (a, b, 0) - value (a, b, size - 1));
		}
	const double mean = double (sum) / count / 255.0;
	const double variance = double (squares) / count / (255.0 * 255.0) - mean * mean;
	Check (mean > 0.5 && mean < 0.6, "cloud density mean stays near its existing character");
	Check (variance > 0.001 && variance < 0.01, "cloud texture retains density contrast");
	// sky_clouds_common.glsl uses this conservative upper bound to reject
	// unresolved, non-stochastic FBM octaves. Test every production R8 texel;
	// the stochastic base octave applies variance correction and is excluded.
	Check (double (maximum) / 255.0 <= 0.78, "R8 noise respects the shader's remaining-octave density upper bound");
	std::cout.precision (10);
	std::cout << "Cloud R8 " << size << " cubed: mean " << mean << " variance " << variance << " min " << minimum << " max " << maximum;
	for (int axis = 0; axis < 3; ++axis)
	{
		const double interior_mean = double (interior[axis]) / (size_t (size - 1) * size * size);
		const double seam_mean = double (seam[axis]) / (size_t (size) * size);
		Check (seam_mean < interior_mean * 1.5 + 0.25, "repeat seam gradient resembles interior neighbors");
		std::cout << " seam/interior " << seam_mean << '/' << interior_mean;
	}
	std::cout << '\n';
	Mem_Free (data);
}
void CheckCloudCoordinates ()
{
	for (int64_t extent : {512LL, 1250048LL, 2097152LL})
		for (float scale : {0.000075f, 0.00013f, -0.000075f})
		{
			const quiver::CloudCoordinates coordinates (scale, extent);
			const double				   actualScale = coordinates.cycles * coordinates.Period / (double (extent) * coordinates.Chunk);
			for (int64_t chunk : {-9007199254740993LL, -1LL, 0LL, 1LL, 9007199254740993LL})
			{
				Check (
					std::abs (std::remainder (coordinates.Phase (chunk, 123.5f) - coordinates.Phase (chunk + extent, 123.5f), 10.0)) < 1e-10,
					"wrapped aliases preserve cloud phase even beyond double integer precision");
				Check (
					std::abs (std::remainder (coordinates.Phase (chunk, 1023.5f) - coordinates.Phase (chunk + 1, -.5f), 10.0)) < 1e-10,
					"changing the local origin preserves cloud phase");
			}
			for (int64_t seam : {int64_t (0), extent / 2, extent})
			{
				const double before = coordinates.Phase (seam - 1, 1023.5f), after = coordinates.Phase (seam, .5f);
				Check (std::abs (std::remainder (after - before - actualScale, 10.0)) < 1e-10, "one-unit travel is continuous across the planet seam");
				const float gpuBefore = coordinates.Position (seam - 1, 1023.5f) * coordinates.scale;
				const float gpuAfter = coordinates.Position (seam, .5f) * coordinates.scale;
				Check (
					std::abs (std::remainder (double (gpuAfter) - gpuBefore - actualScale, 10.0)) < 2e-6,
					"GPU sampling eye remains continuous across origin and planet seams");
			}
		}
	const quiver::CloudCoordinates unwrapped (.000075f);
	for (int64_t chunk : {-9007199254740993LL, -132LL, -1LL, 0LL, 131LL, 9007199254740993LL})
		Check (
			std::abs (std::remainder (unwrapped.Phase (chunk, 1023.5f) - unwrapped.Phase (chunk + 1, -.5f), 10.0)) < 1e-9,
			"unwrapped large-world rebase preserves cloud phase");
	Check (quiver::CloudCoordinates (0).Position (1, 1) == 0, "zero scale has finite coordinates");
}
} // namespace

int main (int argc, char **argv)
{
	try
	{
		CheckCloudCoordinates ();
		for (int period : {4, 8, 16, 32, 64})
		{
			CheckPeriodic ([=] (float x, float y, float z) { return Noise_Perlin3D (x, y, z, period); }, period);
			CheckPeriodic ([=] (float x, float y, float z) { return Noise_Worley3D (x, y, z, period); }, period);
		}
		CheckPeriodic (Noise_CloudDensity3D, 4);
		CheckTexture (argc > 1 ? std::atoi (argv[1]) : 256);
		std::cout << "Periodic cloud noise tests passed\n";
		return 0;
	}
	catch (const std::exception &error)
	{
		std::cerr << "Cloud noise regression: " << error.what () << '\n';
		return 1;
	}
}
