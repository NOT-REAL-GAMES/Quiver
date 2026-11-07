#version 460
/* Collapses the multisampled receiver prepass to one sample per pixel, for the
 * stages that only need one: forward ID checks, the model cull, and diagnostics.
 * The classifier reads the samples directly. Each pixel keeps its largest facet;
 * a triangle seam on a flat surface is one facet, so it does not split the vote.
 * Standard sample locations average to the pixel center, so a full pixel's mean
 * depth is the depth there. */
layout(set = 0, binding = 0) uniform sampler2DMS receiverDepth;
layout(set = 0, binding = 1) uniform sampler2DMS receiverNormal;
layout(set = 0, binding = 2) uniform usampler2DMS receiverID;
layout(location = 0) out vec4 outNormal;
layout(location = 1) out uvec2 outID;

const int RC_MAX_SAMPLES = 8;

bool sameFacet(uvec2 aID, vec3 aNormal, uvec2 bID, vec3 bNormal)
{
	return aID.x == bID.x && (aID.y == bID.y || dot(aNormal, bNormal) > 0.999);
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	int count = min(textureSamples(receiverID), RC_MAX_SAMPLES);
	uvec2 ids[RC_MAX_SAMPLES];
	vec3 normals[RC_MAX_SAMPLES];
	float depths[RC_MAX_SAMPLES];
	for (int i = 0; i < count; ++i)
	{
		ids[i] = texelFetch(receiverID, pixel, i).rg;
		normals[i] = texelFetch(receiverNormal, pixel, i).xyz;
		depths[i] = texelFetch(receiverDepth, pixel, i).r;
	}
	/* An edge against empty space stays on the surface. */
	int best = 0, bestVotes = 0;
	for (int i = 0; i < count; ++i)
	{
		if (all(equal(ids[i], uvec2(0)))) continue;
		int votes = 0;
		for (int j = 0; j < count; ++j)
			if (sameFacet(ids[i], normals[i], ids[j], normals[j])) ++votes;
		if (votes > bestVotes) { bestVotes = votes; best = i; }
	}
	float depth = 0.0;
	int members = 0;
	for (int i = 0; i < count; ++i)
		if (sameFacet(ids[best], normals[best], ids[i], normals[i])) { depth += depths[i]; ++members; }
	gl_FragDepth = depth / float(max(members, 1));
	outNormal = vec4(normals[best], 0.0);
	outID = ids[best];
}
