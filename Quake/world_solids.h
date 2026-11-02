/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_WORLD_SOLIDS_H
#define QUIVER_WORLD_SOLIDS_H
#include "octree_world.h"
#include <string_view>
namespace quiver
{
using SolidPoint = std::array<double, 3>;
enum class SolidOperation : uint8_t
{
	Add,
	Subtract
};
struct SolidFace
{
	std::array<uint32_t, 3> vertices{};
	uint16_t				material = 3;
};
struct SolidDefinition
{
	WorldPosition			origin;
	SolidOperation			operation = SolidOperation::Add;
	std::vector<SolidPoint> vertices;
	std::vector<SolidFace>	faces;
};
struct SolidSample
{
	double	   density = -1e30; // Positive inside, negative outside.
	SolidPoint normal{0, 0, 1}; // Outward normal of the nearest actual face.
	uint16_t   material = 3;
};
// Angles are degrees, pitch around Y, yaw around Z, roll around X (Rz Ry Rx).
// Definitions use local doubles anchored at origin, quantized to 1/1024 unit on wire.
bool MakeSolidBox (
	const WorldPosition &origin, const SolidPoint &size, const SolidPoint &angles, uint16_t material, SolidDefinition &out, std::string *error = nullptr);
bool MakeSolidWedge (
	const WorldPosition &origin, const SolidPoint &size, const SolidPoint &angles, uint16_t material, SolidDefinition &out, std::string *error = nullptr);
// Explicit outward triangular boundary, validated as a convex watertight manifold.
bool MakeSolidConvex (
	const WorldPosition &origin, const std::vector<SolidPoint> &vertices, const std::vector<SolidFace> &faces, SolidDefinition &out,
	std::string *error = nullptr);
// Supports v/f/usemtl, negative indices and convex planar polygons. Material names
// grass/soil/dirt/rock/stone/sand select natural material IDs; other names use
// deterministic sorted IDs starting at 8. The optional output returns sorted names.
bool ImportSolidOBJ (
	std::string_view text, const WorldPosition &origin, SolidDefinition &out, std::string *error = nullptr, std::vector<std::string> *materials = nullptr);
bool		ValidateSolid (const SolidDefinition &solid, std::string *error = nullptr);
SolidSample SampleSolid (const SolidDefinition &solid, const SolidPoint &localPoint);
// Immutable prepared evaluator: convex brushes use half-space sign/face gradients;
// concave imports use a triangle BVH with exact winding fallback for degenerate rays.
class PreparedSolid
{
  public:
	explicit PreparedSolid (const SolidDefinition &solid);
	SolidSample Sample (const SolidPoint &point) const;

  private:
	std::shared_ptr<const SolidDefinition> solid;
	std::vector<std::array<double, 4>>	   planes;
	std::vector<uint16_t>				   materials;
	struct Node
	{
		SolidPoint minimum{}, maximum{};
		uint32_t   first = 0, count = 0, left = 0, right = 0;
	};
	std::vector<Node>	  nodes;
	std::vector<uint32_t> triangles;
	void				  BuildAcceleration ();
	SolidSample			  SampleAcceleration (const SolidPoint &point) const;
};
std::vector<uint8_t> EncodeSolid (const SolidDefinition &solid);
bool				 DecodeSolid (const std::vector<uint8_t> &bytes, SolidDefinition &out, std::string *error = nullptr);
// Exact hard-edged triangles clipped to a local-space box. Clockwise exterior
// winding matches ChunkMesh. Rendering and collision consume the same mesh.
void AppendSolidSurface (const SolidDefinition &solid, const SolidPoint &offset, const SolidPoint &minimum, const SolidPoint &maximum, ChunkMesh &mesh);
} // namespace quiver
#endif
