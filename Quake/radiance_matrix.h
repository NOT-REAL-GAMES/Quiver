#ifndef QUIVER_RADIANCE_MATRIX_H
#define QUIVER_RADIANCE_MATRIX_H

#include <stdbool.h>

/* Invert a column-major 4x4 transform. On invalid or singular input the
 * destination is untouched, so the caller can retain classic rendering. */
bool Radiance_InvertMatrix (const float matrix[16], float inverse[16]);

#endif
