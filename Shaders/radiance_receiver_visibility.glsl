#ifndef QUIVER_RECEIVER_VISIBILITY_GLSL
#define QUIVER_RECEIVER_VISIBILITY_GLSL
/* Experimental conservative probe-visibility compatibility test. This is
 * expensive and is not proof that interpolated irradiance equals a full-res
 * sample. Include after radiance_lookup.glsl. */
bool RC_ReceiverVisibilityCompatible(vec3 p, vec3 q, float strength)
{
	RCCascade c = rcLevels[0];
	if (c.meta.x == 0u || c.meta.y == 0u || c.origin_spacing.w <= 0.0) return false;
	bool gaussian = strength > 0.0, wide = strength > 1.0;
	vec3 pc = (p - c.origin_spacing.xyz) / c.origin_spacing.w - vec3(0.5);
	vec3 qc = (q - c.origin_spacing.xyz) / c.origin_spacing.w - vec3(0.5);
	if (any(lessThan(pc, vec3(-0.5))) || any(lessThan(qc, vec3(-0.5))) ||
		any(greaterThan(pc, vec3(float(c.meta.x) - 0.5))) ||
		any(greaterThan(qc, vec3(float(c.meta.x) - 0.5)))) return false;
	ivec3 center = ivec3(floor(pc + vec3(gaussian ? 0.5 : 0.0)));
	if (any(notEqual(center, ivec3(floor(qc + vec3(gaussian ? 0.5 : 0.0)))))) return false;
	uint side = wide ? 5u : gaussian ? 3u : 2u;
	int offset = wide ? 2 : gaussian ? 1 : 0;
	for (uint i = 0u; i < side * side * side; ++i)
	{
		ivec3 delta = ivec3(i % side, (i / side) % side, i / (side * side));
		uvec3 cell = uvec3(clamp(center + delta - ivec3(offset), ivec3(0), ivec3(c.meta.x) - ivec3(1)));
		uint base = RC_RecordIndex(c, cell, uvec2(0));
		if (base == RC_ABSENT) continue;
		vec3 ps = p - RC_ProbePosition(c, cell), qs = q - RC_ProbePosition(c, cell);
		float pd = length(ps), qd = length(qs);
		if (pd <= 0.0001 || qd <= 0.0001) return false;
		uvec2 pb = RC_DirectionBin(ps / pd, c.meta.y), qb = RC_DirectionBin(qs / qd, c.meta.y);
		if (any(notEqual(pb, qb))) return false;
		uint binOffset = pb.y * c.meta.y + pb.x;
		if (base > 0xffffffffu - binOffset) return false;
		uint index = base + binOffset;
		if (index >= rcMergedValid.length() || index >= rcMergedDistance.length() ||
			index >= rcRawValid.length() || index >= rcRawDistance.length()) return false;
		float visibleDistance;
		if (rcMergedValid[index] != 0u) visibleDistance = rcMergedDistance[index];
#if !RC_INDIRECT_BOUNCE
		else if (rcRawValid[index] != 0u && pd <= c.interval.y && qd <= c.interval.y)
			visibleDistance = rcRawDistance[index];
#endif
		else return false;
		if (isnan(visibleDistance) || isinf(visibleDistance)) return false;
		float bias = max(0.5, c.origin_spacing.w * 0.05);
		float transition = max(2.0, c.origin_spacing.w * 0.25);
		float pv = 1.0 - smoothstep(bias, bias + transition, pd - visibleDistance);
		float qv = 1.0 - smoothstep(bias, bias + transition, qd - visibleDistance);
		if (abs(pv - qv) > 0.01) return false;
	}
	return true;
}
/* Test up to four neighbors against the same receiver. Probe indexing and
 * receiver-side visibility are shared; each neighbor is still checked
 * against exactly the same bin, distance and convergence rules. */
bool RC_ReceiverVisibilityCompatibleAll(vec3 p, vec3 q[4], int count, float strength)
{
    if (count < 1 || count > 4) return false;
    RCCascade c = rcLevels[0];
    if (c.meta.x == 0u || c.meta.y == 0u || c.origin_spacing.w <= 0.0) return false;
    bool gaussian = strength > 0.0, wide = strength > 1.0;
    vec3 pc = (p - c.origin_spacing.xyz) / c.origin_spacing.w - vec3(0.5);
    if (any(lessThan(pc,vec3(-0.5))) || any(greaterThan(pc,vec3(float(c.meta.x)-0.5)))) return false;
    ivec3 center = ivec3(floor(pc + vec3(gaussian ? 0.5 : 0.0)));
    for (int j=0; j<count; ++j) {
        vec3 qc = (q[j] - c.origin_spacing.xyz) / c.origin_spacing.w - vec3(0.5);
        if (any(lessThan(qc,vec3(-0.5))) || any(greaterThan(qc,vec3(float(c.meta.x)-0.5))) ||
            any(notEqual(center,ivec3(floor(qc + vec3(gaussian ? 0.5 : 0.0)))))) return false;
    }
    uint side = wide ? 5u : gaussian ? 3u : 2u;
    int offset = wide ? 2 : gaussian ? 1 : 0;
    float bias = max(0.5,c.origin_spacing.w*0.05);
    float transition = max(2.0,c.origin_spacing.w*0.25);
    /* Bounds shared by every probe. They allow a cheap proof that every
     * neighbor lies in the same directional bin and fully visible plateau. */
    vec3 maxDelta=vec3(0.0);
    float maxDistance=0.0;
    for (int j=0; j<count; ++j) {
        vec3 d=q[j]-p;
        maxDelta=max(maxDelta,abs(d));
        maxDistance=max(maxDistance,length(d));
    }
    /* Visit taps in the original x-fastest order without decoding a flattened
     * index. A rejected/fully-visible tap continues only the innermost loop. */
    for (uint z=0u; z<side; ++z) for (uint y=0u; y<side; ++y) for (uint x=0u; x<side; ++x) {
        ivec3 delta=ivec3(x,y,z);
        uvec3 cell=uvec3(clamp(center+delta-ivec3(offset),ivec3(0),ivec3(c.meta.x)-ivec3(1)));
        uint base=RC_ReceiverProbeBase(c,cell);
        if (base==RC_ABSENT) continue;
        vec3 probe=RC_ProbePosition(c,cell), ps=p-probe;
        float pd=length(ps);
        if (pd<=0.0001) return false;
        uvec2 pb=RC_DirectionBinFromOffset(ps,pd,c.meta.y);
        uint binOffset=pb.y*c.meta.y+pb.x;
        if (base>0xffffffffu-binOffset) return false;
        uint index=base+binOffset;
        if (index>=rcMergedValid.length() || index>=rcMergedDistance.length() ||
            index>=rcRawValid.length() || index>=rcRawDistance.length()) return false;
        bool merged=rcMergedValid[index]!=0u;
        float visibleDistance;
        if (merged) visibleDistance=rcMergedDistance[index];
#if !RC_INDIRECT_BOUNCE
        else if (rcRawValid[index]!=0u && pd<=c.interval.y) visibleDistance=rcRawDistance[index];
#endif
        else return false;
        if (isnan(visibleDistance) || isinf(visibleDistance)) return false;
        /* A 2x2 bin is bounded by the y=0 and z=0 planes. If the
         * whole neighborhood stays well inside those planes and below the
         * visibility transition, all per-neighbor comparisons are zero. */
        float margin=max(0.001,(pd+maxDistance)*0.00002);
        float farthest=pd+maxDistance+margin;
        bool plateau=c.meta.y==2u && pd-maxDistance>0.0001+margin &&
            abs(ps.y)>maxDelta.y+margin && abs(ps.z)>maxDelta.z+margin &&
            farthest<=visibleDistance+bias;
#if !RC_INDIRECT_BOUNCE
        plateau=plateau && (merged || farthest<=c.interval.y);
#endif
        if (plateau) continue;
        float pv=1.0-smoothstep(bias,bias+transition,pd-visibleDistance);
        for (int j=0; j<count; ++j) {
            vec3 qs=q[j]-probe;
            float qd=length(qs);
            if (qd<=0.0001 || any(notEqual(pb,RC_DirectionBinFromOffset(qs,qd,c.meta.y)))) return false;
#if !RC_INDIRECT_BOUNCE
            if (!merged && qd>c.interval.y) return false;
#endif
            float qv=1.0-smoothstep(bias,bias+transition,qd-visibleDistance);
            if (abs(pv-qv)>0.01) return false;
        }
    }
    return true;
}
#endif
