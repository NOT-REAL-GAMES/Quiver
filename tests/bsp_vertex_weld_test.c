/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bsp_vertex_weld.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Check (int condition, const char *message)
{
	if (!condition)
	{
		fprintf (stderr, "BSP vertex weld: %s\n", message);
		exit (1);
	}
}

static void Basic (void)
{
	float p[][3] = {
		{-0.01f, 0.0f, 0.0f}, {0.01f, 0.0f, 0.0f}, /* across zero/cell boundary */
		{-10.01f, 0.0f, 0.0f}, {-9.99f, 0.0f, 0.0f}, /* negative coordinates */
		{2.0f, 2.0f, 2.0f}, {2.06f, 2.06f, 2.06f}, /* outside Euclidean epsilon */
		{3.0f, 0.0f, 0.0f}, {3.0625f, 0.0f, 0.0f}, /* exactly on epsilon */
	};
	size_t n = 0;
	Check (BSPVertexWeld (p, sizeof (p) / sizeof (*p), 0.0625f, NULL, &n), "basic call");
	Check (n == 3, "basic weld count and Euclidean threshold");
	Check (p[1][0] == p[0][0] && p[3][0] == p[2][0], "welds across cell boundaries");
	Check (p[5][0] != p[4][0], "does not weld diagonally outside radius");
	Check (p[7][0] == p[6][0], "inclusive distance threshold");
}

static void DeterministicAndGroups (void)
{
	float p[][3] = {
		{0, 0, 0}, {.075f, 0, 0}, {.15f, 0, 0}, /* chain: last must remain independent */
		{5, 0, 0}, {5.05f, 0, 0}, {5.025f, 0, 0},
		{8, 0, 0}, {8.01f, 0, 0}, {8.02f, 0, 0}, {8.03f, 0, 0},
	};
	const uint32_t groups[] = {0, 0, 0, 0, 1, 0, 2, 3, UINT32_MAX, 2};
	size_t n = 0;
	Check (BSPVertexWeld (p, sizeof (p) / sizeof (*p), .1f, groups, &n), "group call");
	Check (n == 3, "group weld count");
	Check (p[1][0] == p[0][0] && p[2][0] == .15f, "no transitive chaining");
	Check (p[4][0] == 5.05f && p[5][0] == 5.0f, "cross-group isolation");
	Check (p[7][0] == 8.01f && p[8][0] == 8.02f && p[9][0] == p[6][0], "excluded and separate groups");
	float q[][3] = {{0, 0, 0}, {.15f, 0, 0}, {.075f, 0, 0}};
	Check (BSPVertexWeld (q, 3, .1f, NULL, &n), "representative call");
	Check (n == 1 && q[2][0] == q[0][0], "lowest index representative selected");
	float nearest[][3] = {{0, 0, 0}, {.15f, 0, 0}, {.09f, 0, 0}};
	Check (BSPVertexWeld (nearest, 3, .1f, NULL, &n), "nearest representative call");
	Check (n == 1 && nearest[2][0] == nearest[1][0], "nearest representative selected");
}

static void InvalidAndSpecial (void)
{
	float p[][3] = {{1, 2, 3}, {1, 2, 3}, {NAN, 2, 3}, {INFINITY, 2, 3}};
	float original[4][3];
	size_t n;
	memcpy (original, p, sizeof p);
	n = 123;
	Check (!BSPVertexWeld (p, 4, -1, NULL, &n) && memcmp (original, p, sizeof p) == 0, "negative epsilon rejected without mutation");
	Check (n == 123, "failure preserves output count");
	Check (!BSPVertexWeld (p, 4, NAN, NULL, &n) && memcmp (original, p, sizeof p) == 0, "NaN epsilon rejected without mutation");
	Check (!BSPVertexWeld (p, 4, INFINITY, NULL, &n) && memcmp (original, p, sizeof p) == 0, "infinite epsilon rejected without mutation");
	Check (!BSPVertexWeld (NULL, 1, .1f, NULL, &n) && n == 123, "missing input rejected");
	Check (!BSPVertexWeld (p, SIZE_MAX, .1f, NULL, &n) && n == 123 && memcmp (original, p, sizeof p) == 0, "oversize rejected before allocation or mutation");
	Check (BSPVertexWeld (NULL, 0, .1f, NULL, &n) && n == 0, "empty input");
	Check (BSPVertexWeld (p, 4, 0, NULL, &n) && n == 1, "zero epsilon exact duplicates");
	Check (isnan (p[2][0]) && isinf (p[3][0]), "nonfinite coordinates untouched");
	float extreme[][3] = {{FLT_MAX, 0, 0}, {FLT_MAX, 0, 0}, {-FLT_MAX, 0, 0}, {0, 0, 0}};
	Check (BSPVertexWeld (extreme, 4, .001f, NULL, &n), "extreme finite coordinates");
	Check (n == 1 && extreme[2][0] == -FLT_MAX && extreme[3][0] == 0, "extreme coordinates remain distinct");
}

static uint32_t state = 0x9e3779b9u;
static uint32_t Random (void)
{
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

static void CompareReference (void)
{
	enum { N = 800 };
	float p[N][3], reference[N][3], original[N][3];
	uint32_t groups[N];
	unsigned char representative[N] = {0};
	size_t count = 0, expected = 0;
	const double eps = 0.125;
	for (int i = 0; i < N; ++i)
	{
		for (int k = 0; k < 3; ++k)
			p[i][k] = ((int)(Random () % 4001) - 2000) * (1.0f / 256.0f);
		groups[i] = Random () % 5;
		if ((i % 7) == 0 && i > 0)
		{
			memcpy (p[i], p[i - 1], sizeof p[i]);
			p[i][0] += 1.0f / 256.0f;
			groups[i] = groups[i - 1];
		}
		if ((i % 37) == 0)
			groups[i] = UINT32_MAX;
	}
	memcpy (reference, p, sizeof p);
	memcpy (original, p, sizeof p);
	for (int i = 0; i < N; ++i)
	{
		int found = -1;
		double best = eps * eps;
		if (groups[i] != UINT32_MAX)
			for (int j = 0; j < i; ++j)
			{
				if (!representative[j] || groups[j] != groups[i])
					continue;
				double dx = (double)original[i][0] - original[j][0];
				double dy = (double)original[i][1] - original[j][1];
				double dz = (double)original[i][2] - original[j][2];
				double distance = dx * dx + dy * dy + dz * dz;
				if (distance <= best && (found < 0 || distance < best))
				{
					found = j;
					best = distance;
				}
			}
		if (found >= 0)
		{
			memcpy (reference[i], original[found], sizeof reference[i]);
			++expected;
		}
		else
			representative[i] = 1;
	}
	Check (BSPVertexWeld (p, N, (float)eps, groups, &count), "randomized call");
	Check (count == expected, "randomized weld count");
	Check (memcmp (reference, p, sizeof p) == 0, "randomized result matches nearest representative reference");
	for (int i = 0; i < N; ++i)
	{
		double dx = (double)p[i][0] - original[i][0];
		double dy = (double)p[i][1] - original[i][1];
		double dz = (double)p[i][2] - original[i][2];
		Check (dx * dx + dy * dy + dz * dz <= eps * eps, "displacement bounded by epsilon");
	}
}

int main (void)
{
	Basic ();
	DeterministicAndGroups ();
	InvalidAndSpecial ();
	CompareReference ();
	puts ("BSP vertex weld tests passed");
	return 0;
}
