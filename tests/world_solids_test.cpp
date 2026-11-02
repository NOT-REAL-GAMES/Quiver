/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world_solids.h"
#include "terrain_features.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <sstream>
using namespace quiver;
static void require (bool value, const std::string &message)
{
	if (!value)
		throw std::runtime_error (message);
}
static void CheckSmoothTerrainQEF ()
{
	for (int field = 0; field < 3; ++field)
	{
		const double amplitude = field == 1 ? 5 : 10;
		const double periodX = field == 1 ? 64 : 32, periodY = field == 1 ? 71 : 37;
		auto		 height = [&] (double x, double y) { return field == 0 ? .04 * x + .03 * y : amplitude * std::sin (x / periodX) * std::cos (y / periodY); };
		auto		 sample = [&] (const SolidPoint &p)
		{
			const double dx = field == 0 ? .04 : amplitude / periodX * std::cos (p[0] / periodX) * std::cos (p[1] / periodY);
			const double dy = field == 0 ? .03 : -amplitude / periodY * std::sin (p[0] / periodX) * std::sin (p[1] / periodY);
			const double inverse = 1 / std::sqrt (1 + dx * dx + dy * dy);
			return FeatureSample{height (p[0], p[1]) - p[2], {-dx * inverse, -dy * inverse, inverse}, 1, false};
		};
		ChunkMesh mesh;
		BuildFeatureSurface ({4, 4, -28}, {128, 128, 16}, 8, sample, mesh);
		require (!mesh.indices.empty (), "Smooth analytic terrain produces geometry");
		// These are smooth, gentle heightfields. The old unconstrained solve
		// plus per-axis clamp missed the strong field by almost four units and
		// produced vertical/downward faces despite analytic upward normals.
		const double maximumError = field == 0 ? .0001 : field == 1 ? .03 : .15;
		for (const auto &vertex : mesh.vertices)
			require (
				std::abs (height (vertex.position[0], vertex.position[1]) - vertex.position[2]) < maximumError,
				"Smooth QEF vertices stay close to the analytic surface");
		for (size_t i = 0; i < mesh.indices.size (); i += 3)
		{
			const auto &a = mesh.vertices[mesh.indices[i]].position;
			const auto &b = mesh.vertices[mesh.indices[i + 2]].position;
			const auto &c = mesh.vertices[mesh.indices[i + 1]].position;
			SolidPoint	u{}, v{}, normal{};
			for (int axis = 0; axis < 3; ++axis)
			{
				u[axis] = double (b[axis]) - a[axis];
				v[axis] = double (c[axis]) - a[axis];
			}
			for (int axis = 0; axis < 3; ++axis)
				normal[axis] = u[(axis + 1) % 3] * v[(axis + 2) % 3] - u[(axis + 2) % 3] * v[(axis + 1) % 3];
			const double area = std::hypot (normal[0], normal[1], normal[2]);
			require (area > 1e-12, "Smooth QEF triangles retain positive area");
			require (normal[2] / area > (field == 2 ? .90 : .98), "Gentle analytic terrain has no artificial vertical risers or folded faces");
		}
	}
}
static void CheckWedgeDiagonals ()
{
	for (SolidPoint angles : std::array<SolidPoint, 4>{{{0, -20, 0}, {0, 25, 0}, {13, 31, 7}, {0, 0, 0}}})
		for (bool canonical : {false, true})
		{
			SolidDefinition wedge;
			std::string		error;
			require (MakeSolidWedge ({}, {192, 160, 128}, angles, 3, wedge, &error), error);
			if (canonical)
				require (DecodeSolid (EncodeSolid (wedge), wedge, &error), error);
			PreparedSolid prepared (wedge);
			ChunkMesh	  mesh;
			BuildFeatureSurface (
				{324, 324, 324}, {48, 48, 48}, 8,
				[&] (SolidPoint p)
				{
					for (double &v : p)
						v -= 512;
					auto sample = prepared.Sample (p);
					return FeatureSample{sample.density, sample.normal, sample.material, true};
				},
				mesh);
			std::map<std::array<float, 3>, int> positions;
			std::vector<int>					ids;
			for (const auto &vertex : mesh.vertices)
				ids.push_back (positions.emplace (vertex.position, int (positions.size ())).first->second);
			std::map<std::pair<int, int>, std::pair<int, int>> edges;
			for (size_t i = 0; i < mesh.indices.size (); i += 3)
			{
				int a = ids[mesh.indices[i]], b = ids[mesh.indices[i + 1]], c = ids[mesh.indices[i + 2]];
				require (a != b && b != c && c != a, "Wedge triangulation preserves distinct vertices");
				for (auto [u, v] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}})
				{
					auto &edge = edges[{std::min (u, v), std::max (u, v)}];
					++edge.first;
					edge.second += u < v ? 1 : -1;
				}
				SolidPoint ab{}, ac{}, center{};
				for (int k = 0; k < 3; ++k)
				{
					double pa = mesh.vertices[mesh.indices[i]].position[k];
					double pb = mesh.vertices[mesh.indices[i + 1]].position[k];
					double pc = mesh.vertices[mesh.indices[i + 2]].position[k];
					ab[k] = pb - pa;
					ac[k] = pc - pa;
					center[k] = (pa + pb + pc) / 3 - 512;
				}
				// Output is clockwise: reverse the cross product for the outward normal.
				SolidPoint n{ac[1] * ab[2] - ac[2] * ab[1], ac[2] * ab[0] - ac[0] * ab[2], ac[0] * ab[1] - ac[1] * ab[0]};
				double	   area = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				require (area > 0, "Wedge triangulation has no degenerate triangles");
				// The yaw-only regression previously had inward triangles of double
				// area 8.26 units squared. Tolerate only float-scale slivers now.
				if (angles[0] == 0 && angles[2] == 0 && area > .001)
				{
					auto sample = prepared.Sample (center);
					require (
						n[0] * sample.normal[0] + n[1] * sample.normal[1] + n[2] * sample.normal[2] >= -.01 * area,
						"Sharp wedge diagonal does not fold inward");
				}
			}
			for (const auto &[edge, counts] : edges)
				require (counts.first == 2 && counts.second == 0, "Wedge edges remain closed with consistent orientation");
		}
}
int main ()
{
	CheckSmoothTerrainQEF ();
	CheckWedgeDiagonals ();
	std::string		error;
	SolidDefinition box;
	require (MakeSolidBox ({}, {64, 48, 32}, {0, 0, 0}, 7, box, &error), error);
	require (SampleSolid (box, {0, 0, 0}).density > 15.99, "Box inside distance");
	require (std::abs (SampleSolid (box, {40, 0, 0}).density + 8) < 1e-8, "Box outside distance");
	require (SampleSolid (box, {40, 0, 0}).normal[0] > .999, "Sharp outward normal");
	auto			bytes = EncodeSolid (box);
	SolidDefinition decoded;
	require (DecodeSolid (bytes, decoded, &error), error);
	require (bytes == EncodeSolid (decoded), "Canonical roundtrip");
	bytes.push_back (0);
	require (!DecodeSolid (bytes, decoded, &error), "Trailing bytes rejected");
	for (size_t cut = 0; cut < 50; ++cut)
		require (!DecodeSolid (std::vector<uint8_t> (bytes.begin (), bytes.begin () + cut), decoded), "Truncation rejected");
	auto boundary = box;
	boundary.origin.local[0] = std::nextafter (1024.0f, 0.0f);
	require (DecodeSolid (EncodeSolid (boundary), decoded, &error), error);
	require (decoded.origin.chunk.x == 1 && decoded.origin.local[0] == 0, "Quantized anchor carries across chunk boundary");
	// Two distinct, coplanar interior vertices may quantize to one vertex even
	// though all architectural crease edges exceed the minimum feature size.
	auto fine = box;
	auto face = fine.faces.front ();
	fine.faces.erase (fine.faces.begin ());
	SolidPoint p{}, q{};
	for (int axis = 0; axis < 3; ++axis)
	{
		p[axis] = (box.vertices[face.vertices[0]][axis] + box.vertices[face.vertices[1]][axis] + box.vertices[face.vertices[2]][axis]) / 3;
		const double midpoint = (box.vertices[face.vertices[0]][axis] + box.vertices[face.vertices[1]][axis]) * .5;
		q[axis] = p[axis] + (midpoint - p[axis]) * 1e-6;
	}
	const uint32_t pi = uint32_t (fine.vertices.size ());
	fine.vertices.push_back (p);
	const uint32_t qi = uint32_t (fine.vertices.size ());
	fine.vertices.push_back (q);
	for (auto triangle : std::array<std::array<uint32_t, 3>, 5>{
			 {{face.vertices[0], face.vertices[1], qi},
			  {face.vertices[1], pi, qi},
			  {pi, face.vertices[0], qi},
			  {face.vertices[1], face.vertices[2], pi},
			  {face.vertices[2], face.vertices[0], pi}}})
		fine.faces.push_back ({triangle, face.material});
	require (ValidateSolid (fine, &error), error);
	bool precisionRejected = false;
	try
	{
		EncodeSolid (fine);
	}
	catch (const std::invalid_argument &e)
	{
		precisionRejected = std::string (e.what ()).find ("1/1024") != std::string::npos;
	}
	require (precisionRejected, "Canonical quantization collapse rejected before persistence");
	SolidDefinition rotated;
	require (MakeSolidBox ({}, {64, 48, 32}, {13, 31, 7}, 3, rotated, &error), error);
	require (DecodeSolid (EncodeSolid (rotated), decoded, &error), error);
	require (SampleSolid (rotated, {0, 0, 0}).density > 15.99, "Rotated box interior");
	PreparedSolid prepared (rotated);
	for (int z = -48; z <= 48; z += 8)
		for (int y = -48; y <= 48; y += 8)
			for (int x = -48; x <= 48; x += 8)
				require (
					(prepared.Sample ({double (x), double (y), double (z)}).density >= 0) ==
						(SampleSolid (rotated, {double (x), double (y), double (z)}).density >= 0),
					"Prepared convex sign agrees with triangle winding");
	SolidDefinition hollow, inner;
	require (MakeSolidBox ({}, {64, 64, 64}, {}, 3, hollow, &error), error);
	require (MakeSolidBox ({}, {32, 32, 32}, {}, 3, inner, &error), error);
	for (auto p : inner.vertices)
		hollow.vertices.push_back (p);
	for (auto f : inner.faces)
	{
		for (auto &v : f.vertices)
			v += 8;
		std::swap (f.vertices[1], f.vertices[2]);
		hollow.faces.push_back (f);
	}
	require (ValidateSolid (hollow, &error), error);
	PreparedSolid hollowPrepared (hollow);
	for (int z = -40; z <= 40; z += 3)
		for (int y = -40; y <= 40; y += 3)
			for (int x = -40; x <= 40; x += 3)
			{
				SolidPoint p{double (x) + .137, double (y) + .193, double (z) + .321};
				auto	   exact = SampleSolid (hollow, p), fast = hollowPrepared.Sample (p);
				require (std::abs (exact.density - fast.density) < 1e-8, "BVH signed distance agrees with exact winding for hollow import");
			}
	for (SolidPoint p : std::array<SolidPoint, 4>{{{0, 0, 0}, {32, 32, 32}, {16, 16, 16}, {31, 0, 0}}})
		require (std::abs (SampleSolid (hollow, p).density - hollowPrepared.Sample (p).density) < 1e-8, "BVH edge and vertex ray cases");
	auto thin = hollow;
	for (size_t i = 8; i < thin.vertices.size (); ++i)
		for (double &v : thin.vertices[i])
			v *= 1.75;
	require (!ValidateSolid (thin, &error), "Thin opposing wall rejected despite large bounds and edges");
	auto nested = hollow;
	for (size_t i = 12; i < nested.faces.size (); ++i)
		std::swap (nested.faces[i].vertices[1], nested.faces[i].vertices[2]);
	PreparedSolid nestedPrepared (nested);
	require (
		nestedPrepared.Sample ({0, 0, 0}).density > 0 && SampleSolid (nested, {0, 0, 0}).density > 0, "Oriented BVH crossings match nonzero nested winding");
	SolidDefinition tiltedOuter, tiltedInner;
	require (MakeSolidBox ({}, {64, 64, 64}, {13, 31, 7}, 3, tiltedOuter, &error), error);
	require (MakeSolidBox ({}, {56, 56, 56}, {13, 31, 7}, 3, tiltedInner, &error), error);
	for (auto p : tiltedInner.vertices)
		tiltedOuter.vertices.push_back (p);
	for (auto f : tiltedInner.faces)
	{
		for (auto &v : f.vertices)
			v += 8;
		std::swap (f.vertices[1], f.vertices[2]);
		tiltedOuter.faces.push_back (f);
	}
	require (!ValidateSolid (tiltedOuter, &error), "Rotated thin shell rejected despite large bounds and edges");
	SolidDefinition wedge;
	require (MakeSolidWedge ({}, {64, 48, 32}, {0, 19, 0}, 3, wedge, &error), error);
	require (MakeSolidConvex ({}, box.vertices, box.faces, decoded, &error), error);
	auto malformed = box;
	malformed.faces.pop_back ();
	require (!ValidateSolid (malformed, &error), "Open mesh rejected");
	malformed = box;
	std::swap (malformed.faces[0].vertices[0], malformed.faces[0].vertices[1]);
	require (!ValidateSolid (malformed, &error), "Bad orientation rejected");
	auto pinched = box;
	for (size_t i = 1; i < box.vertices.size (); ++i)
	{
		auto p = box.vertices[i];
		p[0] += 64;
		p[1] += 48;
		p[2] += 32;
		pinched.vertices.push_back (p);
	}
	for (auto f : box.faces)
	{
		for (auto &v : f.vertices)
			v = v ? uint32_t (v + 7) : 7;
		pinched.faces.push_back (f);
	}
	require (!ValidateSolid (pinched, &error), "Pinched manifold vertex rejected");
	auto crossed = box;
	for (auto p : box.vertices)
	{
		p[0] += 16;
		p[1] += 16;
		p[2] += 16;
		crossed.vertices.push_back (p);
	}
	for (auto f : box.faces)
	{
		for (auto &v : f.vertices)
			v += 8;
		crossed.faces.push_back (f);
	}
	require (!ValidateSolid (crossed, &error), "Crossing closed components rejected");
	require (!MakeSolidBox ({}, {64, 48, 4}, {}, 3, decoded, &error), "Tiny extent rejected");
	std::ostringstream obj;
	for (auto p : box.vertices)
		obj << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
	obj << "usemtl stone\n";
	for (auto f : box.faces)
		obj << "f " << f.vertices[0] + 1 << ' ' << f.vertices[1] + 1 << ' ' << f.vertices[2] + 1 << '\n';
	std::vector<std::string> materials;
	require (ImportSolidOBJ (obj.str (), {}, decoded, &error, &materials), error);
	require (materials == std::vector<std::string>{"stone"}, "OBJ material names");
	for (const auto &face : decoded.faces)
		require (face.material == 3, "Named stone selects rock texture");
	require (!ImportSolidOBJ ("v nan 0 0\nf 1 2 3", {}, decoded, &error), "Malformed OBJ rejected");
	ChunkMesh mesh;
	AppendSolidSurface (box, {}, {-128, -128, -128}, {128, 128, 128}, mesh);
	require (mesh.indices.size () == 36, "Exact box triangles");
	auto hit = SweepFeatureMesh (mesh, {100, 0, 0}, {-200, 0, 0});
	require (hit.hit && std::abs (hit.fraction - .34) < 1e-8 && hit.normal[0] > .999, "Ray box hit");
	hit = SweepFeatureMesh (mesh, {100, 0, 0}, {-200, 0, 0}, {-16, -16, -24}, {16, 16, 32});
	require (hit.hit && std::abs (hit.fraction - .26) < 1e-8, "Swept player AABB");
	hit = SweepFeatureMesh (mesh, {100, 100, 0}, {-200, 0, 0});
	require (!hit.hit, "Miss outside box");
	ChunkMesh clipped;
	AppendSolidSurface (box, {}, {0, -128, -128}, {128, 128, 128}, clipped);
	for (auto v : clipped.vertices)
		require (v.position[0] >= 0, "Chunk clipping");
	// Corner deliberately off-grid: QEF must retain the three intersecting planes.
	SolidDefinition small;
	require (MakeSolidBox ({}, {33, 35, 37}, {}, 3, small, &error), error);
	auto sample = [&] (SolidPoint p)
	{
		auto s = SampleSolid (small, p);
		return FeatureSample{s.density, s.normal, s.material, true};
	};
	ChunkMesh feature;
	BuildFeatureSurface ({-32, -32, -32}, {8, 8, 8}, 8, sample, feature);
	require (!feature.vertices.empty (), "Feature output");
	bool corner = false;
	for (auto v : feature.vertices)
		if (std::abs (v.position[0] - 16.5) < .001 && std::abs (v.position[1] - 17.5) < .001 && std::abs (v.position[2] - 18.5) < .001)
			corner = true;
	require (corner, "Dual contouring retains exact corner");
	hit = SweepFeatureMesh (feature, {50, 0, 0}, {-100, 0, 0});
	require (hit.hit && std::abs (hit.fraction - .335) < 1e-4, "Feature render/collision agreement");
	// Neighbor extraction must share boundary vertices exactly.
	auto	  plane = [] (SolidPoint p) { return FeatureSample{3 - p[2], {0, 0, 1}, 3, false}; };
	ChunkMesh left, right;
	BuildFeatureSurface ({0, 0, 0}, {4, 4, 4}, 8, plane, left);
	BuildFeatureSurface ({32, 0, 0}, {4, 4, 4}, 8, plane, right);
	for (auto v : left.vertices)
		require (std::abs (v.position[2] - 3) < .001 && v.normal[2] > .999, "Planar QEF");
	require (left.indices.size () == right.indices.size (), "Neighbor planar topology");
	std::cout << "world solids and feature geometry tests passed\n";
}
