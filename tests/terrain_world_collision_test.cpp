#include "octree_world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace quiver;

namespace
{
using Vec = std::array<double, 3>;
constexpr double ContactEpsilon = 0.03125;

void Check (bool value, const std::string &message)
{
	if (!value)
		throw std::runtime_error (message);
}

Vec Subtract (const Vec &a, const Vec &b)
{
	return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec Cross (const Vec &a, const Vec &b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double Dot (const Vec &a, const Vec &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec Vertex (const ChunkMesh &mesh, size_t index)
{
	const auto &position = mesh.vertices[mesh.indices[index]].position;
	return {position[0], position[1], position[2]};
}

// Independent two-sided Moller-Trumbore oracle against the actual published
// float vertex positions. No density sampler or collision helper is used.
double MeshRay (const ChunkMesh &mesh, Vec origin, Vec direction)
{
	double closest = std::numeric_limits<double>::infinity ();
	for (size_t i = 0; i < mesh.indices.size (); i += 3)
	{
		const Vec	 a = Vertex (mesh, i), ab = Subtract (Vertex (mesh, i + 1), a), ac = Subtract (Vertex (mesh, i + 2), a);
		const Vec	 p = Cross (direction, ac);
		const double determinant = Dot (ab, p);
		if (std::abs (determinant) < 1e-12)
			continue;
		const Vec	 fromA = Subtract (origin, a);
		const double u = Dot (fromA, p) / determinant;
		const Vec	 q = Cross (fromA, ab);
		const double v = Dot (direction, q) / determinant;
		if (u < -1e-8 || v < -1e-8 || u + v > 1 + 1e-8)
			continue;
		const double distance = Dot (ac, q) / determinant;
		if (distance >= 0)
			closest = std::min (closest, distance);
	}
	return closest;
}

// Clip every surface triangle to the support rectangle and maximize its Z.
// The maximum of a linear triangle occurs at a clipped polygon vertex, so
// this accounts for footprint edges and interiors rather than probing corners.
double MeshSupport (const ChunkMesh &mesh, double x, double y, double radius)
{
	double height = -std::numeric_limits<double>::infinity ();
	for (size_t triangle = 0; triangle < mesh.indices.size (); triangle += 3)
	{
		std::vector<Vec> polygon{Vertex (mesh, triangle), Vertex (mesh, triangle + 1), Vertex (mesh, triangle + 2)};
		for (int plane = 0; plane < 4 && !polygon.empty (); ++plane)
		{
			const int		 axis = plane / 2;
			const double	 boundary = (axis ? y : x) + (plane & 1 ? radius : -radius);
			auto			 inside = [&] (const Vec &point) { return plane & 1 ? point[axis] <= boundary : point[axis] >= boundary; };
			std::vector<Vec> clipped;
			Vec				 previous = polygon.back ();
			bool			 wasInside = inside (previous);
			for (const auto &current : polygon)
			{
				const bool isInside = inside (current);
				if (wasInside != isInside)
				{
					const double fraction = (boundary - previous[axis]) / (current[axis] - previous[axis]);
					Vec			 crossing;
					for (int a = 0; a < 3; ++a)
						crossing[a] = previous[a] + fraction * (current[a] - previous[a]);
					clipped.push_back (crossing);
				}
				if (isInside)
					clipped.push_back (current);
				previous = current;
				wasInside = isInside;
			}
			polygon = std::move (clipped);
		}
		for (const auto &point : polygon)
			height = std::max (height, point[2]);
	}
	return height;
}

double ContactTime (const TraceResult &trace, const Vec &offset)
{
	double incoming = 0;
	for (int axis = 0; axis < 3; ++axis)
		incoming -= offset[axis] * trace.normal[axis];
	Check (!trace.startSolid && !trace.hitUnknown && trace.fraction < 1 && incoming > 0, "trace has a loaded geometric contact");
	return trace.fraction + ContactEpsilon / incoming;
}

std::shared_ptr<const ChunkMesh> Mesh (World &world, ChunkKey key)
{
	Check (world.Require (key), "load terrain test chunk");
	auto mesh = world.Mesh (key, true);
	Check (mesh && mesh->smooth && !mesh->indices.empty (), "publish terrain oracle mesh");
	return mesh;
}

void CheckRay (World &world, const ChunkMesh &mesh, const Vec &origin, const Vec &offset)
{
	const double expected = MeshRay (mesh, origin, offset);
	Check (expected >= 0 && expected < 1, "oracle ray intersects a published triangle");
	WorldPosition start{mesh.key, {float (origin[0]), float (origin[1]), float (origin[2])}};
	const auto	  trace = world.Trace (start, offset);
	const double  actual = ContactTime (trace, offset);
	const double  length = std::sqrt (Dot (offset, offset));
	Check (std::abs (expected - actual) * length < 0.001, "collision contact matches mesh to one thousandth of a unit");
}

void MeshAgreementTests ()
{
	WorldConfig config;
	config.seed = 4242;
	World		 world (config);
	std::mt19937 random (4431);
	for (const ChunkKey key : {ChunkKey{2, 3, 0}, ChunkKey{-2, -3, 0}, ChunkKey{INT64_MAX - 1, INT64_MIN + 1, 0}})
	{
		const auto mesh = Mesh (world, key);
		for (int sample = 0; sample < 30; ++sample)
		{
			const float x = 40 + float (random () % 940000) / 1000, y = 40 + float (random () % 940000) / 1000;
			CheckRay (world, *mesh, {x, y, 256}, {0, 0, -240});
			if (sample < 10)
				CheckRay (world, *mesh, {x, y, 256}, {sample & 1 ? 40.0 : -40.0, sample & 2 ? 30.0 : -30.0, -240});
			const double height = 256 - 240 * MeshRay (*mesh, {x, y, 256}, {0, 0, -240});
			Check (world.CollisionContents ({key, {x, y, float (height - 0.1)}}).IsSolid (), "continuous contents is solid below rendered surface");
			Check (world.CollisionContents ({key, {x, y, float (height + 0.1)}}).contents == Empty, "continuous contents is empty above rendered surface");
		}
		for (int sample = 0; sample < 8; ++sample)
		{
			const float	 x = 40 + float (random () % 940000) / 1000, y = 40 + float (random () % 940000) / 1000;
			const double support = MeshSupport (*mesh, x, y, 16);
			const auto	 trace = world.Trace ({key, {x, y, 256}}, {0, 0, -200}, {-16, -16, -24}, {16, 16, 32});
			const double contactZ = 256 - 200 * ContactTime (trace, {0, 0, -200}) - 24;
			Check (std::abs (support - contactZ) < 0.001, "player box rests on the highest rendered surface under its full footprint");
			Check (trace.normal[2] > 0.7, "procedural terrain contact is walkable");
		}
	}
	const ChunkKey west{2, 3, 0}, east{3, 3, 0};
	Check (world.Require (east), "load neighboring seam chunk");
	const auto westMesh = Mesh (world, west), eastMesh = Mesh (world, east);
	for (float y : {83.25f, 309.75f, 790.125f})
	{
		CheckRay (world, *westMesh, {1023.75, y, 256}, {0, 0, -240});
		CheckRay (world, *eastMesh, {0.25, y, 256}, {0, 0, -240});
		CheckRay (world, *eastMesh, {0, y, 256}, {0, 0, -240});
		const double support = std::max (MeshSupport (*westMesh, 1024, y, 16), MeshSupport (*eastMesh, 0, y, 16));
		const auto	 trace = world.Trace ({east, {0, y, 256}}, {0, 0, -200}, {-16, -16, -24}, {16, 16, 32});
		Check (std::abs (256 - 200 * ContactTime (trace, {0, 0, -200}) - 24 - support) < 0.001, "player footprint spans chunk seam without a ledge");
	}
}

void MovementAndModeTests ()
{
	World world;
	Check (world.Require ({}) && world.Require ({-1, 0, 0}), "load spawn plateau");
	const auto standing = world.Trace ({{}, {128, 128, 256}}, {0, 0, -200}, {-16, -16, -24}, {16, 16, 32});
	Check (
		!standing.startSolid && standing.normal == std::array<float, 3>{0, 0, 1} && std::abs (standing.end.local[2] - 88.03125f) < 0.0001,
		"standing player rests on plateau with normal collision epsilon");
	const auto slide = world.Trace ({{}, {128, 128, 88}}, {48, 32, 0}, {-16, -16, -24}, {16, 16, 32});
	Check (!slide.startSolid && slide.fraction == 1, "exact floor contact permits tangential motion");
	const auto jump = world.Trace (standing.end, {0, 0, 96}, {-16, -16, -24}, {16, 16, 32});
	Check (!jump.startSolid && jump.fraction == 1, "standing player can jump clear of terrain");
	const auto interior = world.Trace ({{}, {128, 128, 32}}, {32, 24, 0});
	Check (interior.startSolid && interior.allSolid, "internal tetrahedron and cube boundaries stay solid");
	const auto escape = world.Trace ({{}, {128, 128, 32}}, {0, 0, 64});
	Check (escape.startSolid && !escape.allSolid, "startsolid sweep can exit the union of solid tetrahedra");
	const auto seamInterior = world.Trace ({{}, {24, 128, 32}}, {-64, 0, 0});
	Check (seamInterior.startSolid && seamInterior.allSolid, "solid interval union remains closed across a chunk seam");
	Check (world.Require ({2, 3, 0}), "load sloping terrain without mesh jobs");
	const WorldPosition start{{2, 3, 0}, {432.25f, 719.75f, 256}};
	const auto			smooth = world.Trace (start, {0, 0, -240});
	Check (!world.Stats ().pendingMeshes && !world.Stats ().meshBytes, "collision requires no render meshes or workers");
	world.SetSmoothTerrain (false);
	const auto blocks = world.Trace (start, {0, 0, -240});
	Check (smooth.fraction == blocks.fraction && smooth.normal == blocks.normal, "renderer toggle never changes authoritative collision");
	const auto targeting = world.TraceVoxels (start, {0, 0, -240});
	Check (!targeting.startSolid && targeting.fraction < 1 && targeting.normal[2] == 1, "editing retains quantized voxel targeting");
	Check (std::abs (targeting.end.local[2] - smooth.end.local[2]) > 0.05, "sloped collision is distinct from voxel targeting");
}

void EditAndCavityTests ()
{
	World		   world;
	const ChunkKey sky{2, 3, 1}, underground{2, 3, -1};
	Check (world.Require (sky) && world.Require (underground), "load edit test regions");
	const WorldPosition isolated{sky, {516, 516, 516}}, cavity{underground, {516, 516, 516}};
	Check (world.Edit (isolated, {3, Solid}) && world.Edit (cavity, {}), "publish isolated voxel and excavated cavity");
	Check (
		world.CollisionContents (isolated).IsSolid () && world.CollisionContents (cavity).contents == Empty, "edits immediately change continuous occupancy");
	Check (!world.Stats ().pendingMeshes && !world.Stats ().meshBytes, "edited collision does not wait for meshing");
	const auto islandMesh = Mesh (world, sky), cavityMesh = Mesh (world, underground);
	for (int axis = 0; axis < 3; ++axis)
		for (double sign : {-1.0, 1.0})
		{
			Vec origin{516, 516, 516}, offset{};
			origin[axis] += 24 * sign;
			offset[axis] = -48 * sign;
			CheckRay (world, *islandMesh, origin, offset);
			origin = {516, 516, 516};
			offset[axis] = 24 * sign;
			CheckRay (world, *cavityMesh, origin, offset);
		}
	const auto solidInterior = world.Trace (isolated, {});
	Check (solidInterior.startSolid && solidInterior.allSolid, "isolated voxel interior is solid on tetrahedron junction");
	const auto cavityInterior = world.Trace (cavity, {});
	Check (!cavityInterior.startSolid && cavityInterior.fraction == 1, "excavated cavity interior is empty");
	Check (world.Edit (isolated, {}), "remove isolated voxel after mesh publication");
	Check (world.Trace ({sky, {492, 516, 516}}, {48, 0, 0}).fraction == 1, "removal changes collision before stale mesh refresh");
	for (int16_t liquid : {int16_t (Water), int16_t (Slime), int16_t (Lava)})
	{
		Check (world.Edit (isolated, {3, liquid}), "install liquid cell");
		Check (world.Trace ({sky, {492, 516, 516}}, {48, 0, 0}).fraction == 1, "liquid is nonblocking under smooth collision");
		Check (world.CollisionContents (isolated).contents == liquid, "continuous contents preserves liquid type");
	}
	const ChunkKey east{underground.x + 1, underground.y, underground.z};
	Check (world.Require (east), "load adjacent underground edit chunk");
	Check (world.Edit ({underground, {1020, 516, 516}}, {}) && world.Edit ({east, {4, 516, 516}}, {}), "carve adjoining cells across chunk boundary");
	const auto crossing = world.Trace ({underground, {1020, 516, 516}}, {8, 0, 0});
	Check (
		!crossing.startSolid && crossing.fraction == 1 && crossing.end.chunk == east,
		"neighbor edits immediately open a continuous passage across chunk boundary");
	const auto editedWestMesh = Mesh (world, underground), editedEastMesh = Mesh (world, east);
	CheckRay (world, *editedWestMesh, {1023.5, 516, 516}, {0, 24, 0});
	CheckRay (world, *editedEastMesh, {0.5, 516, 516}, {0, 24, 0});
}

void StreamingAndBoundedWorkTests ()
{
	World world;
	Check (world.Require ({}), "load known chunk beside unknown barrier");
	const auto barrier = world.Trace ({{}, {1000, 512, 512}}, {64, 0, 0});
	Check (
		!barrier.startSolid && barrier.hitUnknown && barrier.normal[0] == -1 && barrier.end.local[0] < 1024,
		"procedural density fallback never removes an unknown chunk barrier");
	const auto unknown = world.Trace ({{20, 20, 1}, {512, 512, 512}}, {});
	Check (unknown.startSolid && unknown.allSolid && unknown.hitUnknown, "unknown starting region remains blocked");
	for (int x = 0; x < 8; ++x)
		for (int y = 0; y < 8; ++y)
			Check (world.Require ({x, y, 1}), "load long diagonal sky corridor");
	const auto begin = std::chrono::steady_clock::now ();
	for (int i = 0; i < 8; ++i)
	{
		const auto diagonal = world.Trace ({{0, 0, 1}, {16, 16, 512}}, {8000, 8000, 0});
		Check (!diagonal.startSolid && !diagonal.hitUnknown && diagonal.fraction == 1, "long diagonal air ray prunes the empty volume");
	}
	const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - begin).count ();
	Check (seconds < 5, "eight long empty rays finish within a generous bounded-work limit");
	const auto excessive = world.Trace ({{0, 0, 1}, {512, 512, 512}}, {0, 0, 0}, {-100000, -100000, -100000}, {100000, 100000, 100000});
	Check (excessive.startSolid && excessive.allSolid && excessive.hitUnknown && excessive.fraction == 0, "excessive sweep volume fails closed");
	Check (!world.Stats ().pendingMeshes && !world.Stats ().meshBytes, "streaming collision never allocates render meshes");
}
} // namespace

int main ()
{
	try
	{
		MeshAgreementTests ();
		MovementAndModeTests ();
		EditAndCavityTests ();
		StreamingAndBoundedWorkTests ();
		std::cout << "terrain world collision tests passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << "terrain world collision test failed: " << error.what () << '\n';
		return 1;
	}
}
