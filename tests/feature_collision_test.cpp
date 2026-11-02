/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "octree_world.h"
#include "world_solids.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace quiver;
static void Check (bool pass, const char *message)
{
	if (!pass)
		throw std::runtime_error (message);
}
int main ()
{
	try
	{
		WorldConfig config;
		config.generator = Generator::Empty;
		World world (config);
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x)
					Check (world.Require ({x, y, z}), "load collision neighborhood");
		SolidDefinition box;
		std::string		error;
		Check (MakeSolidBox ({{0, 0, 0}, {128, 128, 128}}, {64, 64, 64}, {}, 3, box, &error), "make collision fixture");
		Check (world.AddSolid (box, &error), error.c_str ());
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x)
					Check (world.Require ({x, y, z}), "reload collision fixture");
		// Physics must work before any render mesh has been requested. Repeat
		// probes exercise the bounded local cache rather than full extraction.
		auto   coldStart = std::chrono::steady_clock::now ();
		auto   cold = world.Trace ({{0, 0, 0}, {128, 128, 200}}, {0, 0, -100}, {-16, -16, -16}, {16, 16, 16});
		double coldMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - coldStart).count ();
		Check (world.CollisionContents ({{0, 0, 0}, {128, 128, 128}}).IsSolid (), "feature contents in an otherwise empty world");
		Check (world.CollisionContents ({{1, 0, 0}, {-896, 128, 128}}).IsSolid (), "feature contents normalizes chunk-local coordinates");
		auto beforeWarm = world.Stats ();
		Check (
			beforeWarm.pendingMeshes == 0 && beforeWarm.meshBytes == 0 && beforeWarm.collisionPatches > 0,
			"collision is independent of render jobs and meshes");
		coldStart = std::chrono::steady_clock::now ();
		for (int i = 0; i < 100; ++i)
		{
			auto warm = world.Trace ({{0, 0, 0}, {128, 128, 200}}, {0, 0, -100}, {-16, -16, -16}, {16, 16, 16});
			Check (warm.fraction == cold.fraction && warm.normal == cold.normal, "cached collision matches cold extraction");
		}
		double warmMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - coldStart).count () / 100;
		Check (world.Stats ().collisionBuilds == beforeWarm.collisionBuilds, "stationary collision reuses patches");
		std::cout << "collision cold " << coldMs << " ms, warm " << warmMs << " ms\n";
		auto mesh = world.Mesh ({0, 0, 0}, true);
		Check (mesh && !mesh->vertices.empty (), "extract collision fixture");
		float roof = -1e9f;
		for (const auto &vertex : mesh->vertices)
			roof = std::max (roof, vertex.position[2]);
		// Box constructor uses centered local vertices: extent96..160.
		// Rest on the extracted roof, including edge-bisection precision.
		auto trace = world.Trace ({{0, 0, 0}, {128, 128, 128}}, {0, 0, 100});
		Check (trace.startSolid && !trace.allSolid && !trace.hitUnknown, "point can leave initially occupied solid");
		trace = world.Trace ({{0, 0, 0}, {128, 128, 128}}, {});
		Check (trace.startSolid && trace.allSolid, "stationary interior point is all solid");
		trace = world.Trace ({{0, 0, 0}, {128, 128, 170}}, {}, {-16, -16, -16}, {16, 16, 16});
		Check (trace.startSolid && trace.allSolid, "hull overlap detected even with origin outside");
		trace = world.Trace ({{0, 0, 0}, {128, 128, 170}}, {0, 0, 100}, {-16, -16, -16}, {16, 16, 16});
		Check (trace.startSolid && !trace.allSolid, "partially embedded hull can leave");
		trace = world.Trace ({{0, 0, 0}, {128, 128, roof + 16}}, {10, 0, 0}, {-16, -16, -16}, {16, 16, 16});
		Check (!trace.startSolid && trace.fraction == 1, "resting hull slides across roof");
		trace = world.Trace ({{0, 0, 0}, {128, 128, 200}}, {0, 0, -100}, {-16, -16, -16}, {16, 16, 16});
		Check (
			!trace.startSolid && trace.fraction > 0.20 && trace.fraction < 0.25 && trace.normal[2] > 0.99,
			"swept hull lands at rendered roof with outward normal");
		trace = world.Trace ({{0, 0, 0}, {170, 170, 128}}, {}, {-16, -16, -16}, {16, 16, 16});
		Check (trace.startSolid && trace.allSolid, "box corner overlap detected");
		trace = world.Trace ({{0, 0, 0}, {80, 128, 128}}, {96, 0, 0});
		Check (!trace.startSolid && trace.fraction > 0 && trace.fraction < 0.2 && trace.normal[0] < -0.99, "zero extent ray enters outward face");
		// A shifted sampling lattice can put a surface at +2 in triangles owned
		// by the negative neighbor, and vice versa. Exercise both orientations
		// at each of the three chunk boundary planes with short point sweeps.
		struct BoundaryCase
		{
			WorldPosition start;
			SolidPoint	  offset;
			int			  axis;
			int			  side;
		};
		std::vector<BoundaryCase> boundaries;
		for (int axis = 0; axis < 3; ++axis)
			for (int side : {-1, 1})
			{
				WorldPosition center{{0, 0, 0}, {float (384 + side * 64), float (384 + side * 64), float (384 + side * 64)}};
				center.local[axis] = float (-14 * side);
				Check (Normalize (center), "normalize boundary fixture");
				SolidDefinition boundary;
				Check (MakeSolidBox (center, {32, 32, 32}, {}, 3, boundary, &error) && world.AddSolid (boundary, &error), "create boundary fixture");
				WorldPosition start;
				SolidPoint	  from{}, offset{};
				from[axis] = 20 * side;
				offset[axis] = -8 * side;
				Check (Add (center, from, start), "boundary trace origin");
				boundaries.push_back ({start, offset, axis, side});
			}
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x)
					Check (world.Require ({x, y, z}), "reload boundary fixtures");
		for (const auto &boundary : boundaries)
		{
			trace = world.Trace (boundary.start, boundary.offset);
			Check (
				!trace.hitUnknown && !trace.startSolid && trace.fraction > 0.45 && trace.fraction < 0.55 && trace.normal[boundary.axis] * boundary.side > 0.99,
				"shifted lattice ownership works in all six boundary directions");
		}
		// Changing a cached patch must immediately update collision, including
		// its neighbors, without waiting for a replacement render mesh.
		Check (MakeSolidBox ({{0, 0, 0}, {128, 128, 128}}, {80, 80, 80}, {}, 3, box, &error), "make subtraction fixture");
		box.operation = SolidOperation::Subtract;
		Check (world.AddSolid (box, &error) && world.Require ({0, 0, 0}), "subtract cached collision fixture");
		Check (!world.CollisionContents ({{0, 0, 0}, {128, 128, 128}}).IsSolid (), "collision cache invalidates after subtraction");
		Check (world.Stats ().collisionBytes <= 32 * 1024 * 1024 && world.Stats ().collisionPatches <= 512, "collision cache stays bounded");
		std::cout << "feature collision integration passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what () << '\n';
		return 1;
	}
}
