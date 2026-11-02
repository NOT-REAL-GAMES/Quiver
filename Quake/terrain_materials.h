/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_MATERIALS_H
#define QUIVER_TERRAIN_MATERIALS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace quiver
{
// Four independently sampled textures: natural cover, soil, rock and sand.
// Natural cover becomes grass/soil/rock in the fragment shader using continuous
// world-space patches and slope. Tint is premultiplied so interpolating painted
// colors against natural materials never introduces dark or colored fringes.
struct TerrainMaterialBlend
{
	std::array<uint8_t, 4> weights{};
	std::array<uint8_t, 4> tint{};
	bool				   operator== (const TerrainMaterialBlend &) const = default;
};

constexpr TerrainMaterialBlend TerrainClassifyMaterial (uint16_t material, bool proceduralTerrain)
{
	TerrainMaterialBlend result;
	switch (material % 8)
	{
	case 1:
		result.weights[0] = 255;
		break;
	case 2:
		// Generated interior and surface IDs describe occupancy, not a grass
		// stratum. Treat them identically to eliminate horizontal green bands.
		result.weights[proceduralTerrain ? 0 : 1] = 255;
		break;
	case 4:
		result.weights[3] = 255;
		break;
	case 5:
		result.weights[2] = 255;
		result.tint = {63, 113, 165, 255};
		break;
	case 6:
		result.weights[0] = 255;
		result.tint = {78, 139, 78, 255};
		break;
	case 7:
		result.weights[2] = 255;
		result.tint = {221, 92, 38, 255};
		break;
	default:
		result.weights[2] = 255;
		break;
	}
	return result;
}

inline TerrainMaterialBlend TerrainInterpolateMaterials (const TerrainMaterialBlend &a, const TerrainMaterialBlend &b, double fraction)
{
	fraction = std::clamp (fraction, 0.0, 1.0);
	TerrainMaterialBlend result;
	for (std::size_t i = 0; i < 4; ++i)
	{
		result.weights[i] = uint8_t (a.weights[i] + fraction * (double (b.weights[i]) - a.weights[i]) + 0.5);
		result.tint[i] = uint8_t (a.tint[i] + fraction * (double (b.tint[i]) - a.tint[i]) + 0.5);
	}
	return result;
}

// Reduce the canonical integer coordinate before float conversion. An 8192-unit
// material period spans eight chunks and stays stable beyond double precision
// and at both int64 limits. The shader adds this phase to mesh-local positions.
constexpr float TerrainMaterialChunkPhase (int64_t chunk)
{
	return float ((uint64_t (chunk) & 7) * 1024);
}
} // namespace quiver
#endif
