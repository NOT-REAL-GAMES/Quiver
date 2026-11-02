/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_features.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
namespace quiver
{
namespace
{
using P = SolidPoint;
P sub (P a, P b)
{
	for (int k = 0; k < 3; ++k)
		a[k] -= b[k];
	return a;
}
P add (P a, P b)
{
	for (int k = 0; k < 3; ++k)
		a[k] += b[k];
	return a;
}
P mul (P a, double b)
{
	for (double &x : a)
		x *= b;
	return a;
}
double dot (P a, P b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
P cross (P a, P b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
P unit (P a)
{
	double length = std::sqrt (dot (a, a));
	return length > 1e-15 ? mul (a, 1 / length) : P{0, 0, 1};
}
constexpr std::array<std::array<int, 2>, 12> edges{{{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
struct Vertex
{
	P		 position, normal;
	uint16_t material = 3;
	bool	 hard = false;
};
struct Hermite
{
	P		 position, normal;
	uint16_t material;
	bool	 hard;
};
P solveQEF (const std::vector<P> &points, const std::vector<P> &normals, const P &minimum, double size, bool smooth)
{
	P center{};
	for (P p : points)
		center = add (center, p);
	center = mul (center, 1.0 / points.size ());
	double a[3][4]{};
	for (size_t i = 0; i < points.size (); ++i)
	{
		P	   n = normals[i];
		double d = dot (n, sub (points[i], center));
		for (int r = 0; r < 3; ++r)
		{
			a[r][3] += n[r] * d;
			for (int c = 0; c < 3; ++c)
				a[r][c] += n[r] * n[c];
		}
	}
	// Smooth terrain's nearly parallel gradients make tangential coordinates
	// ill-conditioned. A modest centroid bias stabilizes those directions;
	// Hermite normals have unit length, so 1e-3 is one thousandth of a
	// single plane's weight. Both terms scale with squared cell size. Hard/mixed
	// construction retains its original near-exact corner solve.
	const double regularizer = smooth ? 1e-3 : 1e-7;
	for (int i = 0; i < 3; ++i)
		a[i][i] += regularizer;
	double system[3][4];
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c)
			system[r][c] = a[r][c];
	for (int c = 0; c < 3; ++c)
	{
		int pivot = c;
		for (int r = c + 1; r < 3; ++r)
			if (std::abs (a[r][c]) > std::abs (a[pivot][c]))
				pivot = r;
		for (int k = c; k < 4; ++k)
			std::swap (a[c][k], a[pivot][k]);
		double d = a[c][c];
		for (int k = c; k < 4; ++k)
			a[c][k] /= d;
		for (int r = 0; r < 3; ++r)
			if (r != c)
			{
				d = a[r][c];
				for (int k = c; k < 4; ++k)
					a[r][k] -= d * a[c][k];
			}
	}
	P		solution{a[0][3], a[1][3], a[2][3]};
	const P lower = sub (minimum, center), upper = add (lower, {size, size, size});
	bool	inside = true;
	for (int k = 0; k < 3; ++k)
		inside &= solution[k] >= lower[k] && solution[k] <= upper[k];
	if (!smooth || inside)
	{
		for (int k = 0; k < 3; ++k)
			center[k] = std::clamp (center[k] + solution[k], minimum[k], minimum[k] + size);
		return center;
	}
	// Clipping an escaped QEF solution independently on X/Y/Z can leave its
	// tangent planes, making gentle terrain into narrow risers. Minimize the
	// same convex quadratic on the cell instead. The constrained optimum is
	// either on a face, edge or corner; these 26 tiny active-set solves cover
	// every case. The common in-cell solution above keeps its fast path.
	auto error = [&] (const P &offset)
	{
		double value = regularizer * dot (offset, offset);
		for (size_t i = 0; i < points.size (); ++i)
		{
			const double distance = dot (normals[i], sub (add (center, offset), points[i]));
			value += distance * distance;
		}
		return value;
	};
	P	   best{}; // The mean of cell-edge roots is a feasible fallback.
	double bestError = error (best);
	for (int constraints = 1; constraints < 27; ++constraints)
	{
		P				   candidate{};
		std::array<int, 3> freeAxes{}, state{};
		int				   freeCount = 0, code = constraints;
		for (int axis = 0; axis < 3; ++axis, code /= 3)
		{
			state[axis] = code % 3;
			if (!state[axis])
				freeAxes[freeCount++] = axis;
			else
				candidate[axis] = state[axis] == 1 ? lower[axis] : upper[axis];
		}
		double reduced[3][4]{};
		for (int r = 0; r < freeCount; ++r)
		{
			const int axis = freeAxes[r];
			reduced[r][3] = system[axis][3];
			for (int k = 0; k < 3; ++k)
				if (state[k])
					reduced[r][3] -= system[axis][k] * candidate[k];
			for (int c = 0; c < freeCount; ++c)
				reduced[r][c] = system[axis][freeAxes[c]];
		}
		for (int c = 0; c < freeCount; ++c)
		{
			int pivot = c;
			for (int r = c + 1; r < freeCount; ++r)
				if (std::abs (reduced[r][c]) > std::abs (reduced[pivot][c]))
					pivot = r;
			for (int k = c; k < 4; ++k)
				std::swap (reduced[c][k], reduced[pivot][k]);
			const double diagonal = reduced[c][c];
			for (int k = c; k < 4; ++k)
				reduced[c][k] /= diagonal;
			for (int r = 0; r < freeCount; ++r)
				if (r != c)
				{
					const double factor = reduced[r][c];
					for (int k = c; k < 4; ++k)
						reduced[r][k] -= factor * reduced[c][k];
				}
		}
		bool feasible = true;
		for (int i = 0; i < freeCount; ++i)
		{
			const int axis = freeAxes[i];
			candidate[axis] = reduced[i][3];
			feasible &= candidate[axis] >= lower[axis] - 1e-9 && candidate[axis] <= upper[axis] + 1e-9;
			candidate[axis] = std::clamp (candidate[axis], lower[axis], upper[axis]);
		}
		if (!feasible)
			continue;
		const double candidateError = error (candidate);
		if (candidateError < bestError)
		{
			best = candidate;
			bestError = candidateError;
		}
	}
	for (int k = 0; k < 3; ++k)
		center[k] = std::clamp (center[k] + best[k], minimum[k], minimum[k] + size);
	return center;
}
} // namespace
void BuildFeatureSurface (
	const P &minimum, const std::array<int, 3> &count, double size, const FeatureSampler &sample, ChunkMesh &mesh, const FeatureDensitySampler &densityOnly)
{
	for (int c : count)
		if (c < 1 || c > 128)
			throw std::invalid_argument ("Feature mesh grid must contain 1..128 cells per axis");
	if (!std::isfinite (size) || size < 8)
		throw std::invalid_argument ("Feature mesh cells must be at least eight units");
	for (double p : minimum)
		if (!std::isfinite (p))
			throw std::invalid_argument ("Invalid feature mesh origin");
	const std::array<int, 3> dim{count[0] + 2, count[1] + 2, count[2] + 2};
	auto					 index = [&] (int x, int y, int z) { return (size_t (z + 1) * dim[1] + size_t (y + 1)) * dim[0] + size_t (x + 1); };
	auto					 position = [&] (int x, int y, int z) { return P{minimum[0] + x * size, minimum[1] + y * size, minimum[2] + z * size}; };
	auto					 densityAt = [&] (const P &p) { return densityOnly ? densityOnly (p) : sample (p).density; };
	std::vector<double>		 density (size_t (dim[0]) * dim[1] * dim[2]);
	for (int z = -1; z <= count[2]; ++z)
		for (int y = -1; y <= count[1]; ++y)
			for (int x = -1; x <= count[0]; ++x)
			{
				double d = densityAt (position (x, y, z));
				if (!std::isfinite (d))
					throw std::runtime_error ("Nonfinite feature density");
				density[index (x, y, z)] = d;
			}
	std::vector<Vertex>								   vertices;
	std::unordered_map<size_t, std::array<int32_t, 8>> cells;
	// Each grid edge belongs to four cells. Reuse the exact root and gradient
	// rather than evaluating the procedural field another17 times per owner.
	struct CachedHermite
	{
		size_t	key = size_t (-1);
		Hermite value;
	};
	// Cell iteration advances along Z. Two slices cover every shared edge and
	// bound scratch storage independently of the chunk's vertical complexity.
	std::vector<CachedHermite> intersections (size_t (dim[0]) * dim[1] * 6);
	for (int z = -1; z < count[2]; ++z)
		for (int y = -1; y < count[1]; ++y)
			for (int x = -1; x < count[0]; ++x)
			{
				std::array<double, 8> d;
				std::array<P, 8>	  p;
				int					  mask = 0;
				for (int c = 0; c < 8; ++c)
				{
					int cx = x + (c & 1), cy = y + ((c >> 1) & 1), cz = z + ((c >> 2) & 1);
					d[c] = density[index (cx, cy, cz)];
					p[c] = position (cx, cy, cz);
					if (d[c] >= 0)
						mask |= 1 << c;
				}
				if (mask == 0 || mask == 255)
					continue;
				// Each connected inside-corner component gets its own QEF. This avoids
				// connecting separate solids merely because they intersect the same cell.
				std::array<int, 8> component;
				for (int c = 0; c < 8; ++c)
					component[c] = c;
				auto root = [&] (int c)
				{
					while (component[c] != c)
						c = component[c];
					return c;
				};
				for (const auto &edge : edges)
					if (d[edge[0]] >= 0 && d[edge[1]] >= 0)
						component[root (edge[1])] = root (edge[0]);
				std::array<int32_t, 8> ids;
				ids.fill (-1);
				for (int c = 0; c < 8; ++c)
				{
					if (d[c] < 0 || root (c) != c)
						continue;
					std::vector<P> points, normals;
					P			   average{};
					bool		   hard = false;
					uint16_t	   material = 3;
					for (const auto &edge : edges)
					{
						int a = edge[0], b = edge[1];
						if ((d[a] >= 0) == (d[b] >= 0))
							continue;
						int inside = d[a] >= 0 ? a : b;
						if (root (inside) != c)
							continue;
						const int	 axis = (b - a) == 1 ? 0 : ((b - a) == 2 ? 1 : 2);
						const size_t edgeKey = index (x + (a & 1), y + ((a >> 1) & 1), z + ((a >> 2) & 1)) * 3 + axis;
						auto		&intersection = intersections[edgeKey % intersections.size ()];
						if (intersection.key != edgeKey)
						{
							P	 lo = p[a], hi = p[b];
							bool sign = d[a] >= 0;
							for (int iteration = 0; iteration < 16; ++iteration)
							{
								P mid = mul (add (lo, hi), .5);
								if ((densityAt (mid) >= 0) == sign)
									lo = mid;
								else
									hi = mid;
							}
							P		   at = mul (add (lo, hi), .5);
							const auto value = sample (at);
							intersection = {edgeKey, {at, unit (value.normal), value.material, value.hard}};
						}
						const auto &value = intersection.value;
						points.push_back (value.position);
						normals.push_back (value.normal);
						average = add (average, value.normal);
						hard |= value.hard;
						material = value.material;
					}
					if (points.empty ())
						continue;
					int32_t id = int32_t (vertices.size ());
					vertices.push_back ({solveQEF (points, normals, position (x, y, z), size, !hard), unit (average), material, hard});
					for (int corner = 0; corner < 8; ++corner)
						if (d[corner] >= 0 && root (corner) == c)
							ids[corner] = id;
				}
				cells.emplace (index (x, y, z), ids);
				if (vertices.size () > 1048576)
					throw std::runtime_error ("Feature mesh vertex budget exceeded");
			}
	auto emit = [&] (const Vertex &a, const Vertex &b, const Vertex &c)
	{
		P face = unit (cross (sub (b.position, a.position), sub (c.position, a.position))); // CCW outward before reversal.
		if (dot (cross (sub (b.position, a.position), sub (c.position, a.position)), cross (sub (b.position, a.position), sub (c.position, a.position))) <
			1e-16)
			return;
		if (mesh.vertices.size () > 3145725)
			throw std::runtime_error ("Feature triangle budget exceeded");
		uint32_t base = uint32_t (mesh.vertices.size ());
		for (const Vertex *v : {&a, &c, &b})
		{
			MeshVertex output;
			P		   normal = v->hard ? face : v->normal;
			for (int k = 0; k < 3; ++k)
			{
				output.position[k] = float (v->position[k]);
				output.normal[k] = float (normal[k]);
			}
			output.blend = TerrainClassifyMaterial (v->material, !v->hard);
			mesh.vertices.push_back (output);
		}
		mesh.indices.insert (mesh.indices.end (), {base, base + 1, base + 2});
	};
	for (int z = 0; z < count[2]; ++z)
		for (int y = 0; y < count[1]; ++y)
			for (int x = 0; x < count[0]; ++x)
				for (int axis = 0; axis < 3; ++axis)
				{
					std::array<int, 3> start{x, y, z}, end = start;
					++end[axis];
					double a = density[index (x, y, z)], b = density[index (end[0], end[1], end[2])];
					if ((a >= 0) == (b >= 0))
						continue;
					int					   u = (axis + 1) % 3, v = (axis + 2) % 3;
					std::array<int32_t, 4> ids;
					int					   n = 0;
					for (auto uv : std::array<std::array<int, 2>, 4>{{{-1, -1}, {0, -1}, {0, 0}, {-1, 0}}})
					{
						auto cell = start;
						cell[u] += uv[0];
						cell[v] += uv[1];
						auto found = cells.find (index (cell[0], cell[1], cell[2]));
						int	 corner = (a >= 0 ? 0 : 1) << axis;
						corner |= (-uv[0]) << u;
						corner |= (-uv[1]) << v;
						if (found == cells.end () || found->second[corner] < 0)
							throw std::runtime_error ("Missing feature cell component");
						ids[n++] = found->second[corner];
					}
					if (a < 0)
						std::reverse (ids.begin (), ids.end ());
					// A sharp crease can make this quad nonplanar. A fixed diagonal
					// then folds a thin triangle inward, producing bright/dark edge
					// speckles. Prefer the split whose worst triangle agrees most
					// closely with its Hermite normals; retain all shared positions.
					bool diagonal02 = true;
					if (vertices[ids[0]].hard || vertices[ids[1]].hard || vertices[ids[2]].hard || vertices[ids[3]].hard)
					{
						auto quality = [&] (int a, int b, int c)
						{
							const auto &v0 = vertices[ids[a]], &v1 = vertices[ids[b]], &v2 = vertices[ids[c]];
							P			n = cross (sub (v1.position, v0.position), sub (v2.position, v0.position));
							double		area = std::sqrt (dot (n, n));
							if (area < 1e-10)
								return 0.0;
							return dot (n, unit (add (add (v0.normal, v1.normal), v2.normal))) / area;
						};
						diagonal02 = std::min (quality (0, 1, 2), quality (0, 2, 3)) >= std::min (quality (0, 1, 3), quality (1, 2, 3));
					}
					if (diagonal02)
					{
						emit (vertices[ids[0]], vertices[ids[1]], vertices[ids[2]]);
						emit (vertices[ids[0]], vertices[ids[2]], vertices[ids[3]]);
					}
					else
					{
						emit (vertices[ids[0]], vertices[ids[1]], vertices[ids[3]]);
						emit (vertices[ids[1]], vertices[ids[2]], vertices[ids[3]]);
					}
				}
	mesh.smooth = true;
}
FeatureTrace SweepFeatureMesh (const ChunkMesh &mesh, const P &start, const P &offset, const P &mins, const P &maxs)
{
	FeatureTrace result;
	P			 center{}, extent{};
	for (int k = 0; k < 3; ++k)
	{
		if (!std::isfinite (start[k]) || !std::isfinite (offset[k]) || !std::isfinite (mins[k]) || !std::isfinite (maxs[k]) || mins[k] > maxs[k])
			return result;
		center[k] = start[k] + (mins[k] + maxs[k]) * .5;
		extent[k] = (maxs[k] - mins[k]) * .5;
	}
	for (size_t tri = 0; tri + 2 < mesh.indices.size (); tri += 3)
	{
		std::array<P, 3> t;
		bool			 valid = true;
		for (int i = 0; i < 3; ++i)
		{
			uint32_t index = mesh.indices[tri + i];
			if (index >= mesh.vertices.size ())
			{
				valid = false;
				break;
			}
			for (int k = 0; k < 3; ++k)
				t[i][k] = mesh.vertices[index].position[k];
		}
		if (!valid)
			continue;
		P				  e0 = sub (t[1], t[0]), e1 = sub (t[2], t[1]), e2 = sub (t[0], t[2]);
		std::array<P, 13> axes{};
		axes[0] = {1, 0, 0};
		axes[1] = {0, 1, 0};
		axes[2] = {0, 0, 1};
		axes[3] = cross (e0, e1);
		int n = 4;
		for (P edge : {e0, e1, e2})
			for (int k = 0; k < 3; ++k)
			{
				P axis{};
				axis[k] = 1;
				axes[n++] = cross (edge, axis);
			}
		struct Interval
		{
			double enter = -1e30, leave = 1e30;
			P	   normal{};
			bool   valid = true, inside = true;
		};
		auto intersect = [&] (const P &halfExtent)
		{
			Interval interval;
			for (P axis : axes)
			{
				if (dot (axis, axis) < 1e-20)
					continue;
				axis = unit (axis);
				double		 lo = std::min ({dot (t[0], axis), dot (t[1], axis), dot (t[2], axis)});
				double		 hi = std::max ({dot (t[0], axis), dot (t[1], axis), dot (t[2], axis)});
				const double radius = dot (halfExtent, {std::abs (axis[0]), std::abs (axis[1]), std::abs (axis[2])});
				const double p = dot (center, axis), velocity = dot (offset, axis);
				lo -= radius;
				hi += radius;
				// A hull resting against a plane can slide freely. A zero-width
				// ray still includes exact triangle/edge boundaries.
				const bool inside = radius > 1e-10 ? p > lo + 1e-8 && p < hi - 1e-8 : p >= lo - 1e-8 && p <= hi + 1e-8;
				interval.inside &= inside;
				if (std::abs (velocity) < 1e-14)
				{
					if (!inside)
					{
						interval.valid = false;
						break;
					}
					continue;
				}
				double a = (lo - p) / velocity, b = (hi - p) / velocity;
				P	   nearNormal = mul (axis, velocity > 0 ? -1 : 1);
				if (a > b)
					std::swap (a, b);
				if (a > interval.enter)
				{
					interval.enter = a;
					interval.normal = nearNormal;
				}
				interval.leave = std::min (interval.leave, b);
				if (interval.enter > interval.leave + 1e-10)
				{
					interval.valid = false;
					break;
				}
			}
			return interval;
		};
		const auto	 volume = intersect (extent);
		const P		 outward = mul (unit (axes[3]), -1); // clockwise render winding
		const double facing = dot (offset, outward);
		if (volume.valid && volume.leave >= 0 && volume.enter <= 1)
		{
			const double enter = std::max (0.0, volume.enter), leave = std::min (1.0, volume.leave);
			if (leave > enter + 1e-10)
				result.surfaceIntervals.emplace_back (enter, leave);
			// Zero-extent rays lying on a triangle are not volume penetration.
			if (extent[0] > 0 && extent[1] > 0 && extent[2] > 0)
				result.startSolid |= volume.inside;
			if (volume.enter >= -1e-8 && volume.enter <= result.fraction && facing < -1e-12)
			{
				result.hit = true;
				result.fraction = std::max (0.0, volume.enter);
				result.normal = volume.normal;
				result.triangle = uint32_t (tri / 3);
			}
		}
		if (volume.valid && std::abs (facing) > 1e-12)
		{
			const auto ray = intersect ({});
			if (ray.valid && ray.enter >= -1e-8 && ray.enter <= 1 && ray.leave >= 0)
				result.centerCrossings.emplace_back (std::max (0.0, ray.enter), facing < 0);
		}
	}
	return result;
}
} // namespace quiver
