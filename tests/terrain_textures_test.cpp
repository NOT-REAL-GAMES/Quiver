#include "terrain_textures.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace quiver;

namespace
{
void Check (bool value, const char *message)
{
	if (!value)
		throw std::runtime_error (message);
}

std::array<double, 3> Validate (TerrainTextureKind kind)
{
	const auto pixels = BuildTerrainTexture (kind);
	Check (pixels.size () == size_t (TerrainTextureSize * TerrainTextureSize * 4), "complete RGBA tile");
	Check (pixels == BuildTerrainTexture (kind), "deterministic texture across reloads");
	std::array<double, 3> means{};
	double				  sum = 0, squared = 0, adjacent = 0, seamX = 0, seamY = 0;
	const auto			  value = [&] (int x, int y) { return pixels[(y * TerrainTextureSize + x) * 4]; };
	for (int y = 0; y < TerrainTextureSize; ++y)
		for (int x = 0; x < TerrainTextureSize; ++x)
		{
			const size_t offset = size_t (y * TerrainTextureSize + x) * 4;
			Check (pixels[offset + 3] == 255, "opaque albedo does not require alpha handling");
			for (int channel = 0; channel < 3; ++channel)
				means[channel] += pixels[offset + channel];
			const double sample = value (x, y);
			sum += sample;
			squared += sample * sample;
			if (x + 1 < TerrainTextureSize)
				adjacent += std::abs (sample - value (x + 1, y));
			if (y + 1 < TerrainTextureSize)
				adjacent += std::abs (sample - value (x, y + 1));
		}
	for (int axis = 0; axis < TerrainTextureSize; ++axis)
	{
		seamX += std::abs (int (value (0, axis)) - int (value (TerrainTextureSize - 1, axis)));
		seamY += std::abs (int (value (axis, 0)) - int (value (axis, TerrainTextureSize - 1)));
	}
	constexpr double count = TerrainTextureSize * TerrainTextureSize;
	for (double &mean : means)
		mean /= count;
	const double variance = squared / count - (sum / count) * (sum / count);
	Check (variance > 12 && variance < 1200, "visible detail without extreme albedo contrast");
	adjacent /= 2 * TerrainTextureSize * (TerrainTextureSize - 1);
	Check (adjacent > 2, "texture contains fine detail");
	// The opposite edges are adjacent texels, not duplicate texels. Their
	// gradient must resemble the interior instead of forming a visible stripe.
	Check (seamX / TerrainTextureSize < adjacent * 1.8 + 2, "horizontal repeat has no image-border seam");
	Check (seamY / TerrainTextureSize < adjacent * 1.8 + 2, "vertical repeat has no image-border seam");
	return means;
}
} // namespace

int main ()
{
	try
	{
		const auto grass = Validate (TerrainTextureKind::Grass);
		const auto soil = Validate (TerrainTextureKind::Soil);
		const auto rock = Validate (TerrainTextureKind::Rock);
		const auto sand = Validate (TerrainTextureKind::Sand);
		Check (grass[1] > grass[0] && grass[0] > grass[2], "grass retains a muted olive palette");
		Check (soil[0] > soil[1] && soil[1] > soil[2], "soil has an earth palette");
		Check (std::abs (rock[0] - rock[1]) < 12 && rock[1] - rock[2] < 18, "rock remains near-neutral");
		Check (sand[0] > soil[0] + 30 && sand[1] > soil[1] + 30, "sand remains distinguishable from soil");
		std::cout << "Terrain texture tests passed\n";
		return 0;
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what () << '\n';
		return 1;
	}
}
