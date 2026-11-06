/* Include after radiance_cascades.glsl. Returns false when no valid visible
 * probes cover the point, so the caller can use runtime direct light while
 * the cascade converges. The result is diffuse irradiance / PI. */
#ifndef QUIVER_RADIANCE_LOOKUP_GLSL
#define QUIVER_RADIANCE_LOOKUP_GLSL
#if !RC_RECEIVER_TEXTURE_FILTER || RC_RECEIVER_GAUSSIAN_FALLBACK
#include "radiance_filter.glsl"
#endif

uvec2 RC_DirectionBin (vec3 direction, uint side)
{
	float u = fract (atan (direction.y, direction.x) / (2.0 * RC_PI) + 1.0);
	float v = clamp ((1.0 - direction.z) * 0.5, 0.0, 0.99999994);
	return min (uvec2 (vec2 (u, v) * float (side)), uvec2 (side - 1u));
}

/* The four bins in the common 2x2 grid are separated by the Y and Z
 * planes. Near a boundary use the original normalized trigonometric path:
 * rounding in atan/fract can otherwise select a different bin. */
uvec2 RC_DirectionBinFromOffset (vec3 offset, float distance, uint side)
{
	if (side == 2u && abs (offset.y) > distance * 0.00001 && abs (offset.z) > distance * 0.00001)
		return uvec2 (offset.y < 0.0 ? 1u : 0u, offset.z < 0.0 ? 1u : 0u);
	return RC_DirectionBin (offset / distance, side);
}

/* Level zero normally uses 4^3 bricks with four directions per probe.
 * Keep their address calculation in integer shifts; dynamic division and
 * remainder are otherwise repeated for every tap of the spatial filter. */
uint RC_ReceiverProbeBase (RCCascade c, uvec3 cell)
{
	if (c.meta.w != 4u || c.meta.y != 2u)
		return RC_RecordIndex (c, cell, uvec2 (0));
	if (any (greaterThanEqual (cell, uvec3 (c.meta.x)))) return RC_ABSENT;
	uint side = c.meta.x >> 2u;
	uvec3 brick = cell >> 2u, local = cell & 3u;
	uint tableIndex = c.meta.z + (brick.z * side + brick.y) * side + brick.x;
	if (tableIndex >= rcBrickTable.length ()) return RC_ABSENT;
	uint base = rcBrickTable[tableIndex];
	if (base == RC_ABSENT) return RC_ABSENT;
	uint within = ((local.z * 4u + local.y) * 4u + local.x) * 4u;
	return base > 0xffffffffu - within ? RC_ABSENT : base + within;
}

bool RC_SampleDiffuseMode (vec3 point, vec3 normal, float strength, out vec3 irradiance)
{
	RCCascade c = rcLevels[0];
	if (c.meta.x == 0u || c.meta.y == 0u || c.origin_spacing.w <= 0.0) { irradiance = vec3 (0.0); return false; }
	vec3 coord = (point - c.origin_spacing.xyz) / c.origin_spacing.w - vec3 (0.5);
	if (any (lessThan (coord, vec3 (-0.5))) || any (greaterThan (coord, vec3 (float (c.meta.x) - 0.5))))
	{ irradiance = vec3 (0.0); return false; }
	/* The reduced producer always takes eight visible corners. Fragment
	 * fallback retains the requested world-space Gaussian at failed guides. */
#if RC_RECEIVER_TEXTURE_FILTER && !RC_RECEIVER_GAUSSIAN_FALLBACK
	ivec3 center = ivec3 (floor (coord));
	vec3 relative = coord - vec3 (center);
	vec3 axisWeight[2];
	axisWeight[0] = vec3 (1.0) - relative; axisWeight[1] = relative;
#else
	bool gaussian = strength > 0.0;
	bool wide = strength > 1.0;
	/* Wider strengths use five probes per axis. Each kernel tapers to zero
	 * at the edge of its window to avoid seams when the center changes. */
	ivec3 center = ivec3 (floor (coord + vec3 (gaussian ? 0.5 : 0.0)));
	vec3 relative = coord - vec3 (center);
	vec3 axisWeight[5];
	if (gaussian) for (int i = 0; i < (wide ? 5 : 3); ++i)
	{
		vec3 distance = abs (relative - vec3 (float (i - (wide ? 2 : 1))));
		axisWeight[i] = RC_FilterKernel (distance, strength);
	}
	else { axisWeight[0] = vec3 (1.0) - relative; axisWeight[1] = relative; }
#endif
	vec3 result = vec3 (0.0);
	float weightSum = 0.0;
	float totalWeight = 0.0;
	/* The directional basis and receiver normal do not change across the
	 * spatial taps. Compute the common four-bin quadrature only once. */
	float cosine2[4];
	if (c.meta.y == 2u)
		for (uint i = 0u; i < 4u; ++i)
			cosine2[i] = max (dot (normal, RC_Direction (uvec2 (i & 1u, i >> 1u), 2u)), 0.0);
	for (uint corner = 0u; corner <
#if RC_RECEIVER_TEXTURE_FILTER && !RC_RECEIVER_GAUSSIAN_FALLBACK
		8u;
#else
		(wide ? 125u : gaussian ? 27u : 8u);
#endif
		++corner)
	{
#if RC_RECEIVER_TEXTURE_FILTER && !RC_RECEIVER_GAUSSIAN_FALLBACK
		uvec3 delta = uvec3 (corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
#else
		uvec3 delta = wide ? uvec3 (corner % 5u, (corner / 5u) % 5u, corner / 25u) : gaussian ? uvec3 (corner % 3u, (corner / 3u) % 3u, corner / 9u) :
			uvec3 (corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
#endif
		float weight = axisWeight[delta.x].x * axisWeight[delta.y].y * axisWeight[delta.z].z;
		totalWeight += weight;
		if (weight <= 0.0) continue;
		uvec3 cell = uvec3 (clamp (center + ivec3 (delta)
#if !RC_RECEIVER_TEXTURE_FILTER || RC_RECEIVER_GAUSSIAN_FALLBACK
			- ivec3 (wide ? 2 : gaussian ? 1 : 0)
#endif
			, ivec3 (0), ivec3 (c.meta.x) - ivec3 (1)));
		/* All directions for one probe are contiguous. Resolve the sparse
		 * brick table only once instead of once per directional sample. */
		uint probeBase = RC_ReceiverProbeBase (c, cell);
		if (probeBase == RC_ABSENT) continue;
		float visibility = 1.0;
		vec3 separation = point - RC_ProbePosition (c, cell);
		float distance = length (separation);
		if (distance > 0.0001)
		{
			uvec2 visBin = RC_DirectionBinFromOffset (separation, distance, c.meta.y);
			uint visOffset = visBin.y * c.meta.y + visBin.x;
			uint visIndex = probeBase > 0xffffffffu - visOffset ? RC_ABSENT : probeBase + visOffset;
			/* Conservative distance bias permits a receiver lying on the sampled
			 * surface while rejecting probes separated by closer geometry. */
			if (visIndex == RC_ABSENT || visIndex >= rcMergedValid.length () || visIndex >= rcMergedDistance.length () ||
				visIndex >= rcRawValid.length () || visIndex >= rcRawDistance.length ()) continue;
			float visibleDistance;
			if (rcMergedValid[visIndex] != 0u) visibleDistance = rcMergedDistance[visIndex];
#if !RC_INDIRECT_BOUNCE
			/* Before far cascades converge, a traced near interval is enough
			 * to test visibility if it covers the whole probe/receiver segment. */
			else if (rcRawValid[visIndex] != 0u && distance <= c.interval.y) visibleDistance = rcRawDistance[visIndex];
#endif
			else continue;
			float bias = max (0.5, c.origin_spacing.w * 0.05);
			float transition = max (2.0, c.origin_spacing.w * 0.25);
			visibility = 1.0 - smoothstep (bias, bias + transition, distance - visibleDistance);
			if (visibility <= 0.0) continue;
		}
		vec3 light = vec3 (0.0);
		float cosineSum = 0.0;
		bool hasEvidence = false;
		for (uint y = 0u; y < c.meta.y; ++y)
			for (uint x = 0u; x < c.meta.y; ++x)
			{
				uvec2 bin = uvec2 (x, y);
				uint offset = y * c.meta.y + x;
				uint index = probeBase > 0xffffffffu - offset ? RC_ABSENT : probeBase + offset;
				float cosine;
				if (c.meta.y == 2u) cosine = cosine2[offset];
				else cosine = max (dot (normal, RC_Direction (bin, c.meta.y)), 0.0);
				cosineSum += cosine;
				if (index == RC_ABSENT || index >= rcMergedValid.length () || index >= rcMerged.length () ||
					index >= rcRawValid.length () || index >= rcRaw.length ()) continue;
				if (rcMergedValid[index] != 0u)
				{
					light += rcMerged[index].rgb * cosine;
					hasEvidence = true;
				}
#if !RC_INDIRECT_BOUNCE
				else if (rcRawValid[index] != 0u)
				{
					/* An opaque near hit already contains real reflected light;
					 * open intervals remain black until their far data arrives. */
					if (rcRaw[index].a <= 0.0) light += rcRaw[index].rgb * cosine;
					hasEvidence = true;
				}
#endif
			}
		/* Unknown directions contribute zero, not invented ambient. All known
		 * directions retain their share of the complete cosine quadrature. */
		if (!hasEvidence || cosineSum <= 1e-6) continue;
		/* Normalize the discrete cosine quadrature to unit response for an
		 * isotropic field. This keeps diffuse feedback energy bounded even at
		 * very low direction counts; dense sampling approaches the usual PI. */
		result += (light / cosineSum) * (weight * visibility);
		weightSum += weight * visibility;
	}
	/* Do not renormalize away occluded or unavailable corners: that turns a
	 * single surviving probe into a full-strength circle with a hard edge. */
	irradiance = weightSum > 0.0 && totalWeight > 0.0 ? result / totalWeight : vec3 (0.0);
	return weightSum > 0.0;
}

/* Probe transport retains the existing eight-corner interpolation. */
bool RC_SampleDiffuse (vec3 point, vec3 normal, out vec3 irradiance)
{
	return RC_SampleDiffuseMode (point, normal, 0.0, irradiance);
}
#endif
