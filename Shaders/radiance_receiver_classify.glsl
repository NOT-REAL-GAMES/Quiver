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

/* A shader may define RC_CLASSIFY_STREAM and RC_ClassifyPoint to read samples on
 * demand. An array indexed by a live count sits in local memory, which at 16
 * samples per block cost up to 9 ms a frame. */
#ifdef RC_CLASSIFY_STREAM
RCReceiverSample RC_ClassifyPoint(uint index);
#define RC_CLASSIFY_POINTS
#define RC_CLASSIFY_POINT(index) RC_ClassifyPoint(index)
#else
#define RC_CLASSIFY_POINTS RCReceiverSample points[16],
#define RC_CLASSIFY_POINT(index) points[index]
#endif

uint RC_ClassifyReceivers(RC_CLASSIFY_POINTS uint count, float planeTolerance,
	float normalCosine, out vec3 representativePosition, out vec3 representativeNormal)
{
	representativePosition = vec3(0);
	representativeNormal = vec3(0);
	bool empty = false, covered = false;
	uvec2 id = uvec2(0);
	for (uint i = 0u; i < count; ++i)
	{
		RCReceiverSample point = RC_CLASSIFY_POINT(i);
		if (!point.valid) return 2u;
		if (all(equal(point.id, uvec2(0)))) { empty = true; continue; }
#if RC_RECEIVER_TEXTURE_FILTER
		if (any(equal(point.id, uvec2(0)))) return 2u;
#endif
		if (!covered)
		{
			covered = true;
			id = point.id;
			representativePosition = point.position;
			representativeNormal = point.normal;
		}
		else if (
#if !RC_RECEIVER_TEXTURE_FILTER
			any(notEqual(point.id, id)) ||
#endif
			dot(representativeNormal, point.normal) < normalCosine ||
#if RC_RECEIVER_TEXTURE_FILTER
			/* Smooth shading normals need not be perpendicular to their own
			 * triangle, and exact primitive identity proves coplanarity. Across
			 * triangles allow the rise those normals permit, or low-resolution
			 * blocks on tessellated surfaces all become holes. */
			(any(notEqual(point.id, id)) &&
			 (abs(dot(representativeNormal, point.position - representativePosition)) > planeTolerance + length(point.position - representativePosition) * sqrt(max(1.0 - normalCosine * normalCosine, 0.0)) ||
			  abs(dot(point.normal, point.position - representativePosition)) > planeTolerance + length(point.position - representativePosition) * sqrt(max(1.0 - normalCosine * normalCosine, 0.0))))
#else
			abs(dot(representativeNormal, point.position - representativePosition)) > planeTolerance
#endif
			)
			return 2u;
	}
	return covered ? (empty ? 2u : 1u) : 0u;
}

#if RC_RECEIVER_TEXTURE_FILTER
bool RC_ClassifyMatches(RCReceiverSample a, RCReceiverSample b, float planeTolerance, float normalCosine)
{
	if (dot(a.normal, b.normal) < normalCosine) return false;
	if (all(equal(a.id, b.id))) return true;
	vec3 delta = b.position - a.position;
	float allowance = planeTolerance + length(delta) * sqrt(max(1.0 - normalCosine * normalCosine, 0.0));
	return abs(dot(a.normal, delta)) <= allowance && abs(dot(b.normal, delta)) <= allowance;
}

/* A block across a silhouette holds several surfaces. Depth cannot be averaged
 * there without inventing points between them, so keep the surface most of the
 * samples agree on, and let the rest take the per-pixel path. Returns the
 * representative sample, or count when no surface covers at least half. */
uint RC_ClassifyMajority(RC_CLASSIFY_POINTS uint count, float planeTolerance, float normalCosine)
{
	uint best = count, bestVotes = 0u;
	for (uint i = 0u; i < count; ++i)
	{
		RCReceiverSample a = RC_CLASSIFY_POINT(i);
		if (!a.valid || any(equal(a.id, uvec2(0)))) continue;
		uint votes = 0u;
		for (uint j = 0u; j < count; ++j)
		{
			RCReceiverSample b = RC_CLASSIFY_POINT(j);
			if (b.valid && !any(equal(b.id, uvec2(0))) &&
				RC_ClassifyMatches(a, b, planeTolerance, normalCosine)) ++votes;
		}
		if (votes > bestVotes) { bestVotes = votes; best = i; }
	}
	return bestVotes * 2u >= count ? best : count;
}
#endif
#endif
