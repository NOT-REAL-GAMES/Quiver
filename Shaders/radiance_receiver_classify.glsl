#ifndef RADIANCE_RECEIVER_CLASSIFY_GLSL
#define RADIANCE_RECEIVER_CLASSIFY_GLSL

/* Stable nonzero identities distinguish covered samples from sky. Legacy
 * receivers also require primitive equality; texture receivers accept
 * continuous coplanar triangles while preserving mixed coverage fallback. */
struct RCReceiverSample
{
	vec3 position;
	vec3 normal;
	uvec2 id;
	bool valid;
};

uint RC_ClassifyReceivers(RCReceiverSample points[16], uint count, float planeTolerance,
	float normalCosine, out vec3 representativePosition, out vec3 representativeNormal)
{
	representativePosition = vec3(0);
	representativeNormal = vec3(0);
	bool empty = false, covered = false;
	uvec2 id = uvec2(0);
	for (uint i = 0u; i < count; ++i)
	{
		if (!points[i].valid) return 2u;
		if (all(equal(points[i].id, uvec2(0)))) { empty = true; continue; }
#if RC_RECEIVER_TEXTURE_FILTER
		if (any(equal(points[i].id, uvec2(0)))) return 2u;
#endif
		if (!covered)
		{
			covered = true;
			id = points[i].id;
			representativePosition = points[i].position;
			representativeNormal = points[i].normal;
		}
		else if (
#if !RC_RECEIVER_TEXTURE_FILTER
			any(notEqual(points[i].id, id)) ||
#endif
			dot(representativeNormal, points[i].normal) < normalCosine ||
#if RC_RECEIVER_TEXTURE_FILTER
			/* Smooth shading normals need not be perpendicular to their own
			 * triangle. Exact primitive identity already proves coplanarity. */
			(any(notEqual(points[i].id, id)) &&
			 (abs(dot(representativeNormal, points[i].position - representativePosition)) > planeTolerance ||
			  abs(dot(points[i].normal, points[i].position - representativePosition)) > planeTolerance))
#else
			abs(dot(representativeNormal, points[i].position - representativePosition)) > planeTolerance
#endif
			)
			return 2u;
	}
	return covered ? (empty ? 2u : 1u) : 0u;
}
#endif
