/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_PLANET_CURVATURE_H
#define QUIVER_PLANET_CURVATURE_H

#include <algorithm>
#include <array>
#include <cmath>

namespace quiver
{
// Shared camera-relative visual transform. This is intentionally independent of
// gravity and collision coordinates. Do not enable it in one rendering pass:
// all world geometry, depth ray reconstruction, culling and targeting must use
// this same forward/inverse transform before the visual bend can be enabled.
struct PlanetCurvature
{
	static constexpr double EarthRadius = 6371000.0 * 32.0;
	double					radius = EarthRadius;
	double					flatDistance = 4096;
	double					blendDistance = 8192;

	// {downward displacement, radial derivative}. The parabolic continuation
	// beyond half a radius avoids a singularity and remains monotone.
	std::array<double, 2> Sag (double distance) const
	{
		if (!std::isfinite (radius) || radius <= 0 || !std::isfinite (distance) || distance <= flatDistance)
			return {};
		const double d = std::max (distance, 0.0), end = std::max (blendDistance, flatDistance + 1.0);
		const double u = std::clamp ((d - flatDistance) / (end - flatDistance), 0.0, 1.0);
		const double blend = u * u * u * (u * (u * 6 - 15) + 10);
		const double slope = 30 * u * u * (u - 1) * (u - 1) / (end - flatDistance);
		const double bounded = std::min (d, radius * 0.5);
		const double root = std::sqrt (radius * radius - bounded * bounded);
		const double base = bounded * bounded / (radius + root);
		const double beyond = d - bounded;
		const double sag = base + beyond * bounded / root + beyond * beyond / (2 * radius);
		const double derivative = bounded / root + beyond / radius;
		return {sag * blend, derivative * blend + sag * slope};
	}

	std::array<double, 3> Forward (std::array<double, 3> relative) const
	{
		relative[2] -= Sag (std::hypot (relative[0], relative[1]))[0];
		return relative;
	}
	std::array<double, 3> Inverse (std::array<double, 3> relative) const
	{
		relative[2] += Sag (std::hypot (relative[0], relative[1]))[0];
		return relative;
	}
	std::array<double, 3> Normal (const std::array<double, 3> &relative, std::array<double, 3> normal) const
	{
		const double d = std::hypot (relative[0], relative[1]);
		if (d > 0)
		{
			const double slope = Sag (d)[1] * normal[2] / d;
			normal[0] += relative[0] * slope;
			normal[1] += relative[1] * slope;
		}
		const double length = std::sqrt (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		if (length > 0)
			for (double &value : normal)
				value /= length;
		return normal;
	}
	void Bounds (std::array<double, 3> &minimum, std::array<double, 3> &maximum) const
	{
		double nearSquared = 0, farSquared = 0;
		for (int axis = 0; axis < 2; ++axis)
		{
			const double near = std::max ({minimum[axis], -maximum[axis], 0.0});
			const double far = std::max (std::abs (minimum[axis]), std::abs (maximum[axis]));
			nearSquared += near * near;
			farSquared += far * far;
		}
		minimum[2] -= Sag (std::sqrt (farSquared))[0];
		maximum[2] -= Sag (std::sqrt (nearSquared))[0];
	}
};
} // namespace quiver
#endif
