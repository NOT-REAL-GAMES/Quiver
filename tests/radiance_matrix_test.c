#include "radiance_matrix.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void CheckInverse (const float matrix[16])
{
	float inverse[16];
	assert (Radiance_InvertMatrix (matrix, inverse));
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
		{
			double value = 0;
			for (int k = 0; k < 4; ++k) value += (double)matrix[k * 4 + row] * inverse[column * 4 + k];
			assert (fabs (value - (row == column ? 1.0 : 0.0)) < 1e-5);
		}
	float in_place[16]; memcpy (in_place, matrix, sizeof (in_place));
	assert (Radiance_InvertMatrix (in_place, in_place));
	for (int i = 0; i < 16; ++i) assert (in_place[i] == inverse[i]);
}

int main (void)
{
	const float affine[16] = {0, 2, 0, 0, -3, 0, 0, 0, 0, 0, 4, 0, 100, -20, 7, 1};
	/* The renderer's reversed-Z infinite projection has a zero z diagonal. */
	const float projection[16] = {1.2f, 0, 0, 0, 0, -1.8f, 0, 0, 0, 0, 0, -1, 0, 0, 4, 0};
	CheckInverse (affine); CheckInverse (projection);
	float output[16], untouched[16];
	for (int i = 0; i < 16; ++i) output[i] = untouched[i] = 42.0f;
	const float singular[16] = {0};
	assert (!Radiance_InvertMatrix (singular, output) && !memcmp (output, untouched, sizeof (output)));
	float invalid[16]; memcpy (invalid, affine, sizeof (invalid)); invalid[3] = NAN;
	assert (!Radiance_InvertMatrix (invalid, output) && !memcmp (output, untouched, sizeof (output)));
	assert (!Radiance_InvertMatrix (NULL, output));
	assert (!Radiance_InvertMatrix (affine, NULL));
	puts ("PASS: affine, reversed-Z projection, aliasing and invalid matrix cases");
	return 0;
}
