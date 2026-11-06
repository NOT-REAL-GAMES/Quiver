/* Compiled static model instances and world-space collision triangles.
 * Q_STATIC_MODELS is a versioned BSPX extension; the base BSP stays29/BSP2. */
#ifndef QUIVER_STATIC_MODELS_H
#define QUIVER_STATIC_MODELS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QSM_LUMP_NAME "Q_STATIC_MODELS"
#define QSM_VERSION 1u
#define QSM_MODEL_PATH 256u
#define QSM_COLLISION 1u
#define QSM_MAX_INSTANCES 65536u
#define QSM_MAX_TRIANGLES (8u * 1024u * 1024u)

typedef struct
{
	char model[QSM_MODEL_PATH];
	float origin[3], angles[3], modelscale;
	uint32_t skin, flags, first_triangle, triangle_count;
} qsm_instance_t;

typedef struct { float position[3][3]; } qsm_triangle_t;
typedef struct qsm_collision_s qsm_collision_t;
typedef struct qsm_data_s
{
	qsm_instance_t *instances;
	qsm_triangle_t *triangles;
	uint32_t instance_count, triangle_count;
	qsm_collision_t *collision;
} qsm_data_t;

typedef struct
{
	bool startsolid, allsolid;
	float fraction, endpos[3], normal[3], plane_dist;
	uint32_t instance, triangle;
} qsm_trace_t;

void QSM_Free (qsm_data_t *data);
bool QSM_ValidModelPath (const char *path);
/* Inputs and outputs are owned by the caller. Failed decodes/builds preserve
 * prior data. Encoded bytes use explicit little endian fields, no C padding. */
bool QSM_Encode (const qsm_data_t *data, uint8_t **bytes, size_t *length, char *error, size_t error_size);
bool QSM_Decode (qsm_data_t *data, const void *bytes, size_t length, char *error, size_t error_size);
bool QSM_FindBSPXLump (const void *bsp, size_t bsp_bytes, const void **lump, size_t *lump_bytes,
	char *error, size_t error_size);
bool QSM_AttachBSPX (const void *bsp, size_t bsp_bytes, const void *lump, size_t lump_bytes,
	uint8_t **output, size_t *output_bytes, char *error, size_t error_size);

/* Collision is independent of Radiance and remains active on dedicated
 * servers and with classic lighting. Sweeps use actual triangle geometry. */
bool QSM_BuildCollision (qsm_data_t *data, char *error, size_t error_size);
void QSM_FreeCollision (qsm_collision_t *collision);
bool QSM_Trace (const qsm_data_t *data, const float start[3], const float mins[3],
	const float maxs[3], const float end[3], qsm_trace_t *trace);
#endif
