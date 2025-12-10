/*
Copyright (C) 2024 Quiver developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// noise_gen.c -- 3D noise texture generation for volumetric clouds

#include "quakedef.h"

// Permutation table for Perlin noise
static const int perm[512] = {
	151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,
	190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,
	125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,
	41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130,
	116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207,
	206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,
	172, 9,   129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144,
	12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,
	127, 4,   150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180,
	// Repeat for wrap-around
	151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,
	190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,
	125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,
	41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130,
	116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207,
	206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,
	172, 9,   129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144,
	12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,
	127, 4,   150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180};

/*
================
Noise_Fade

Fade function for Perlin noise (6t^5 - 15t^4 + 10t^3)
================
*/
static float Noise_Fade (float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/*
================
Noise_Lerp

Linear interpolation
================
*/
static float Noise_Lerp (float a, float b, float t)
{
	return a + t * (b - a);
}

/*
================
Noise_Grad3D

Gradient function for 3D Perlin noise
================
*/
static float Noise_Grad3D (int hash, float x, float y, float z)
{
	int	  h = hash & 15;
	float u = h < 8 ? x : y;
	float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
	return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

/*
================
Noise_Perlin3D

3D Perlin noise function
Returns value in range [-1, 1]
================
*/
static float Noise_Perlin3D (float x, float y, float z)
{
	// Find unit cube containing point
	int X = (int)floorf (x) & 255;
	int Y = (int)floorf (y) & 255;
	int Z = (int)floorf (z) & 255;

	// Find relative position in cube
	x -= floorf (x);
	y -= floorf (y);
	z -= floorf (z);

	// Compute fade curves
	float u = Noise_Fade (x);
	float v = Noise_Fade (y);
	float w = Noise_Fade (z);

	// Hash coordinates of cube corners
	int A = perm[X] + Y;
	int AA = perm[A] + Z;
	int AB = perm[A + 1] + Z;
	int B = perm[X + 1] + Y;
	int BA = perm[B] + Z;
	int BB = perm[B + 1] + Z;

	// Blend results from 8 corners
	return Noise_Lerp (
		Noise_Lerp (Noise_Lerp (Noise_Grad3D (perm[AA], x, y, z), Noise_Grad3D (perm[BA], x - 1, y, z), u),
					Noise_Lerp (Noise_Grad3D (perm[AB], x, y - 1, z), Noise_Grad3D (perm[BB], x - 1, y - 1, z), u), v),
		Noise_Lerp (Noise_Lerp (Noise_Grad3D (perm[AA + 1], x, y, z - 1), Noise_Grad3D (perm[BA + 1], x - 1, y, z - 1), u),
					Noise_Lerp (Noise_Grad3D (perm[AB + 1], x, y - 1, z - 1), Noise_Grad3D (perm[BB + 1], x - 1, y - 1, z - 1), u), v),
		w);
}

/*
================
Noise_Hash3D

Simple hash function for Worley noise point generation
================
*/
static unsigned int Noise_Hash3D (int x, int y, int z)
{
	unsigned int h = (unsigned int)(x * 374761393 + y * 668265263 + z * 1274126177);
	h = (h ^ (h >> 13)) * 1274126177;
	return h;
}

/*
================
Noise_Worley3D

3D Worley (cellular) noise function
Returns distance to nearest feature point, normalized to [0, 1]
================
*/
static float Noise_Worley3D (float x, float y, float z)
{
	int ix = (int)floorf (x);
	int iy = (int)floorf (y);
	int iz = (int)floorf (z);

	float min_dist = 1e10f;

	// Check 3x3x3 neighborhood of cells
	for (int dx = -1; dx <= 1; dx++)
	{
		for (int dy = -1; dy <= 1; dy++)
		{
			for (int dz = -1; dz <= 1; dz++)
			{
				int cx = ix + dx;
				int cy = iy + dy;
				int cz = iz + dz;

				// Generate random point in this cell
				unsigned int h = Noise_Hash3D (cx, cy, cz);
				float		 px = (float)cx + (float)(h & 0xFF) / 255.0f;
				h >>= 8;
				float py = (float)cy + (float)(h & 0xFF) / 255.0f;
				h >>= 8;
				float pz = (float)cz + (float)(h & 0xFF) / 255.0f;

				// Distance to this point
				float ddx = x - px;
				float ddy = y - py;
				float ddz = z - pz;
				float dist = sqrtf (ddx * ddx + ddy * ddy + ddz * ddz);

				if (dist < min_dist)
					min_dist = dist;
			}
		}
	}

	// Normalize (max possible distance in unit cube is sqrt(3) ~ 1.732)
	return CLAMP (0.0f, min_dist / 1.5f, 1.0f);
}

/*
================
Noise_FBM3D

Fractal Brownian Motion - multiple octaves of Perlin noise
================
*/
static float Noise_FBM3D (float x, float y, float z, int octaves, float persistence, float lacunarity)
{
	float total = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float max_value = 0.0f;

	for (int i = 0; i < octaves; i++)
	{
		total += Noise_Perlin3D (x * frequency, y * frequency, z * frequency) * amplitude;
		max_value += amplitude;
		amplitude *= persistence;
		frequency *= lacunarity;
	}

	return total / max_value;
}

/*
================
Noise_WorleyFBM3D

Fractal Worley noise
================
*/
static float Noise_WorleyFBM3D (float x, float y, float z, int octaves, float persistence, float lacunarity)
{
	float total = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float max_value = 0.0f;

	for (int i = 0; i < octaves; i++)
	{
		total += Noise_Worley3D (x * frequency, y * frequency, z * frequency) * amplitude;
		max_value += amplitude;
		amplitude *= persistence;
		frequency *= lacunarity;
	}

	return total / max_value;
}

/*
================
Noise_CloudDensity3D

Combined Worley-Perlin noise for cloud density
This creates the characteristic puffy cloud look
================
*/
static float Noise_CloudDensity3D (float x, float y, float z)
{
	// Base shape from Perlin FBM
	float perlin = Noise_FBM3D (x, y, z, 4, 0.5f, 2.0f);
	perlin = (perlin + 1.0f) * 0.5f; // Remap to [0, 1]

	// Detail from inverted Worley noise (creates puffy shapes)
	float worley = 1.0f - Noise_WorleyFBM3D (x * 2.0f, y * 2.0f, z * 2.0f, 3, 0.5f, 2.0f);

	// Combine: Perlin for large shapes, Worley for detail
	float combined = perlin * 0.6f + worley * 0.4f;

	// Add high-frequency detail
	float detail = Noise_FBM3D (x * 8.0f, y * 8.0f, z * 8.0f, 2, 0.5f, 2.0f);
	detail = (detail + 1.0f) * 0.5f;

	combined = combined * 0.8f + detail * 0.2f;

	return CLAMP (0.0f, combined, 1.0f);
}

/*
================
Noise_Generate3DTexture

Generate a 3D noise texture for volumetric clouds
Returns allocated texture data (caller must free)
================
*/
byte *Noise_Generate3DTexture (int size)
{
	byte *data = (byte *)Mem_Alloc (size * size * size);

	float scale = 4.0f / (float)size; // Adjust for good cloud scale

	for (int z = 0; z < size; z++)
	{
		for (int y = 0; y < size; y++)
		{
			for (int x = 0; x < size; x++)
			{
				float fx = (float)x * scale;
				float fy = (float)y * scale;
				float fz = (float)z * scale;

				float noise = Noise_CloudDensity3D (fx, fy, fz);

				int index = z * size * size + y * size + x;
				data[index] = (byte)(noise * 255.0f);
			}
		}
	}

	return data;
}
