/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world_solids.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
namespace quiver
{
namespace
{
using P = SolidPoint;
P sub (P a, P b)
{
	for (int i = 0; i < 3; ++i)
		a[i] -= b[i];
	return a;
}
P add (P a, P b)
{
	for (int i = 0; i < 3; ++i)
		a[i] += b[i];
	return a;
}
P mul (P a, double s)
{
	for (double &v : a)
		v *= s;
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
	double l = std::sqrt (dot (a, a));
	return l > 1e-20 ? mul (a, 1 / l) : P{0, 0, 1};
}
bool fail (std::string *e, const char *s)
{
	if (e)
		*e = s;
	return false;
}
P closest (P p, P a, P b, P c)
{
	P	   ab = sub (b, a), ac = sub (c, a), ap = sub (p, a);
	double d1 = dot (ab, ap), d2 = dot (ac, ap);
	if (d1 <= 0 && d2 <= 0)
		return a;
	P	   bp = sub (p, b);
	double d3 = dot (ab, bp), d4 = dot (ac, bp);
	if (d3 >= 0 && d4 <= d3)
		return b;
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0)
		return add (a, mul (ab, d1 / (d1 - d3)));
	P	   cp = sub (p, c);
	double d5 = dot (ab, cp), d6 = dot (ac, cp);
	if (d6 >= 0 && d5 <= d6)
		return c;
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0)
		return add (a, mul (ac, d2 / (d2 - d6)));
	double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
		return add (b, mul (sub (c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
	double inv = 1 / (va + vb + vc);
	return add (a, add (mul (ab, vb * inv), mul (ac, vc * inv)));
}
bool segmentTriangle (P p, P q, P a, P b, P c)
{
	P	   dir = sub (q, p), e1 = sub (b, a), e2 = sub (c, a), h = cross (dir, e2);
	double det = dot (e1, h);
	if (std::abs (det) < 1e-12 * std::sqrt (dot (dir, dir) * dot (e1, e1) * dot (e2, e2)))
		return false;
	double inv = 1 / det;
	P	   s = sub (p, a);
	double u = dot (s, h) * inv;
	if (u < -1e-8 || u > 1 + 1e-8)
		return false;
	P	   r = cross (s, e1);
	double v = dot (dir, r) * inv;
	if (v < -1e-8 || u + v > 1 + 1e-8)
		return false;
	double t = dot (e2, r) * inv;
	return t > 1e-8 && t < 1 - 1e-8;
}
bool trianglesIntersect (const std::array<P, 3> &a, const std::array<P, 3> &b)
{
	for (int i = 0; i < 3; ++i)
		if (segmentTriangle (a[i], a[(i + 1) % 3], b[0], b[1], b[2]) || segmentTriangle (b[i], b[(i + 1) % 3], a[0], a[1], a[2]))
			return true;
	P na = unit (cross (sub (a[1], a[0]), sub (a[2], a[0]))), nb = unit (cross (sub (b[1], b[0]), sub (b[2], b[0])));
	if (std::abs (dot (na, nb)) < 1 - 1e-8 || std::abs (dot (na, sub (b[0], a[0]))) > 1e-7)
		return false;
	// Coplanar triangles: strict interior overlap, allowing shared boundaries.
	int drop = 0;
	for (int i = 1; i < 3; ++i)
		if (std::abs (na[i]) > std::abs (na[drop]))
			drop = i;
	int	 x = (drop + 1) % 3, y = (drop + 2) % 3;
	auto orient = [&] (P p, P q, P r) { return (q[x] - p[x]) * (r[y] - p[y]) - (q[y] - p[y]) * (r[x] - p[x]); };
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			if (orient (a[i], a[(i + 1) % 3], b[j]) * orient (a[i], a[(i + 1) % 3], b[(j + 1) % 3]) < -1e-10 &&
				orient (b[j], b[(j + 1) % 3], a[i]) * orient (b[j], b[(j + 1) % 3], a[(i + 1) % 3]) < -1e-10)
				return true;
	for (int s = 0; s < 2; ++s)
	{
		const auto &t = s ? a : b;
		const auto &u = s ? b : a;
		for (P p : u)
		{
			double o0 = orient (t[0], t[1], p), o1 = orient (t[1], t[2], p), o2 = orient (t[2], t[0], p);
			if ((o0 > 1e-8 && o1 > 1e-8 && o2 > 1e-8) || (o0 < -1e-8 && o1 < -1e-8 && o2 < -1e-8))
				return true;
		}
	}
	return false;
}
P rotate (P p, P a)
{
	constexpr double rad = 3.14159265358979323846 / 180;
	// roll X, pitch Y, yaw Z.
	double			 c = std::cos (a[2] * rad), s = std::sin (a[2] * rad);
	p = {p[0], c * p[1] - s * p[2], s * p[1] + c * p[2]};
	c = std::cos (a[0] * rad);
	s = std::sin (a[0] * rad);
	p = {c * p[0] + s * p[2], p[1], -s * p[0] + c * p[2]};
	c = std::cos (a[1] * rad);
	s = std::sin (a[1] * rad);
	return {c * p[0] - s * p[1], s * p[0] + c * p[1], p[2]};
}
void orientOutward (SolidDefinition &s)
{
	P center{};
	for (P p : s.vertices)
		center = add (center, p);
	center = mul (center, 1.0 / s.vertices.size ());
	for (auto &f : s.faces)
	{
		P a = s.vertices[f.vertices[0]], b = s.vertices[f.vertices[1]], c = s.vertices[f.vertices[2]];
		if (dot (cross (sub (b, a), sub (c, a)), sub (a, center)) < 0)
			std::swap (f.vertices[1], f.vertices[2]);
	}
}
void put (std::vector<uint8_t> &b, uint64_t x, int n)
{
	for (int i = 0; i < n; ++i)
		b.push_back (uint8_t (x >> (8 * i)));
}
} // namespace
bool ValidateSolid (const SolidDefinition &s, std::string *error)
{
	if (s.vertices.size () < 4 || s.vertices.size () > 8192 || s.faces.size () < 4 || s.faces.size () > 16384)
		return fail (error, "Solid requires 4..8192 vertices and 4..16384 faces");
	if (s.operation != SolidOperation::Add && s.operation != SolidOperation::Subtract)
		return fail (error, "Invalid solid operation");
	for (float f : s.origin.local)
		if (!std::isfinite (f) || f < 0 || f >= ChunkSize)
			return fail (error, "Solid origin must be normalized");
	P			lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
	std::set<P> unique;
	for (P p : s.vertices)
	{
		for (int i = 0; i < 3; ++i)
		{
			if (!std::isfinite (p[i]) || std::abs (p[i]) > 1048576)
				return fail (error, "Solid vertices must be finite and within 1048576 units of the anchor");
			lo[i] = std::min (lo[i], p[i]);
			hi[i] = std::max (hi[i], p[i]);
		}
		if (!unique.insert (p).second)
			return fail (error, "Duplicate vertex positions: weld the OBJ before import");
	}
	for (int i = 0; i < 3; ++i)
		if (hi[i] - lo[i] < 8 - 0.002)
			return fail (error, "Solid extent is below the eight-unit feature resolution");
	struct Edge
	{
		int		 count = 0, balance = 0;
		uint32_t face = 0;
	};
	std::map<std::pair<uint32_t, uint32_t>, Edge> edges;
	std::vector<bool>							  used (s.vertices.size ());
	std::vector<size_t>							  links (s.faces.size () * 3);
	for (size_t i = 0; i < links.size (); ++i)
		links[i] = i;
	auto root = [&] (size_t i)
	{
		while (links[i] != i)
		{
			links[i] = links[links[i]];
			i = links[i];
		}
		return i;
	};
	std::vector<P> normals;
	normals.reserve (s.faces.size ());
	std::vector<std::array<P, 3>> tris;
	tris.reserve (s.faces.size ());
	double							  volume = 0;
	std::set<std::array<uint32_t, 3>> faces;
	for (size_t fi = 0; fi < s.faces.size (); ++fi)
	{
		const auto &f = s.faces[fi];
		for (uint32_t v : f.vertices)
			if (v >= s.vertices.size ())
				return fail (error, "Face index out of range");
		auto key = f.vertices;
		std::sort (key.begin (), key.end ());
		if (!faces.insert (key).second)
			return fail (error, "Duplicate solid face");
		std::array<P, 3> t{s.vertices[f.vertices[0]], s.vertices[f.vertices[1]], s.vertices[f.vertices[2]]};
		P				 n = cross (sub (t[1], t[0]), sub (t[2], t[0]));
		if (dot (n, n) < 1e-10)
			return fail (error, "Degenerate solid triangle");
		normals.push_back (unit (n));
		tris.push_back (t);
		volume += dot (sub (t[0], lo), cross (sub (t[1], lo), sub (t[2], lo))) / 6;
		for (int j = 0; j < 3; ++j)
		{
			uint32_t a = f.vertices[j], b = f.vertices[(j + 1) % 3];
			used[a] = true;
			auto &e = edges[{std::min (a, b), std::max (a, b)}];
			if (e.count && dot (normals[e.face], normals.back ()) < .99999 &&
				dot (sub (s.vertices[a], s.vertices[b]), sub (s.vertices[a], s.vertices[b])) < (8 - 0.002) * (8 - 0.002))
				return fail (error, "Hard edge shorter than eight units; simplify or scale the import");
			if (e.count == 1)
			{
				for (uint32_t vertex : {a, b})
				{
					size_t old = 0, current = 0;
					while (s.faces[e.face].vertices[old] != vertex)
						++old;
					while (f.vertices[current] != vertex)
						++current;
					links[root (fi * 3 + current)] = root (size_t (e.face) * 3 + old);
				}
			}
			++e.count;
			e.balance += a < b ? 1 : -1;
			e.face = uint32_t (fi);
		}
	}
	for (const auto &[key, e] : edges)
		if (e.count != 2 || e.balance != 0)
			return fail (error, "Solid must be a closed, consistently oriented two-manifold (every edge twice, opposite directions)");
	std::vector<size_t> vertexRoot (s.vertices.size (), SIZE_MAX);
	for (size_t f = 0; f < s.faces.size (); ++f)
		for (int c = 0; c < 3; ++c)
		{
			auto v = s.faces[f].vertices[c];
			auto component = root (f * 3 + c);
			if (vertexRoot[v] != SIZE_MAX && vertexRoot[v] != component)
				return fail (error, "Nonmanifold pinched solid vertex");
			vertexRoot[v] = component;
		}
	if (std::find (used.begin (), used.end (), false) != used.end ())
		return fail (error, "Unused solid vertices");
	if (volume <= 1e-6)
		return fail (error, "Solid has inward orientation or zero volume");
	// Reject crossing nonadjacent surfaces. Bounding boxes avoid most pair work.
	for (size_t a = 0; a < tris.size (); ++a)
		for (size_t b = a + 1; b < tris.size (); ++b)
		{
			int shared = 0;
			for (uint32_t av : s.faces[a].vertices)
				for (uint32_t bv : s.faces[b].vertices)
					shared += av == bv;
			if (shared >= 2)
			{
				if (dot (normals[a], normals[b]) < -1 + 1e-8)
					return fail (error, "Overlapping coplanar faces at shared edge");
				continue;
			}
			if (dot (normals[a], normals[b]) < -.99999)
			{
				P ca = mul (add (add (tris[a][0], tris[a][1]), tris[a][2]), 1.0 / 3), cb = mul (add (add (tris[b][0], tris[b][1]), tris[b][2]), 1.0 / 3);
				if (segmentTriangle (ca, sub (ca, mul (normals[a], 8 - .002)), tris[b][0], tris[b][1], tris[b][2]) ||
					segmentTriangle (cb, sub (cb, mul (normals[b], 8 - .002)), tris[a][0], tris[a][1], tris[a][2]))
					return fail (error, "Wall thickness below eight units between opposing faces; thicken or scale the import");
			}
			bool overlap = true;
			for (int k = 0; k < 3; ++k)
			{
				double alo = std::min ({tris[a][0][k], tris[a][1][k], tris[a][2][k]}), ahi = std::max ({tris[a][0][k], tris[a][1][k], tris[a][2][k]});
				double blo = std::min ({tris[b][0][k], tris[b][1][k], tris[b][2][k]}), bhi = std::max ({tris[b][0][k], tris[b][1][k], tris[b][2][k]});
				if (ahi < blo - 1e-8 || bhi < alo - 1e-8)
				{
					overlap = false;
					break;
				}
			}
			if (overlap && trianglesIntersect (tris[a], tris[b]))
				return fail (error, "Self-intersecting solid surface");
		}
	return true;
}
bool MakeSolidBox (const WorldPosition &origin, const P &size, const P &angles, uint16_t material, SolidDefinition &out, std::string *error)
{
	for (int i = 0; i < 3; ++i)
		if (!std::isfinite (size[i]) || size[i] < 8 || !std::isfinite (angles[i]))
			return fail (error, "Box sizes must be at least eight units and angles finite");
	SolidDefinition s;
	s.origin = origin;
	for (int i = 0; i < 8; ++i)
		s.vertices.push_back (rotate ({((i & 1) ? .5 : -.5) * size[0], ((i & 2) ? .5 : -.5) * size[1], ((i & 4) ? .5 : -.5) * size[2]}, angles));
	for (int axis = 0; axis < 3; ++axis)
		for (int side = 0; side < 2; ++side)
		{
			int base = side << axis, u = 1 << ((axis + 1) % 3), v = 1 << ((axis + 2) % 3);
			s.faces.push_back ({{uint32_t (base), uint32_t (base | u), uint32_t (base | u | v)}, material});
			s.faces.push_back ({{uint32_t (base), uint32_t (base | u | v), uint32_t (base | v)}, material});
		}
	orientOutward (s);
	if (!ValidateSolid (s, error))
		return false;
	out = std::move (s);
	return true;
}
bool MakeSolidWedge (const WorldPosition &origin, const P &size, const P &angles, uint16_t material, SolidDefinition &out, std::string *error)
{
	for (int i = 0; i < 3; ++i)
		if (!std::isfinite (size[i]) || size[i] < 8 || !std::isfinite (angles[i]))
			return fail (error, "Wedge sizes must be at least eight units and angles finite");
	SolidDefinition s;
	s.origin = origin;
	for (P p : std::array<P, 6>{{{-.5, -.5, -.5}, {.5, -.5, -.5}, {.5, -.5, .5}, {-.5, .5, -.5}, {.5, .5, -.5}, {.5, .5, .5}}})
	{
		for (int i = 0; i < 3; ++i)
			p[i] *= size[i];
		s.vertices.push_back (rotate (p, angles));
	}
	for (auto f : std::array<std::array<uint32_t, 3>, 8>{{{0, 1, 2}, {3, 5, 4}, {0, 3, 4}, {0, 4, 1}, {1, 4, 5}, {1, 5, 2}, {2, 5, 3}, {2, 3, 0}}})
		s.faces.push_back ({f, material});
	orientOutward (s);
	if (!ValidateSolid (s, error))
		return false;
	out = std::move (s);
	return true;
}
bool MakeSolidConvex (
	const WorldPosition &origin, const std::vector<P> &vertices, const std::vector<SolidFace> &faces, SolidDefinition &out, std::string *error)
{
	SolidDefinition s;
	s.origin = origin;
	s.vertices = vertices;
	s.faces = faces;
	if (!ValidateSolid (s, error))
		return false;
	for (const auto &f : faces)
	{
		P a = vertices[f.vertices[0]], n = unit (cross (sub (vertices[f.vertices[1]], a), sub (vertices[f.vertices[2]], a)));
		for (P p : vertices)
			if (dot (n, sub (p, a)) > 1e-6)
				return fail (error, "Brush boundary is not convex");
	}
	out = std::move (s);
	return true;
}
SolidSample SampleSolid (const SolidDefinition &s, const P &p)
{
	SolidSample result;
	double		best = 1e300, angle = 0;
	for (const auto &f : s.faces)
	{
		P	   a = s.vertices[f.vertices[0]], b = s.vertices[f.vertices[1]], c = s.vertices[f.vertices[2]], d = sub (p, closest (p, a, b, c));
		double ds = dot (d, d);
		if (ds < best)
		{
			best = ds;
			result.normal = unit (cross (sub (b, a), sub (c, a)));
			result.material = f.material;
		}
		a = sub (a, p);
		b = sub (b, p);
		c = sub (c, p);
		double la = std::sqrt (dot (a, a)), lb = std::sqrt (dot (b, b)), lc = std::sqrt (dot (c, c));
		angle += 2 * std::atan2 (dot (a, cross (b, c)), la * lb * lc + dot (a, b) * lc + dot (b, c) * la + dot (c, a) * lb);
	}
	result.density = std::sqrt (best) * (std::abs (angle) > 6.283185307179586 ? 1 : -1);
	return result;
}
PreparedSolid::PreparedSolid (const SolidDefinition &definition) : solid (std::make_shared<const SolidDefinition> (definition))
{
	for (const auto &face : definition.faces)
	{
		P a = definition.vertices[face.vertices[0]],
		  n = unit (cross (sub (definition.vertices[face.vertices[1]], a), sub (definition.vertices[face.vertices[2]], a)));
		const double d = dot (n, a);
		for (P p : definition.vertices)
			if (dot (n, p) - d > 1e-6)
			{
				planes.clear ();
				materials.clear ();
				BuildAcceleration ();
				return;
			}
		planes.push_back ({n[0], n[1], n[2], d});
		materials.push_back (face.material);
	}
}
SolidSample PreparedSolid::Sample (const SolidPoint &point) const
{
	if (planes.empty ())
		return SampleAcceleration (point);
	SolidSample result;
	result.density = 1e30;
	for (size_t i = 0; i < planes.size (); ++i)
	{
		const auto	&plane = planes[i];
		P			 n{plane[0], plane[1], plane[2]};
		const double d = plane[3] - dot (n, point);
		if (d < result.density)
		{
			result.density = d;
			result.normal = n;
			result.material = materials[i];
		}
	}
	return result;
}
void PreparedSolid::BuildAcceleration ()
{
	triangles.resize (solid->faces.size ());
	for (size_t i = 0; i < triangles.size (); ++i)
		triangles[i] = uint32_t (i);
	nodes.reserve (triangles.size () * 2);
	auto build = [&] (auto &&self, uint32_t first, uint32_t count) -> uint32_t
	{
		const uint32_t index = uint32_t (nodes.size ());
		nodes.push_back ({});
		Node node;
		node.minimum = {1e30, 1e30, 1e30};
		node.maximum = {-1e30, -1e30, -1e30};
		node.first = first;
		node.count = count;
		for (uint32_t i = first; i < first + count; ++i)
			for (uint32_t v : solid->faces[triangles[i]].vertices)
				for (int k = 0; k < 3; ++k)
				{
					node.minimum[k] = std::min (node.minimum[k], solid->vertices[v][k]);
					node.maximum[k] = std::max (node.maximum[k], solid->vertices[v][k]);
				}
		if (count > 8)
		{
			int axis = 0;
			for (int k = 1; k < 3; ++k)
				if (node.maximum[k] - node.minimum[k] > node.maximum[axis] - node.minimum[axis])
					axis = k;
			auto centroid = [&] (uint32_t f)
			{
				double c = 0;
				for (uint32_t v : solid->faces[f].vertices)
					c += solid->vertices[v][axis];
				return c;
			};
			std::sort (
				triangles.begin () + first, triangles.begin () + first + count,
				[&] (uint32_t a, uint32_t b)
				{
					double ca = centroid (a), cb = centroid (b);
					return ca < cb || (ca == cb && a < b);
				});
			node.count = 0;
			node.left = self (self, first, count / 2);
			node.right = self (self, first + count / 2, count - count / 2);
		}
		nodes[index] = node;
		return index;
	};
	if (!triangles.empty ())
		build (build, 0, uint32_t (triangles.size ()));
}
SolidSample PreparedSolid::SampleAcceleration (const SolidPoint &point) const
{
	if (nodes.empty ())
		return SampleSolid (*solid, point);
	auto boundsDistance = [&] (const Node &node)
	{
		double d = 0;
		for (int k = 0; k < 3; ++k)
		{
			double v = std::max ({node.minimum[k] - point[k], 0.0, point[k] - node.maximum[k]});
			d += v * v;
		}
		return d;
	};
	SolidSample				 result;
	double					 best = 1e300;
	uint32_t				 bestFace = UINT32_MAX;
	std::array<uint32_t, 64> stack{};
	size_t					 top = 1;
	while (top)
	{
		const auto &node = nodes[stack[--top]];
		if (boundsDistance (node) > best)
			continue;
		if (!node.count)
		{
			uint32_t a = node.left, b = node.right;
			if (boundsDistance (nodes[a]) < boundsDistance (nodes[b]))
				std::swap (a, b);
			stack[top++] = a;
			stack[top++] = b;
			continue;
		}
		for (uint32_t i = node.first; i < node.first + node.count; ++i)
		{
			const auto	f = triangles[i];
			const auto &face = solid->faces[f];
			P			a = solid->vertices[face.vertices[0]], b = solid->vertices[face.vertices[1]], c = solid->vertices[face.vertices[2]],
			  delta = sub (point, closest (point, a, b, c));
			double d = dot (delta, delta);
			if (d < best || (d == best && f < bestFace))
			{
				best = d;
				bestFace = f;
				result.normal = unit (cross (sub (b, a), sub (c, a)));
				result.material = face.material;
			}
		}
	}
	if (best < 1e-20)
	{
		result.density = 0;
		return result;
	}
	// Reject edge/vertex/coplanar intersections instead of guessing crossings.
	// Three deterministic skew rays avoid common aligned mesh degeneracies;
	// Sum oriented crossings to match nonzero winding, including nested shells.
	// If all are ambiguous, the original solid-angle winding is authoritative.
	for (P direction :
		 std::array<P, 3>{{{1, .3713906763541037, .6947465906068658}, {.4196433776070806, 1, .237829127986273}, {.731178986278309, .431928374601913, 1}}})
	{
		bool	ambiguous = false;
		int32_t intersections = 0;
		top = 1;
		stack[0] = 0;
		auto rayBounds = [&] (const Node &node)
		{
			double enter = 0, leave = 1e300;
			for (int k = 0; k < 3; ++k)
			{
				double a = (node.minimum[k] - point[k]) / direction[k], b = (node.maximum[k] - point[k]) / direction[k];
				if (a > b)
					std::swap (a, b);
				enter = std::max (enter, a);
				leave = std::min (leave, b);
			}
			return leave >= enter - 1e-9;
		};
		while (top && !ambiguous)
		{
			const auto &node = nodes[stack[--top]];
			if (!rayBounds (node))
				continue;
			if (!node.count)
			{
				stack[top++] = node.left;
				stack[top++] = node.right;
				continue;
			}
			for (uint32_t i = node.first; i < node.first + node.count; ++i)
			{
				const auto &f = solid->faces[triangles[i]];
				P a = solid->vertices[f.vertices[0]], b = solid->vertices[f.vertices[1]], c = solid->vertices[f.vertices[2]], e1 = sub (b, a), e2 = sub (c, a),
				  h = cross (direction, e2), s = sub (point, a);
				double det = dot (e1, h), scale = std::sqrt (dot (e1, e1) * dot (e2, e2));
				if (std::abs (det) < scale * 1e-12)
				{
					if (std::abs (dot (unit (cross (e1, e2)), s)) < 1e-8)
						ambiguous = true;
					continue;
				}
				double u = dot (s, h) / det;
				P	   q = cross (s, e1);
				double v = dot (direction, q) / det, t = dot (e2, q) / det;
				if (t <= 1e-9 || u < -1e-9 || v < -1e-9 || u + v > 1 + 1e-9)
					continue;
				if (u < 1e-8 || v < 1e-8 || 1 - u - v < 1e-8)
				{
					ambiguous = true;
					break;
				}
				intersections += det < 0 ? 1 : -1;
			}
		}
		if (!ambiguous)
		{
			result.density = std::sqrt (best) * (intersections != 0 ? 1 : -1);
			return result;
		}
	}
	result.density = std::copysign (std::sqrt (best), SampleSolid (*solid, point).density);
	return result;
}
bool ImportSolidOBJ (std::string_view text, const WorldPosition &origin, SolidDefinition &out, std::string *error, std::vector<std::string> *materials)
{
	if (text.size () > 8 * 1024 * 1024)
		return fail (error, "OBJ exceeds eight MiB import limit");
	SolidDefinition s;
	s.origin = origin;
	std::istringstream		 input{std::string (text)};
	std::string				 line, material;
	std::vector<std::string> faceMaterials;
	std::set<std::string>	 names;
	while (std::getline (input, line))
	{
		std::istringstream in (line);
		std::string		   type;
		in >> type;
		if (type.empty () || type[0] == '#')
			continue;
		if (type == "v")
		{
			P			p;
			std::string extra;
			if (!(in >> p[0] >> p[1] >> p[2]))
				return fail (error, "Malformed OBJ vertex");
			if (in >> extra && extra[0] != '#')
				return fail (error, "Homogeneous/colored OBJ vertices are unsupported");
			s.vertices.push_back (p);
			if (s.vertices.size () > 8192)
				return fail (error, "OBJ vertex limit exceeded");
		}
		else if (type == "usemtl")
		{
			if (!(in >> material))
				return fail (error, "Missing OBJ material name");
			names.insert (material);
			if (names.size () > 65534)
				return fail (error, "Too many materials");
		}
		else if (type == "f")
		{
			std::vector<uint32_t> ids;
			std::string			  token;
			while (in >> token)
			{
				if (token[0] == '#')
					break;
				std::string index = token.substr (0, token.find ('/'));
				size_t		consumed = 0;
				long		v;
				try
				{
					v = std::stol (index, &consumed);
				}
				catch (...)
				{
					return fail (error, "Malformed OBJ face index");
				}
				if (consumed != index.size () || v == 0)
					return fail (error, "Malformed OBJ face index");
				if (v < 0)
					v = long (s.vertices.size ()) + v + 1;
				if (v <= 0 || size_t (v) > s.vertices.size ())
					return fail (error, "OBJ face index out of range");
				ids.push_back (uint32_t (v - 1));
				if (ids.size () > 256)
					return fail (error, "OBJ polygon exceeds 256 vertices");
			}
			if (ids.size () < 3)
				return fail (error, "OBJ face requires three vertices");
			P a = s.vertices[ids[0]], n = unit (cross (sub (s.vertices[ids[1]], a), sub (s.vertices[ids[2]], a)));
			for (size_t j = 0; j < ids.size (); ++j)
			{
				P p = s.vertices[ids[j]], q = s.vertices[ids[(j + 1) % ids.size ()]], r = s.vertices[ids[(j + 2) % ids.size ()]];
				if (std::abs (dot (n, sub (p, a))) > 1e-5 || dot (n, cross (sub (q, p), sub (r, q))) <= 1e-9)
					return fail (error, "OBJ polygons must be planar and strictly convex; triangulate the import");
			}
			for (size_t j = 1; j + 1 < ids.size (); ++j)
			{
				s.faces.push_back ({{ids[0], ids[j], ids[j + 1]}, 3});
				faceMaterials.push_back (material);
			}
			if (s.faces.size () > 16384)
				return fail (error, "OBJ face limit exceeded");
		}
		else if (type != "vn" && type != "vt" && type != "o" && type != "g" && type != "s" && type != "mtllib")
			return fail (error, "Unsupported OBJ record (only polygonal solids supported)");
	}
	std::vector<std::string> sorted (names.begin (), names.end ());
	for (size_t i = 0; i < s.faces.size (); ++i)
		if (!faceMaterials[i].empty ())
		{
			std::string name = faceMaterials[i];
			for (char &c : name)
				if (c >= 'A' && c <= 'Z')
					c = char (c - 'A' + 'a');
			if (name == "grass")
				s.faces[i].material = 1;
			else if (name == "soil" || name == "dirt")
				s.faces[i].material = 2;
			else if (name == "rock" || name == "stone")
				s.faces[i].material = 3;
			else if (name == "sand")
				s.faces[i].material = 4;
			else
			{
				auto index = std::lower_bound (sorted.begin (), sorted.end (), faceMaterials[i]) - sorted.begin ();
				if (index > 65527)
					return fail (error, "Too many OBJ material identifiers");
				s.faces[i].material = uint16_t (index + 8); // Stable name order outside reserved natural IDs.
			}
		}
	if (!ValidateSolid (s, error))
		return false;
	out = std::move (s);
	if (materials)
		*materials = std::move (sorted);
	return true;
}
std::vector<uint8_t> EncodeSolid (const SolidDefinition &s)
{
	std::string error;
	if (!ValidateSolid (s, &error))
		throw std::invalid_argument (error);
	std::vector<uint8_t>	b{'Q', 'S', 'L', 'D', 1, uint8_t (s.operation)};
	std::array<int64_t, 3>	chunks{s.origin.chunk.x, s.origin.chunk.y, s.origin.chunk.z};
	std::array<uint32_t, 3> local{};
	for (int i = 0; i < 3; ++i)
	{
		local[i] = uint32_t (std::llround (double (s.origin.local[i]) * 1024));
		if (local[i] == 1048576)
		{
			if (chunks[i] == INT64_MAX)
				throw std::invalid_argument ("Solid origin quantization overflows chunk range");
			++chunks[i];
			local[i] = 0;
		}
	}
	// The wire geometry, rather than only the source doubles, must remain a
	// valid manifold. Otherwise a committed asset could fail its own reload.
	SolidDefinition canonical = s;
	canonical.origin.chunk = {chunks[0], chunks[1], chunks[2]};
	for (int i = 0; i < 3; ++i)
		canonical.origin.local[i] = float (local[i]) / 1024;
	for (auto &point : canonical.vertices)
		for (double &value : point)
			value = double (std::llround (value * 1024)) / 1024;
	if (!ValidateSolid (canonical, &error))
		throw std::invalid_argument ("Solid cannot be represented at 1/1024-unit precision: " + error);
	for (int64_t c : chunks)
		put (b, uint64_t (c), 8);
	for (uint32_t f : local)
		put (b, f, 4);
	put (b, s.vertices.size (), 4);
	put (b, s.faces.size (), 4);
	for (P p : canonical.vertices)
		for (double d : p)
			put (b, uint64_t (std::llround (d * 1024)), 8);
	for (const auto &f : s.faces)
	{
		for (uint32_t i : f.vertices)
			put (b, i, 4);
		put (b, f.material, 2);
	}
	return b;
}
bool DecodeSolid (const std::vector<uint8_t> &b, SolidDefinition &out, std::string *error)
{
	if (b.size () < 50 || std::memcmp (b.data (), "QSLD\1", 5) != 0)
		return fail (error, "Unsupported solid object version");
	size_t at = 5;
	auto   get = [&] (int n)
	{
		uint64_t v = 0;
		for (int i = 0; i < n; ++i)
			v |= uint64_t (b[at++]) << (8 * i);
		return v;
	};
	SolidDefinition s;
	s.operation = SolidOperation (get (1));
	s.origin.chunk.x = int64_t (get (8));
	s.origin.chunk.y = int64_t (get (8));
	s.origin.chunk.z = int64_t (get (8));
	for (float &v : s.origin.local)
		v = float (get (4)) / 1024;
	uint64_t nv = get (4), nf = get (4);
	if (nv > 8192 || nf > 16384 || b.size () != 50 + nv * 24 + nf * 14)
		return fail (error, "Invalid solid object length");
	for (uint64_t i = 0; i < nv; ++i)
	{
		P p;
		for (double &v : p)
			v = double (int64_t (get (8))) / 1024;
		s.vertices.push_back (p);
	}
	for (uint64_t i = 0; i < nf; ++i)
	{
		SolidFace f;
		for (uint32_t &v : f.vertices)
			v = uint32_t (get (4));
		f.material = uint16_t (get (2));
		s.faces.push_back (f);
	}
	if (!ValidateSolid (s, error))
		return false;
	out = std::move (s);
	return true;
}
void AppendSolidSurface (const SolidDefinition &s, const P &offset, const P &minimum, const P &maximum, ChunkMesh &mesh)
{
	for (const auto &f : s.faces)
	{
		std::vector<P> poly;
		for (uint32_t i : f.vertices)
			poly.push_back (add (s.vertices[i], offset));
		P normal = unit (cross (sub (poly[1], poly[0]), sub (poly[2], poly[0])));
		for (int axis = 0; axis < 3; ++axis)
			for (int side = 0; side < 2; ++side)
			{
				std::vector<P> clipped;
				double		   bound = side ? maximum[axis] : minimum[axis];
				for (size_t i = 0; i < poly.size (); ++i)
				{
					P	   a = poly[i], b = poly[(i + 1) % poly.size ()];
					double da = (a[axis] - bound) * (side ? -1 : 1), db = (b[axis] - bound) * (side ? -1 : 1);
					if (da >= 0)
						clipped.push_back (a);
					if ((da >= 0) != (db >= 0))
						clipped.push_back (add (a, mul (sub (b, a), da / (da - db))));
				}
				poly = std::move (clipped);
			}
		for (size_t i = 1; i + 1 < poly.size (); ++i)
		{
			uint32_t start = uint32_t (mesh.vertices.size ());
			for (P p : {poly[0], poly[i + 1], poly[i]})
			{
				MeshVertex v;
				for (int k = 0; k < 3; ++k)
				{
					v.position[k] = float (p[k]);
					v.normal[k] = float (normal[k]);
				}
				v.blend = TerrainClassifyMaterial (f.material, false);
				mesh.vertices.push_back (v);
			}
			mesh.indices.insert (mesh.indices.end (), {start, start + 1, start + 2});
		}
	}
	mesh.smooth = true;
}
} // namespace quiver
