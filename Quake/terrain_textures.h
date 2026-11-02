/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_TERRAIN_TEXTURES_H
#define QUIVER_TERRAIN_TEXTURES_H

#include <cstdint>
#include <vector>

namespace quiver
{
inline constexpr int TerrainTextureSize = 256;
enum class TerrainTextureKind
{
	Grass,
	Soil,
	Rock,
	Sand
};
using TerrainTexturePixels = std::vector<uint8_t>;

// Deterministic, periodic RGBA8 albedo tiles. Retain the returned pixels while
// the texture manager may reload their source; alpha is always opaque.
TerrainTexturePixels BuildTerrainTexture (TerrainTextureKind kind);
} // namespace quiver
#endif
