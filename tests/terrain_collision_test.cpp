#include "terrain_collision.h"
#include "terrain_mesher.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace quiver;

namespace
{
using Vec3 = std::array<double, 3>;
void Check (bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error (message);
}
double Dot (const Vec3 &a, const Vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 Subtract (const Vec3 &a, const Vec3 &b)
{
	return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 Cross (const Vec3 &a, const Vec3 &b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
std::array<TerrainDensitySample, 8> Plane (double a, double b, double height)
{
	std::array<TerrainDensitySample, 8> samples;
	for (int i = 0; i < 8; ++i)
	{
		const double x = (i & 1) * VoxelSize, y = ((i >> 1) & 1) * VoxelSize, z = ((i >> 2) & 1) * VoxelSize;
		samples[i].density = float (height + a * x + b * y - z);
		samples[i].material = 7;
		samples[i].cell = {7, samples[i].density > 0 ? Solid : Empty};
	}
	return samples;
}
TerrainCollisionInterval First (const TerrainCubeTrace &trace)
{
	Check (trace.count > 0, "trace has a hit");
	return *std::min_element (
		trace.intervals.begin (), trace.intervals.begin () + trace.count, [] (const auto &a, const auto &b) { return a.enter < b.enter; });
}
bool Contains (const TerrainCubeTrace &trace, double time)
{
	for (size_t i = 0; i < trace.count; ++i)
		if (trace.intervals[i].enter < time && trace.intervals[i].exit > time)
			return true;
	return false;
}

void AnalyticTests ()
{
	const auto slope = Plane (0.25, 0.125, 4);
	auto	   hit = First (TraceTerrainCube (slope, {}, {3, 5, 12}, {0, 0, -16}));
	Check (std::abs (hit.enter - (12 - 5.375) / 16) < 1e-12, "ray hits unrounded sloping surface");
	Check (!hit.startSolid && hit.cell.contents == Solid, "surface hit is solid without starting inside");
	const double normalLength = std::sqrt (1 + 0.25 * 0.25 + 0.125 * 0.125);
	Check (std::abs (hit.normal[0] + 0.25 / normalLength) < 1e-7 && std::abs (hit.normal[2] - 1 / normalLength) < 1e-7, "contact uses triangle plane normal");
	hit = First (TraceTerrainCube (slope, {}, {3, 5, 12}, {0, 0, -16}, {-1, -2, -1}, {1, 2, 1}));
	Check (std::abs (hit.enter - (12 - 1 - 5.875) / 16) < 1e-12, "swept box rests on highest support point under its footprint");
	Check (!TraceTerrainCube (slope, {}, {3, 4, 6.75}, {1, 0, 0.25}, {-1, -2, -1}, {1, 2, 1}).count, "box slides tangentially on slope without penetration");
	Check (!TraceTerrainCube (Plane (0, 0, 4), {}, {3, 3, 5}, {2, 0, 0}, {-1, -1, -1}, {1, 1, 1}).count, "resting box slides across coplanar tetrahedra");
	for (double x : {0.0, 4.0, 8.0})
	{
		hit = First (TraceTerrainCube (slope, {}, {x, x, 12}, {0, 0, -16}));
		Check (std::abs (hit.enter - (12 - 4 - 0.375 * x) / 16) < 1e-12, "point ray cannot leak through body diagonal or cube edge");
	}
	hit = First (TraceTerrainCube (Plane (0, 0, 4), {}, {3, 5, 2}, {0, 0, 8}));
	Check (
		hit.startSolid && hit.enter < 0 && Contains (TraceTerrainCube (Plane (0, 0, 4), {}, {3, 5, 2}, {0, 0, 8}), 0.1),
		"starts-inside interval is retained for union coverage");
	Check (!Contains (TraceTerrainCube (Plane (0, 0, 4), {}, {3, 5, 2}, {0, 0, 8}), 0.5), "escaping solid leaves an uncovered interval");
	Check (!TraceTerrainCube (Plane (0, 0, -1), {}, {4, 4, 12}, {0, 0, -20}).count, "empty cube skips collision");
	hit = First (TraceTerrainCube (Plane (0, 0, 16), {}, {4, 4, 12}, {0, 0, -20}));
	Check (std::abs (hit.enter - 0.2) < 1e-12 && hit.normal[2] == 1, "solid cube fast path has exact entry normal");
	Check (!TraceTerrainCube (Plane (0, 0, 16), {}, {9, 4, 4}, {0, 1, 0}, {-1, -1, -1}, {1, 1, 1}).count, "box touching full-cube side slides freely");

	auto water = Plane (0, 0, 16);
	for (auto &sample : water)
		sample.cell.contents = Water;
	Check (!TraceTerrainCube (water, {}, {4, 4, 12}, {0, 0, -20}).count, "liquids remain nonblocking");
	auto unrounded = slope;
	for (auto &sample : unrounded)
		sample.cell = {};
	Check (
		First (TraceTerrainCube (unrounded, {}, {3, 5, 12}, {0, 0, -16})).cell.contents == Solid,
		"positive procedural density above quantized cells remains solid");
	const Vec3 translated{1000000, -1000000, 123456};
	hit = First (TraceTerrainCube (slope, translated, {translated[0] + 3, translated[1] + 5, translated[2] + 12}, {0, 0, -16}));
	Check (std::abs (hit.enter - (12 - 5.375) / 16) < 1e-12, "local-frame translation does not alter contact");
}

struct HalfSpace
{
	Vec3   normal;
	double distance;
};

// Independent reference: intersect halfspaces by enumerating triples of planes.
// Unlike the production SAT, this includes neither edge axes nor projections.
bool ReferenceOverlap (const std::array<TerrainDensitySample, 8> &samples, Vec3 center, Vec3 extent)
{
	for (const auto &tet : TerrainTetrahedra)
	{
		std::array<Vec3, 4>	  p;
		std::array<double, 4> density;
		int					  occupied = 0;
		for (int i = 0; i < 4; ++i)
		{
			const int corner = tet[i];
			p[i] = {double ((corner & 1) * 8), double (((corner >> 1) & 1) * 8), double (((corner >> 2) & 1) * 8)};
			density[i] = samples[corner].density;
			occupied += density[i] > 0;
		}
		if (!occupied)
			continue;
		std::vector<HalfSpace> planes;
		for (int opposite = 0; opposite < 4; ++opposite)
		{
			const Vec3 &a = p[(opposite + 1) % 4], &b = p[(opposite + 2) % 4], &c = p[(opposite + 3) % 4];
			Vec3		normal = Cross (Subtract (b, a), Subtract (c, a));
			if (Dot (normal, Subtract (p[opposite], a)) > 0)
				for (double &value : normal)
					value = -value;
			planes.push_back ({normal, Dot (normal, a)});
		}
		const Vec3	 a = Subtract (p[1], p[0]), b = Subtract (p[2], p[0]), c = Subtract (p[3], p[0]);
		const Vec3	 bc = Cross (b, c), ca = Cross (c, a), ab = Cross (a, b);
		const double determinant = Dot (a, bc);
		Vec3		 outward;
		for (int axis = 0; axis < 3; ++axis)
			outward[axis] = -((density[1] - density[0]) * bc[axis] + (density[2] - density[0]) * ca[axis] + (density[3] - density[0]) * ab[axis]) / determinant;
		planes.push_back ({outward, density[0] + Dot (outward, p[0])});
		for (int axis = 0; axis < 3; ++axis)
		{
			Vec3 normal{};
			normal[axis] = 1;
			planes.push_back ({normal, center[axis] + extent[axis]});
			normal[axis] = -1;
			planes.push_back ({normal, -center[axis] + extent[axis]});
		}
		for (size_t i = 0; i < planes.size (); ++i)
			for (size_t j = i + 1; j < planes.size (); ++j)
				for (size_t k = j + 1; k < planes.size (); ++k)
				{
					const auto	&a = planes[i], &b = planes[j], &c = planes[k];
					const Vec3	 bc = Cross (b.normal, c.normal), ca = Cross (c.normal, a.normal), ab = Cross (a.normal, b.normal);
					const double determinant = Dot (a.normal, bc);
					if (std::abs (determinant) < 1e-12)
						continue;
					Vec3 point;
					for (int axis = 0; axis < 3; ++axis)
						point[axis] = (a.distance * bc[axis] + b.distance * ca[axis] + c.distance * ab[axis]) / determinant;
					bool inside = true;
					for (const auto &plane : planes)
						if (Dot (plane.normal, point) > plane.distance + 1e-8)
						{
							inside = false;
							break;
						}
					if (inside)
						return true;
				}
	}
	return false;
}

void SatReferenceTests ()
{
	std::mt19937						   random (7919);
	std::uniform_real_distribution<double> coordinate (-4, 12), scalar (-8, 8), width (0.05, 3);
	for (int iteration = 0; iteration < 160; ++iteration)
	{
		std::array<TerrainDensitySample, 8> samples;
		for (auto &sample : samples)
		{
			sample.density = float (scalar (random));
			sample.cell = {1, sample.density > 0 ? Solid : Empty};
		}
		const Vec3				   start{coordinate (random), coordinate (random), coordinate (random)}, extent{width (random), width (random), width (random)};
		const std::array<float, 3> mins{float (-extent[0]), float (-extent[1]), float (-extent[2])},
			maxs{float (extent[0]), float (extent[1]), float (extent[2])};
		const Vec3 exactExtent{maxs[0], maxs[1], maxs[2]};
		const auto stationary = TraceTerrainCube (samples, {}, start, {}, mins, maxs);
		Check (bool (stationary.count) == ReferenceOverlap (samples, start, exactExtent), "SAT matches independent convex halfspace intersection");
		if (iteration < 32)
		{
			const Vec3 offset{scalar (random), scalar (random), scalar (random)};
			const auto moving = TraceTerrainCube (samples, {}, start, offset, mins, maxs);
			for (int step = 1; step < 16; ++step)
			{
				const double time = double (step) / 16;
				Vec3		 position;
				for (int axis = 0; axis < 3; ++axis)
					position[axis] = start[axis] + time * offset[axis];
				Check (Contains (moving, time) == ReferenceOverlap (samples, position, exactExtent), "swept intervals match independent intermediate overlaps");
			}
		}
	}
}

void SharedDensityTests ()
{
	WorldConfig config;
	config.smoothTerrain = false;
	TerrainNeighborhood chunks;
	for (ChunkKey key : {ChunkKey{3, -2, 0}, ChunkKey{9007199254740993LL, -9007199254740993LL, 0}, ChunkKey{INT64_MAX, INT64_MIN, 0}})
	{
		for (TerrainDensityPoint point : {TerrainDensityPoint{-1, 128, 12}, TerrainDensityPoint{24, 58, 14}, TerrainDensityPoint{128, -1, -1}})
		{
			const auto sample = TerrainSampleDensity (config, key, point, chunks);
			Check (std::isfinite (sample.density), "density remains finite at huge chunk coordinates");
			if (point[0] >= 0 && point[0] < 128 && point[1] >= 0 && point[1] < 128 && point[2] >= 0)
			{
				const float height = TerrainSurfaceHeight (config.seed, key.x, key.y, float (point[0] * 8 + 4), float (point[1] * 8 + 4));
				Check (sample.density == std::clamp (height - float (point[2] * 8 + 4), -8.0f, 8.0f), "shared sample uses exact renderer scalar field");
			}
		}
	}
	const ChunkKey key{9007199254740993LL, -9007199254740993LL, 0};
	for (int z = 7; z <= 17; ++z)
		Check (
			TerrainSampleDensity (config, key, {128, 63, z}, chunks).density ==
				TerrainSampleDensity (config, {key.x + 1, key.y, 0}, {0, 63, z}, chunks).density,
			"canonical halo gives identical cross-chunk density");

	World world (config);
	Check (world.Require ({3, -2, 0}), "load density test chunk");
	chunks[13] = world.Find ({3, -2, 0});
	auto blocks = world.Mesh ({3, -2, 0}, true);
	Check (bool (blocks), "build source mesh for physics comparison");
	ChunkMesh mesh = *blocks;
	BuildSmoothTerrainMesh (config, chunks, mesh);
	Check (!mesh.indices.empty (), "mesh comparison has triangles");
	const size_t stride = std::max (size_t (3), mesh.indices.size () / 128 / 3 * 3);
	size_t		 comparisons = 0;
	for (size_t triangle = 0; triangle + 2 < mesh.indices.size (); triangle += stride)
	{
		Vec3 center{};
		for (int i = 0; i < 3; ++i)
			for (int axis = 0; axis < 3; ++axis)
				center[axis] += double (mesh.vertices[mesh.indices[triangle + i]].position[axis]) / 3;
		TerrainDensityPoint					origin;
		Vec3								minimum;
		std::array<TerrainDensitySample, 8> samples;
		for (int axis = 0; axis < 3; ++axis)
		{
			origin[axis] = int (std::floor ((center[axis] - 4) / 8));
			minimum[axis] = origin[axis] * 8 + 4;
		}
		for (int i = 0; i < 8; ++i)
			samples[i] = TerrainSampleDensity (config, {3, -2, 0}, {origin[0] + (i & 1), origin[1] + ((i >> 1) & 1), origin[2] + ((i >> 2) & 1)}, chunks);
		Vec3 start = center;
		start[2] = minimum[2] + 9;
		const auto hit = First (TraceTerrainCube (samples, minimum, start, {0, 0, -10}));
		Check (std::abs (start[2] - 10 * hit.enter - center[2]) < 0.00003, "collision and rendered triangle have matching heights");
		++comparisons;
	}
	Check (comparisons > 100, "compared rendered surface across the chunk");
}
} // namespace

int main ()
{
	try
	{
		AnalyticTests ();
		SatReferenceTests ();
		SharedDensityTests ();
		std::cout << "terrain collision tests passed\n";
		return 0;
	}
	catch (const std::exception &error)
	{
		std::cerr << "terrain collision test failed: " << error.what () << '\n';
		return 1;
	}
}
