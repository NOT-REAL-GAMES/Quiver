/* Shared radiance-cascade scene query. The caller must define
 *   bool RC_AcceptHit(RCSceneHit hit)
 * after this include. Rejected hits (e.g. cutout texels) do not occlude.
 *
 * Hardware mode requires GL_EXT_ray_query, enabled by the including shader,
 * and an acceleration structure containing the same world-space triangles.
 * Instance custom index is the base into rcSceneIndices; primitive index is
 * the offset within that instance's one triangle geometry. */
#ifndef RC_SCENE_TRACE_GLSL
#define RC_SCENE_TRACE_GLSL
#ifndef RC_SCENE_SET
#define RC_SCENE_SET 1
#endif
#ifndef RC_SCENE_BINDING_BASE
#define RC_SCENE_BINDING_BASE 0
#endif

const uint RC_SCENE_CUTOUT = 1u;
const uint RC_SCENE_LIQUID = 2u;
const uint RC_SCENE_SKY = 4u;
const uint RC_SCENE_EMISSIVE = 8u;
const uint RC_SCENE_DYNAMIC = 16u;
const uint RC_SCENE_EMISSIVE_PROXY = 32u;
const uint RC_SCENE_ORIENTED = 64u;

struct RCSceneTriangle { vec4 p0; vec4 p1; vec4 p2; vec4 uv01; vec4 uv2; uvec4 metadata; };
struct RCSceneNode { vec4 mins; vec4 maxs; uvec4 metadata; };
struct RCSceneHit
{
	vec3 position;
	vec3 normal;
	vec3 barycentric;
	vec2 uv;
	float distance;
	uint material;
	uint flags;
	uint surface;
	uint instance;
	uint triangle;
};
layout (std430, set = RC_SCENE_SET, binding = RC_SCENE_BINDING_BASE + 0) readonly buffer RCSceneNodes { RCSceneNode rcSceneNodes[]; };
layout (std430, set = RC_SCENE_SET, binding = RC_SCENE_BINDING_BASE + 1) readonly buffer RCSceneIndices { uint rcSceneIndices[]; };
layout (std430, set = RC_SCENE_SET, binding = RC_SCENE_BINDING_BASE + 2) readonly buffer RCSceneTriangles { RCSceneTriangle rcSceneTriangles[]; };
layout (std430, set = RC_SCENE_SET, binding = RC_SCENE_BINDING_BASE + 3) readonly buffer RCSceneCounts { uvec4 rcSceneCounts; }; // nodes, indices, triangles, reserved
#ifdef RC_HARDWARE_RAY_QUERY
layout (set = RC_SCENE_SET, binding = RC_SCENE_BINDING_BASE + 4) uniform accelerationStructureEXT rcSceneAcceleration;
#endif

bool RC_AcceptHit (RCSceneHit hit);

/* Conservative sparse-brick activity query. Expand the queried box by the
 * interpolation support radius. A leaf bounding box can give a safe false
 * positive. Also mark receiver/light bounds and far-cascade parent support;
 * geometry alone cannot represent visibility through empty space. */
bool RC_SceneAnyGeometryAABB (vec3 mins, vec3 maxs)
{
	uint nodeIndex = 0u;
	while (nodeIndex < rcSceneCounts.x)
	{
		RCSceneNode node = rcSceneNodes[nodeIndex];
		if (any (lessThan (node.maxs.xyz, mins)) || any (greaterThan (node.mins.xyz, maxs)))
		{
			if (node.metadata.z <= nodeIndex) break;
			nodeIndex = node.metadata.z;
			continue;
		}
		if (node.metadata.y != 0u) return true;
		++nodeIndex;
	}
	return false;
}

RCSceneHit RC_MakeHit (uint triangle, vec3 origin, vec3 direction, float t, vec2 bary)
{
	RCSceneTriangle tri = rcSceneTriangles[triangle];
	RCSceneHit hit;
	hit.position = origin + t * direction;
	hit.normal = normalize (cross (tri.p1.xyz - tri.p0.xyz, tri.p2.xyz - tri.p0.xyz));
	hit.barycentric = vec3 (1.0 - bary.x - bary.y, bary.x, bary.y);
	hit.uv = tri.uv01.xy * hit.barycentric.x + tri.uv01.zw * bary.x + tri.uv2.xy * bary.y;
	hit.distance = t;
	hit.material = tri.metadata.x;
	hit.flags = tri.metadata.y;
	hit.surface = tri.metadata.z;
	hit.instance = tri.metadata.w;
	hit.triangle = triangle;
	return hit;
}

#ifndef RC_HARDWARE_RAY_QUERY
bool RC_IntersectsBox (RCSceneNode node, vec3 o, vec3 d, float tmin, float tmax)
{
	for (int a = 0; a < 3; ++a)
	{
		if (d[a] == 0.0)
		{
			if (o[a] < node.mins[a] || o[a] > node.maxs[a]) return false;
			continue;
		}
		float x = (node.mins[a] - o[a]) / d[a];
		float y = (node.maxs[a] - o[a]) / d[a];
		tmin = max (tmin, min (x, y));
		tmax = min (tmax, max (x, y));
		if (tmin > tmax) return false;
	}
	return true;
}

bool RC_IntersectsTriangle (RCSceneTriangle tri, vec3 o, vec3 d, float tmin, float tmax, out float t, out vec2 bary)
{
	vec3 e1 = tri.p1.xyz - tri.p0.xyz;
	vec3 e2 = tri.p2.xyz - tri.p0.xyz;
	vec3 p = cross (d, e2);
	float det = dot (e1, p);
	if (abs (det) < 1e-8) return false;
	float inv = 1.0 / det;
	vec3 s = o - tri.p0.xyz;
	bary.x = dot (s, p) * inv;
	if (bary.x < 0.0 || bary.x > 1.0) return false;
	vec3 q = cross (s, e1);
	bary.y = dot (d, q) * inv;
	if (bary.y < 0.0 || bary.x + bary.y > 1.0) return false;
	t = dot (e2, q) * inv;
	return t >= tmin && t <= tmax;
}
#endif

bool RC_SceneTrace (vec3 origin, vec3 direction, float tmin, float tmax, out RCSceneHit hit)
{
	bool found = false;
	if (tmax < tmin) return false;
#ifdef RC_HARDWARE_RAY_QUERY
	rayQueryEXT query;
	/* Force triangle candidates through the material callback even if a BLAS
	 * was built opaque, so cutouts behave like the software backend. */
	rayQueryInitializeEXT (query, rcSceneAcceleration, gl_RayFlagsNoOpaqueEXT, 1, origin, tmin, direction, tmax);
	while (rayQueryProceedEXT (query))
	{
		if (rayQueryGetIntersectionTypeEXT (query, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
		uint base = rayQueryGetIntersectionInstanceCustomIndexEXT (query, false);
		uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT (query, false);
		if (base >= rcSceneCounts.y || primitive >= rcSceneCounts.y - base) continue;
		uint triangle = rcSceneIndices[base + primitive];
		if (triangle >= rcSceneCounts.z) continue;
		RCSceneHit candidate = RC_MakeHit (triangle, origin, direction, rayQueryGetIntersectionTEXT (query, false),
			rayQueryGetIntersectionBarycentricsEXT (query, false));
		if (RC_AcceptHit (candidate)) rayQueryConfirmIntersectionEXT (query);
	}
	if (rayQueryGetIntersectionTypeEXT (query, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
	{
		uint base = rayQueryGetIntersectionInstanceCustomIndexEXT (query, true);
		uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT (query, true);
		if (base < rcSceneCounts.y && primitive < rcSceneCounts.y - base)
		{
			uint triangle = rcSceneIndices[base + primitive];
			if (triangle < rcSceneCounts.z)
			{
				hit = RC_MakeHit (triangle, origin, direction, rayQueryGetIntersectionTEXT (query, true),
					rayQueryGetIntersectionBarycentricsEXT (query, true));
				found = true;
			}
		}
	}
#else
	uint nodeIndex = 0u;
	while (nodeIndex < rcSceneCounts.x)
	{
		RCSceneNode node = rcSceneNodes[nodeIndex];
		if (!RC_IntersectsBox (node, origin, direction, tmin, tmax))
		{
			/* Escape links always advance; terminate safely on corrupt data. */
			if (node.metadata.z <= nodeIndex) break;
			nodeIndex = node.metadata.z;
			continue;
		}
		uint first = node.metadata.x, count = node.metadata.y;
		if (first < rcSceneCounts.y && count <= rcSceneCounts.y - first)
		{
			for (uint i = 0u; i < count; ++i)
			{
				uint triangle = rcSceneIndices[first + i];
				if (triangle >= rcSceneCounts.z) continue;
				float t; vec2 bary;
				if (!RC_IntersectsTriangle (rcSceneTriangles[triangle], origin, direction, tmin, tmax, t, bary)) continue;
				RCSceneHit candidate = RC_MakeHit (triangle, origin, direction, t, bary);
				if (RC_AcceptHit (candidate)) { hit = candidate; tmax = t; found = true; }
			}
		}
		++nodeIndex;
	}
#endif
	return found;
}

/* Batch instances are verified opaque and carry no material data, so sun rays
 * only need to know whether one lies before the sky. */
bool RC_SceneBatchOccluded (vec3 origin, vec3 direction, float tmin, float tmax)
{
#ifdef RC_HARDWARE_RAY_QUERY
	if (tmax < tmin) return false;
	rayQueryEXT query;
	rayQueryInitializeEXT (query, rcSceneAcceleration, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 2, origin, tmin, direction, tmax);
	while (rayQueryProceedEXT (query)) {}
	return rayQueryGetIntersectionTypeEXT (query, true) != gl_RayQueryCommittedIntersectionNoneEXT;
#else
	return false;
#endif
}

/* Local-light visibility only needs to know whether any accepted surface
 * blocks the segment. It need not find the closest hit or build normals/UVs
 * for opaque triangles. Sky blocks local lights; liquid is transparent. */
bool RC_SceneOccluded (vec3 origin, vec3 direction, float tmin, float tmax)
{
	if (tmax < tmin) return false;
#ifdef RC_HARDWARE_RAY_QUERY
	rayQueryEXT query;
	rayQueryInitializeEXT (query, rcSceneAcceleration, gl_RayFlagsNoOpaqueEXT, 3, origin, tmin, direction, tmax);
	while (rayQueryProceedEXT (query))
	{
		if (rayQueryGetIntersectionTypeEXT (query, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
		uint base = rayQueryGetIntersectionInstanceCustomIndexEXT (query, false);
		/* Translation-only batch instances are verified opaque at build time.
		 * They cast direct shadows but remain excluded from closest-hit GI. */
		if (base == 0xffffffu) return true;
		uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT (query, false);
		if (base >= rcSceneCounts.y || primitive >= rcSceneCounts.y - base) continue;
		uint triangle = rcSceneIndices[base + primitive];
		if (triangle >= rcSceneCounts.z) continue;
		uint flags = rcSceneTriangles[triangle].metadata.y;
		if ((flags & RC_SCENE_SKY) != 0u) return true;
		if ((flags & RC_SCENE_LIQUID) != 0u) continue;
		if ((flags & RC_SCENE_CUTOUT) == 0u) return true;
		RCSceneHit candidate = RC_MakeHit (triangle, origin, direction, rayQueryGetIntersectionTEXT (query, false),
			rayQueryGetIntersectionBarycentricsEXT (query, false));
		if (RC_AcceptHit (candidate)) return true;
	}
#else
	uint nodeIndex = 0u;
	while (nodeIndex < rcSceneCounts.x)
	{
		RCSceneNode node = rcSceneNodes[nodeIndex];
		if (!RC_IntersectsBox (node, origin, direction, tmin, tmax))
		{
			if (node.metadata.z <= nodeIndex) break;
			nodeIndex = node.metadata.z;
			continue;
		}
		uint first = node.metadata.x, count = node.metadata.y;
		if (first < rcSceneCounts.y && count <= rcSceneCounts.y - first)
			for (uint i = 0u; i < count; ++i)
			{
				uint triangle = rcSceneIndices[first + i];
				if (triangle >= rcSceneCounts.z) continue;
				RCSceneTriangle tri = rcSceneTriangles[triangle];
				uint flags = tri.metadata.y;
				if ((flags & RC_SCENE_LIQUID) != 0u && (flags & RC_SCENE_SKY) == 0u) continue;
				float t; vec2 bary;
				if (!RC_IntersectsTriangle (tri, origin, direction, tmin, tmax, t, bary)) continue;
				if ((flags & RC_SCENE_SKY) != 0u || (flags & RC_SCENE_CUTOUT) == 0u) return true;
				if (RC_AcceptHit (RC_MakeHit (triangle, origin, direction, t, bary))) return true;
			}
		++nodeIndex;
	}
#endif
	return false;
}
#endif
