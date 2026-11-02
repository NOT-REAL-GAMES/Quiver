/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_collision.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace quiver
{
namespace
{
using Vec3 = std::array<double, 3>;

Vec3 Subtract (const Vec3 &a, const Vec3 &b)
{
	return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 Cross (const Vec3 &a, const Vec3 &b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Dot (const Vec3 &a, const Vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

struct Vertex
{
	Vec3	 position;
	// Bits 0..3 are original tetrahedron faces; bit 4 is the density plane.
	unsigned faces = 0;
};

class Sweep
{
  public:
	Sweep (const Vec3 &origin, const Vec3 &movement, const std::array<float, 3> &minimum, const std::array<float, 3> &maximum)
		: start (origin), offset (movement), mins (minimum), maxs (maximum)
	{
		interval.enter = -std::numeric_limits<double>::infinity ();
		interval.exit = std::numeric_limits<double>::infinity ();
		interval.startSolid = true;
	}

	void Add (Vec3 position, unsigned faces)
	{
		for (size_t i = 0; i < count; ++i)
			if (Dot (Subtract (position, vertices[i].position), Subtract (position, vertices[i].position)) < 1e-24)
			{
				vertices[i].faces |= faces;
				return;
			}
		vertices[count++] = {position, faces};
	}

	bool Axis (Vec3 axis)
	{
		const double squared = Dot (axis, axis);
		if (squared < 1e-24)
			return true;
		double low = Dot (axis, vertices[0].position), high = low;
		for (size_t i = 1; i < count; ++i)
		{
			const double projection = Dot (axis, vertices[i].position);
			low = std::min (low, projection);
			high = std::max (high, projection);
		}
		double boxLow = 0, boxHigh = 0;
		for (int component = 0; component < 3; ++component)
		{
			boxLow += axis[component] * (axis[component] < 0 ? maxs[component] : mins[component]);
			boxHigh += axis[component] * (axis[component] < 0 ? mins[component] : maxs[component]);
		}
		low -= boxHigh;
		high -= boxLow;
		const double origin = Dot (axis, start), velocity = Dot (axis, offset);
		// A full box needs positive-volume intersection. A point ray (or a
		// zero-width projection of a degenerate box) includes shared boundaries.
		const bool	 pointAxis = boxHigh == boxLow;
		const bool	 inside = pointAxis ? origin >= low && origin <= high : origin > low && origin < high;
		interval.startSolid &= inside;
		if (velocity == 0)
			return inside;
		double enter = (low - origin) / velocity, leave = (high - origin) / velocity;
		double sign = -1;
		if (enter > leave)
		{
			std::swap (enter, leave);
			sign = 1;
		}
		if (enter > interval.enter)
		{
			interval.enter = enter;
			for (int component = 0; component < 3; ++component)
				normal[component] = sign * axis[component];
		}
		interval.exit = std::min (interval.exit, leave);
		return interval.enter < interval.exit && interval.exit > 0 && interval.enter <= 1;
	}

	bool BoxAxes ()
	{
		return Axis ({1, 0, 0}) && Axis ({0, 1, 0}) && Axis ({0, 0, 1});
	}

	bool EdgeAxes ()
	{
		for (size_t i = 0; i < count; ++i)
			for (size_t j = i + 1; j < count; ++j)
			{
				// Edges are intersections of two clipping planes. Diagonals of
				// a quad face need no axes. This keeps the exact SAT small.
				if (std::popcount (vertices[i].faces & vertices[j].faces) < 2)
					continue;
				const Vec3 edge = Subtract (vertices[j].position, vertices[i].position);
				if (!Axis ({0, edge[2], -edge[1]}) || !Axis ({-edge[2], 0, edge[0]}) || !Axis ({edge[1], -edge[0], 0}))
					return false;
			}
		return true;
	}

	TerrainCollisionInterval Result (Cell cell)
	{
		interval.cell = cell;
		const double length = std::sqrt (Dot (normal, normal));
		if (length > 0)
			for (int axis = 0; axis < 3; ++axis)
				interval.normal[axis] = float (normal[axis] / length);
		return interval;
	}

  private:
	const Vec3				   &start, &offset;
	const std::array<float, 3> &mins, &maxs;
	std::array<Vertex, 8>		vertices;
	size_t						count = 0;
	TerrainCollisionInterval	interval;
	Vec3						normal{};
};

Cell ContactCell (const TerrainDensitySample &sample)
{
	Cell cell = sample.cell;
	if (!cell.IsSolid ())
		cell = {sample.material, Solid};
	return cell;
}
} // namespace

TerrainCubeTrace TraceTerrainCube (
	const std::array<TerrainDensitySample, 8> &samples, const Vec3 &minimum, const Vec3 &start, const Vec3 &offset, const std::array<float, 3> &mins,
	const std::array<float, 3> &maxs)
{
	TerrainCubeTrace	  result;
	std::array<double, 8> density;
	std::array<Vec3, 8>	  points;
	int					  occupied = 0;
	Cell				  cell{0, Solid};
	for (int i = 0; i < 8; ++i)
	{
		const int contents = samples[i].cell.contents;
		density[i] = contents == Water || contents == Slime || contents == Lava ? -double (VoxelSize / 2) : double (samples[i].density);
		if (density[i] > 0)
		{
			++occupied;
			cell = ContactCell (samples[i]);
		}
		points[i] = {double ((i & 1) * VoxelSize), double (((i >> 1) & 1) * VoxelSize), double (((i >> 2) & 1) * VoxelSize)};
	}
	if (!occupied)
		return result;
	const Vec3 origin = Subtract (start, minimum);
	if (occupied == 8)
	{
		Sweep sweep (origin, offset, mins, maxs);
		for (const Vec3 &point : points)
			sweep.Add (point, 0);
		if (sweep.BoxAxes ())
			result.intervals[result.count++] = sweep.Result (cell);
		return result;
	}
	for (const auto &tetrahedron : TerrainTetrahedra)
	{
		std::array<Vec3, 4>	  positions;
		std::array<double, 4> values;
		int					  solid = 0;
		for (int i = 0; i < 4; ++i)
		{
			positions[i] = points[tetrahedron[i]];
			values[i] = density[tetrahedron[i]];
			if (values[i] > 0)
			{
				++solid;
				cell = ContactCell (samples[tetrahedron[i]]);
			}
		}
		if (!solid)
			continue;
		Sweep sweep (origin, offset, mins, maxs);
		for (int i = 0; i < 4; ++i)
		{
			if (values[i] > 0)
				sweep.Add (positions[i], 15u ^ (1u << i));
			for (int j = i + 1; j < 4; ++j)
				if ((values[i] > 0) != (values[j] > 0))
				{
					const double fraction = values[i] / (values[i] - values[j]);
					Vec3		 point;
					for (int axis = 0; axis < 3; ++axis)
						point[axis] = positions[i][axis] + fraction * (positions[j][axis] - positions[i][axis]);
					unsigned faces = (15u ^ ((1u << i) | (1u << j))) | 16u;
					if (values[i] == 0)
						faces = (15u ^ (1u << i)) | 16u;
					else if (values[j] == 0)
						faces = (15u ^ (1u << j)) | 16u;
					sweep.Add (point, faces);
				}
		}
		if (!sweep.BoxAxes ())
			continue;
		bool intersects = true;
		for (int opposite = 0; opposite < 4; ++opposite)
		{
			const Vec3 &a = positions[(opposite + 1) % 4], &b = positions[(opposite + 2) % 4], &c = positions[(opposite + 3) % 4];
			if (!sweep.Axis (Cross (Subtract (b, a), Subtract (c, a))))
			{
				intersects = false;
				break;
			}
		}
		if (!intersects)
			continue;
		if (solid != 4)
		{
			// Gradient of the affine scalar field inside this tetrahedron. Its
			// zero plane is exactly the triangle plane used by the renderer.
			const Vec3 a = Subtract (positions[1], positions[0]), b = Subtract (positions[2], positions[0]), c = Subtract (positions[3], positions[0]);
			const Vec3 bc = Cross (b, c), ca = Cross (c, a), ab = Cross (a, b);
			Vec3	   gradient;
			for (int axis = 0; axis < 3; ++axis)
				gradient[axis] = (values[1] - values[0]) * bc[axis] + (values[2] - values[0]) * ca[axis] + (values[3] - values[0]) * ab[axis];
			if (!sweep.Axis (gradient))
				continue;
		}
		if (sweep.EdgeAxes ())
			result.intervals[result.count++] = sweep.Result (cell);
	}
	return result;
}
} // namespace quiver
