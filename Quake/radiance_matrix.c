#include "radiance_matrix.h"

#include <float.h>
#include <math.h>

bool Radiance_InvertMatrix (const float matrix[16], float inverse[16])
{
	if (!matrix || !inverse) return false;
	double augmented[4][8];
	for (int row = 0; row < 4; ++row)
		for (int col = 0; col < 8; ++col)
		{
			const double value = col < 4 ? matrix[col * 4 + row] : (col - 4 == row ? 1.0 : 0.0);
			if (!isfinite (value)) return false;
			augmented[row][col] = value;
		}
	for (int col = 0; col < 4; ++col)
	{
		int pivot = col;
		for (int row = col + 1; row < 4; ++row)
			if (fabs (augmented[row][col]) > fabs (augmented[pivot][col])) pivot = row;
		if (fabs (augmented[pivot][col]) < DBL_MIN) return false;
		if (pivot != col)
			for (int j = 0; j < 8; ++j)
			{
				double swap = augmented[col][j]; augmented[col][j] = augmented[pivot][j]; augmented[pivot][j] = swap;
			}
		const double scale = augmented[col][col];
		for (int j = 0; j < 8; ++j) augmented[col][j] /= scale;
		for (int row = 0; row < 4; ++row)
		{
			if (row == col) continue;
			const double factor = augmented[row][col];
			for (int j = 0; j < 8; ++j) augmented[row][j] -= factor * augmented[col][j];
		}
	}
	float result[16];
	for (int row = 0; row < 4; ++row)
		for (int col = 0; col < 4; ++col)
		{
			const double value = augmented[row][col + 4];
			if (!isfinite (value) || fabs (value) > FLT_MAX) return false;
			result[col * 4 + row] = (float)value;
		}
	for (int i = 0; i < 16; ++i) inverse[i] = result[i];
	return true;
}
