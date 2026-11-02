/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace quiver
{
// Every density component repeats after ten base UV units, including the
// stochastic selector and the 0.1/0.7 broad/warp frequencies. A wrapped world
// must contain an integer number of those periods, not merely wrap its terrain.
struct CloudCoordinates
{
	static constexpr double Period = 10;
	static constexpr double Chunk = 1024;
	double					cycles = 0;
	int64_t					extent = 0;
	float					scale;

	explicit CloudCoordinates (float requested, int64_t planetExtent = 0) : extent (planetExtent), scale (requested)
	{
		if (!std::isfinite (requested) || std::abs (requested) < 1e-12f)
		{
			scale = 0;
			extent = 0;
		}
		else if (extent > 0)
		{
			cycles = std::copysign (std::max (1.0, std::round (std::abs (double (requested)) * (double (extent) * Chunk) / Period)), requested);
			scale = float (cycles * Period / (double (extent) * Chunk));
		}
	}

	double Phase (int64_t chunk, float local) const
	{
		if (!scale)
			return 0;
		double phase;
		if (extent > 0)
		{
			int64_t wrapped = chunk % extent;
			if (wrapped < 0)
				wrapped += extent;
			// Reduce the integer product exactly before conversion to float.
			// Planet extents are at most 2^21 chunks, so this product fits uint64.
			const uint64_t residue = uint64_t (std::fmod (std::abs (cycles), double (extent)));
			phase = std::copysign (Period * double ((uint64_t (wrapped) * residue) % uint64_t (extent)) / double (extent), cycles);
			phase += double (local) * (cycles * Period / (double (extent) * Chunk));
		}
		else
		{
			uint64_t count = uint64_t (chunk);
			if (chunk < 0)
				count = uint64_t (0) - count;
			double step = std::fmod (Chunk * double (scale), Period);
			phase = 0;
			while (count)
			{
				if (count & 1)
					phase = std::fmod (phase + step, Period);
				step = std::fmod (step * 2, Period);
				count >>= 1;
			}
			if (chunk < 0)
				phase = -phase;
			phase += double (local) * scale;
		}
		return std::remainder (phase, Period);
	}

	float Position (int64_t chunk, float local) const
	{
		return scale ? float (Phase (chunk, local) / scale) : 0;
	}
};
} // namespace quiver
