/* CPU-retained alias geometry for radiance scene extraction. */
#ifndef RADIANCE_ALIAS_H
#define RADIANCE_ALIAS_H
#include "radiance_scene.h"

typedef struct rc_alias_mesh_s rc_alias_mesh_t;
typedef enum { RC_ALIAS_MDL, RC_ALIAS_MD3, RC_ALIAS_SKINNED_STATIC } rc_alias_format_t;

/* Copy the exact packed GPU pose and UV data while model loading owns it.
 * xyz_stride must be at least 3*sizeof(uint16_t), uv_stride >= 2*sizeof(float).
 * Returns NULL on invalid input or allocation failure. */
rc_alias_mesh_t *RCAlias_Create (rc_alias_format_t format, uint32_t num_vertices, uint32_t num_poses,
	uint32_t num_indices, const void *xyz, size_t xyz_stride, const void *uv, size_t uv_stride,
	const uint16_t *indices, const float scale[3], const float scale_origin[3]);
/* Retain one MD5 pose from the exact renderer vertex stream (88-byte four-
 * influence or144-byte eight-influence layout, with optional stride padding).
 * joint_rows contains num_joints absolute frame0 row-major3x4 matrices.
 * The packed bytes are normalized for the translation term only: joint-local
 * xyz are already weighted and must not be multiplied by those bytes again.
 * UVs/indices/float positions become owned; no animation matrices are kept.
 * Caller must reject animated headers before invoking this static factory. */
rc_alias_mesh_t *RCAlias_CreateSkinnedStatic (uint32_t num_vertices, uint32_t num_indices,
    const void *vertices, size_t vertex_stride, uint32_t num_influences, uint32_t num_joints,
    const float *joint_rows, const uint16_t *indices);
void RCAlias_Destroy (rc_alias_mesh_t *mesh);
/* Unique allocation generation, even if an allocator reuses the address. */
uint64_t RCAlias_Revision (const rc_alias_mesh_t *mesh);
size_t RCAlias_Bytes (const rc_alias_mesh_t *mesh);
uint32_t RCAlias_TriangleCount (const rc_alias_mesh_t *mesh);
/* Requested CPU bytes for all live retained alias meshes, including headers. */
uint64_t RCAlias_TotalBytes (void);

/* Append interpolated, world-transformed triangles; matrix is column-major.
 * Returns false without publishing a partial mesh on error. */
bool RCAlias_Append (rc_scene_t *scene, const rc_alias_mesh_t *mesh, uint32_t pose1, uint32_t pose2,
	float blend, const float matrix[16], uint32_t material, uint32_t flags, uint32_t surface, uint32_t instance);

#endif
