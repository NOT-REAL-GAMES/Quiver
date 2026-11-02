/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terrain_textures.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace quiver
{
namespace
{
using Color = std::array<float, 3>;
using Canvas = std::vector<Color>;

uint32_t Hash (uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	return value ^ (value >> 16);
}

float Random (uint32_t value)
{
	return float (Hash (value) >> 8) * (1.0f / 16777216.0f);
}

int Wrap (int value, int period)
{
	const int wrapped = value % period;
	return wrapped < 0 ? wrapped + period : wrapped;
}

uint32_t CellHash (int x, int y, int period, uint32_t seed)
{
	return Hash (uint32_t (Wrap (x, period)) * 0x9e3779b9U ^ uint32_t (Wrap (y, period)) * 0x85ebca6bU ^ seed);
}

float Ease (float value)
{
	return value * value * (3.0f - 2.0f * value);
}

float Noise (float x, float y, int period, uint32_t seed)
{
	x *= float (period) / TerrainTextureSize;
	y *= float (period) / TerrainTextureSize;
	const int	ix = int (std::floor (x)), iy = int (std::floor (y));
	const float tx = Ease (x - ix), ty = Ease (y - iy);
	return std::lerp (
		std::lerp (Random (CellHash (ix, iy, period, seed)), Random (CellHash (ix + 1, iy, period, seed)), tx),
		std::lerp (Random (CellHash (ix, iy + 1, period, seed)), Random (CellHash (ix + 1, iy + 1, period, seed)), tx), ty);
}

Color Mix (Color a, Color b, float weight)
{
	for (int channel = 0; channel < 3; ++channel)
		a[channel] = std::lerp (a[channel], b[channel], weight);
	return a;
}

Color Shade (Color color, float amount)
{
	for (float &channel : color)
		channel += amount;
	return color;
}

void Paint (Canvas &canvas, int x, int y, Color color, float coverage)
{
	auto &pixel = canvas[Wrap (y, TerrainTextureSize) * TerrainTextureSize + Wrap (x, TerrainTextureSize)];
	pixel = Mix (pixel, color, std::clamp (coverage, 0.0f, 1.0f));
}

// Toroidal rasterization makes strands and litter crossing an image edge
// continue into the opposite edge, including the antialiased border.
void Strand (Canvas &canvas, float x, float y, float dx, float dy, float width, Color color, float opacity)
{
	const float length2 = dx * dx + dy * dy;
	const int	minX = int (std::floor (std::min (x, x + dx) - width - 1));
	const int	maxX = int (std::ceil (std::max (x, x + dx) + width + 1));
	const int	minY = int (std::floor (std::min (y, y + dy) - width - 1));
	const int	maxY = int (std::ceil (std::max (y, y + dy) + width + 1));
	for (int py = minY; py <= maxY; ++py)
		for (int px = minX; px <= maxX; ++px)
		{
			const float t = std::clamp (((px + 0.5f - x) * dx + (py + 0.5f - y) * dy) / length2, 0.0f, 1.0f);
			const float ex = px + 0.5f - x - t * dx, ey = py + 0.5f - y - t * dy;
			const float radius = width * (1.0f - 0.65f * t);
			Paint (canvas, px, py, Shade (color, 5.0f * t), opacity * std::clamp (radius + 0.55f - std::sqrt (ex * ex + ey * ey), 0.0f, 1.0f));
		}
}

void Pebble (Canvas &canvas, float x, float y, float radius, float aspect, Color color, float opacity)
{
	const int minX = int (std::floor (x - radius - 1)), maxX = int (std::ceil (x + radius + 1));
	const int minY = int (std::floor (y - radius - 1)), maxY = int (std::ceil (y + radius + 1));
	for (int py = minY; py <= maxY; ++py)
		for (int px = minX; px <= maxX; ++px)
		{
			const float dx = (px + 0.5f - x) / radius, dy = (py + 0.5f - y) / (radius * aspect);
			const float distance = std::sqrt (dx * dx + dy * dy);
			const float coverage = std::clamp ((1.0f - distance) * radius + 0.4f, 0.0f, 1.0f);
			Paint (canvas, px, py, Shade (color, -7.0f * (dx + dy) - 8.0f * distance), coverage * opacity);
		}
}

struct Fracture
{
	float edge;
	float shade;
};

Fracture RockFracture (float x, float y)
{
	// Warp a periodic cellular field so fissures curve and branch instead of
	// outlining an obvious regular grid of polygons.
	x += 16.0f * (Noise (x, y, 8, 981) - 0.5f);
	y += 16.0f * (Noise (x, y, 8, 719) - 0.5f);
	constexpr int period = 9;
	x *= float (period) / TerrainTextureSize;
	y *= float (period) / TerrainTextureSize;
	const int ix = int (std::floor (x)), iy = int (std::floor (y));
	float	  nearest = 100, second = 100, shade = 0;
	for (int cy = iy - 1; cy <= iy + 1; ++cy)
		for (int cx = ix - 1; cx <= ix + 1; ++cx)
		{
			const auto	hash = CellHash (cx, cy, period, 51821);
			const float dx = cx + 0.15f + 0.7f * Random (hash) - x;
			const float dy = cy + 0.15f + 0.7f * Random (hash ^ 548) - y;
			const float distance = dx * dx + dy * dy;
			if (distance < nearest)
			{
				second = nearest;
				nearest = distance;
				shade = Random (hash ^ 1099) - 0.5f;
			}
			else
				second = std::min (second, distance);
		}
	return {std::sqrt (second) - std::sqrt (nearest), shade};
}

void AddGrass (Canvas &canvas)
{
	constexpr float tau = 6.28318530718f;
	for (uint32_t blade = 0; blade < 7500; ++blade)
	{
		const uint32_t seed = blade * 31 + 8129;
		const float	   x = Random (seed) * TerrainTextureSize, y = Random (seed + 1) * TerrainTextureSize;
		const float	   fertility = Noise (x, y, 4, 616);
		if (Random (seed + 2) > 0.48f + fertility * 0.52f)
			continue;
		const float angle = tau * Random (seed + 3);
		const float length = 2.5f + 9.0f * Random (seed + 4);
		const float width = 0.35f + 0.35f * Random (seed + 5);
		const float dx = std::cos (angle) * length, dy = std::sin (angle) * length;
		const bool	dry = Random (seed + 6) < 0.24f;
		Color		color = dry ? Color{116, 106, 68} : Color{79, 99, 48};
		color = Shade (color, (Random (seed + 7) - 0.5f) * 35.0f);
		Strand (canvas, x + 0.7f, y + 0.7f, dx, dy, width + 0.3f, {43, 42, 25}, 0.28f);
		Strand (canvas, x, y, dx, dy, width, color, 0.85f);
	}
}

void AddGrains (Canvas &canvas, TerrainTextureKind kind)
{
	const bool	   soil = kind == TerrainTextureKind::Soil;
	const uint32_t count = soil ? 2700 : 4300;
	for (uint32_t grain = 0; grain < count; ++grain)
	{
		const uint32_t seed = grain * 29 + (soil ? 7837 : 23751);
		const float	   x = Random (seed) * TerrainTextureSize, y = Random (seed + 1) * TerrainTextureSize;
		float		   radius = soil ? 0.25f + 1.3f * std::pow (Random (seed + 2), 3.0f) : 0.2f + 0.42f * Random (seed + 2);
		if (soil && grain % 23 == 0)
			radius += 0.9f;
		Color color = soil ? Mix ({55, 47, 36}, {132, 125, 105}, Random (seed + 4)) : Mix ({112, 97, 67}, {179, 163, 125}, Random (seed + 4));
		Pebble (canvas, x, y, radius, 0.6f + 0.4f * Random (seed + 3), color, 0.78f);
	}
	if (soil)
		for (uint32_t twig = 0; twig < 180; ++twig)
		{
			const uint32_t seed = twig * 19 + 881;
			const float	   x = Random (seed) * TerrainTextureSize, y = Random (seed + 1) * TerrainTextureSize;
			const float	   angle = 6.28318530718f * Random (seed + 2), length = 1.0f + 5.0f * Random (seed + 3);
			Strand (canvas, x, y, std::cos (angle) * length, std::sin (angle) * length, 0.4f, {69, 57, 38}, 0.65f);
		}
}

void RemoveRepeatedPatches (Canvas &canvas)
{
	// Keep the albedo tile's fine blades and grains, but remove broad local
	// color gradients that would expose its repeat when reduced to mid mips.
	// The shader supplies larger world-space variation. Wrapped filtering
	// preserves seamless borders, and restoring the mean preserves the palette.
	std::array<double, 3> mean{};
	for (const auto &pixel : canvas)
		for (int channel = 0; channel < 3; ++channel)
			mean[channel] += pixel[channel] / canvas.size ();
	Canvas		  blurred = canvas, scratch (canvas.size ());
	constexpr int radius = 4, diameter = 2 * radius + 1;
	for (int pass = 0; pass < 2; ++pass)
		for (int axis = 0; axis < 2; ++axis)
		{
			for (int y = 0; y < TerrainTextureSize; ++y)
				for (int x = 0; x < TerrainTextureSize; ++x)
				{
					Color sum{};
					for (int offset = -radius; offset <= radius; ++offset)
					{
						const int sx = Wrap (x + (axis == 0 ? offset : 0), TerrainTextureSize);
						const int sy = Wrap (y + (axis == 1 ? offset : 0), TerrainTextureSize);
						for (int channel = 0; channel < 3; ++channel)
							sum[channel] += blurred[sy * TerrainTextureSize + sx][channel] / diameter;
					}
					scratch[y * TerrainTextureSize + x] = sum;
				}
			blurred.swap (scratch);
		}
	for (size_t i = 0; i < canvas.size (); ++i)
		for (int channel = 0; channel < 3; ++channel)
			canvas[i][channel] += float (mean[channel]) - blurred[i][channel];
}
} // namespace

TerrainTexturePixels BuildTerrainTexture (TerrainTextureKind kind)
{
	Canvas canvas (TerrainTextureSize * TerrainTextureSize);
	for (int y = 0; y < TerrainTextureSize; ++y)
		for (int x = 0; x < TerrainTextureSize; ++x)
		{
			const float px = x + 0.5f, py = y + 0.5f;
			const float broad = Noise (px, py, 4, 616), medium = Noise (px, py, 16, 844), fine = Noise (px, py, 64, 197);
			const float grain = Random (CellHash (x, y, TerrainTextureSize, 2681)) - 0.5f;
			Color		color{};
			switch (kind)
			{
			case TerrainTextureKind::Grass:
				color = Mix ({77, 70, 43}, {67, 84, 41}, Ease (broad));
				color = Shade (color, 14.0f * (medium - 0.5f) + 9.0f * (fine - 0.5f) + 12.0f * grain);
				break;
			case TerrainTextureKind::Soil:
				color = Mix ({83, 67, 47}, {106, 87, 64}, Ease (broad));
				color = Shade (color, 20.0f * (medium - 0.5f) + 16.0f * (fine - 0.5f) + 21.0f * grain);
				break;
			case TerrainTextureKind::Rock:
			{
				const auto fracture = RockFracture (px, py);
				color = Mix ({99, 98, 90}, {121, 115, 102}, Ease (broad));
				color = Shade (color, 15.0f * fracture.shade + 21.0f * (medium - 0.5f) + 20.0f * (fine - 0.5f) + 17.0f * grain);
				const float crack = 1.0f - Ease (std::clamp (fracture.edge / 0.048f, 0.0f, 1.0f));
				color = Shade (color, -27.0f * crack * (0.5f + 0.5f * medium));
				break;
			}
			case TerrainTextureKind::Sand:
			{
				const float ripple = std::sin ((py / TerrainTextureSize * 12.0f + 0.25f * Noise (px, py, 4, 677)) * 6.28318530718f);
				color = Mix ({139, 122, 88}, {155, 139, 105}, Ease (broad));
				color = Shade (color, 8.0f * (medium - 0.5f) + 7.0f * (fine - 0.5f) + 17.0f * grain + 1.6f * ripple);
				break;
			}
			}
			canvas[y * TerrainTextureSize + x] = color;
		}
	if (kind == TerrainTextureKind::Grass)
		AddGrass (canvas);
	else if (kind == TerrainTextureKind::Soil || kind == TerrainTextureKind::Sand)
		AddGrains (canvas, kind);
	if (kind == TerrainTextureKind::Grass || kind == TerrainTextureKind::Soil)
		RemoveRepeatedPatches (canvas);
	TerrainTexturePixels pixels (TerrainTextureSize * TerrainTextureSize * 4);
	for (size_t i = 0; i < canvas.size (); ++i)
	{
		for (int channel = 0; channel < 3; ++channel)
			pixels[4 * i + channel] = uint8_t (std::clamp (std::lround (canvas[i][channel]), 0L, 255L));
		pixels[4 * i + 3] = 255;
	}
	return pixels;
}
} // namespace quiver
