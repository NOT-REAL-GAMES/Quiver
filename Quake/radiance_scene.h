/* Scene data shared by radiance-cascade tracing backends.
 * The float[4] and uint32_t[4] records deliberately match std430 vec4/uvec4.
 * A scene owns its triangles, a reordered triangle-index array and a threaded
 * depth-first BVH. Callers may append transformed geometry before building. */
#ifndef RADIANCE_SCENE_H
#define RADIANCE_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
	RC_SCENE_CUTOUT = 1u << 0,
	RC_SCENE_LIQUID = 1u << 1,
	RC_SCENE_SKY = 1u << 2,
	RC_SCENE_EMISSIVE = 1u << 3,
	RC_SCENE_DYNAMIC = 1u << 4,
	/* Direct emissive proxy represents this triangle's surface. */
	RC_SCENE_EMISSIVE_PROXY = 1u << 5,
	/* Brush winding is oriented toward its visible, emitting side. */
	RC_SCENE_ORIENTED = 1u << 6
};

typedef struct
{
	float position[3][4];
	float uv01[4];
	float uv2[4];
	uint32_t metadata[4]; /* material, flags, surface, instance */
} rc_scene_triangle_t;

/* Opaque static alias mesh and translation-only instances for optional
 * hardware direct-shadow acceleration. Caller retains arrays during recording. */
typedef struct
{
	const rc_scene_triangle_t *triangles;
	uint32_t triangle_count;
	const float (*origins)[3];
	uint32_t count;
	uint64_t mesh_revision;
} rc_scene_rt_batch_t;

typedef struct
{
	float mins[4], maxs[4];
	uint32_t metadata[4]; /* first index, count (0 = internal), escape, reserved */
} rc_scene_node_t;

typedef struct
{
	rc_scene_triangle_t *triangles;
	uint32_t *indices;
	rc_scene_node_t *nodes;
	uint32_t triangle_count, triangle_capacity, index_count, node_count;
} rc_scene_t;

typedef struct
{
	float distance, position[3], normal[3], barycentric[3], uv[2];
	uint32_t material, flags, surface, instance, triangle;
} rc_scene_hit_t;

typedef bool (*rc_scene_accept_fn) (const rc_scene_hit_t *hit, void *user);

void RCScene_Init (rc_scene_t *scene);
void RCScene_Clear (rc_scene_t *scene);
bool RCScene_AppendTriangle (rc_scene_t *scene, const rc_scene_triangle_t *triangle);
/* Rebuild after every triangle mutation. On failure, tracing is unavailable. */
bool RCScene_Build (rc_scene_t *scene);
/* Transactionally combine two already-built, valid immutable BVHs. Triangle
 * order is first then second. A synthetic root joins their threaded trees;
 * neither source is sorted or refit. Empty sources are supported. */
bool RCScene_Compose (rc_scene_t *scene, const rc_scene_t *first, const rc_scene_t *second);
/* Replace only the second tree of an existing composition when its allocated
 * topology sizes match. The first tree must be unchanged. False changes no
 * bytes, so callers may fall back to transactional full composition. */
bool RCScene_ReplaceSuffix (rc_scene_t *scene, const rc_scene_t *first, const rc_scene_t *second);
/* Refit existing BVH bounds after changing vertex positions without changing
 * triangle count/order. Returns false if the existing topology is invalid. */
bool RCScene_Refit (rc_scene_t *scene);
/* Conservative leaf-AABB overlap; true can be a false positive, never a
 * false negative for valid geometry. Useful for sparse probe-brick activity. */
bool RCScene_AnyGeometryAABB (const rc_scene_t *scene, const float mins[3], const float maxs[3]);
/* CPU reference for unit tests and debugging, not the software GPU backend. */
bool RCScene_TraceReference (const rc_scene_t *scene, const float origin[3], const float direction[3],
	float tmin, float tmax, rc_scene_accept_fn accept, void *user, rc_scene_hit_t *hit);

#endif
