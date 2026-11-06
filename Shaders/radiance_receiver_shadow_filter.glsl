#ifndef RC_RECEIVER_SHADOW_FILTER_GLSL
#define RC_RECEIVER_SHADOW_FILTER_GLSL
#include "radiance_filter.glsl"
bool RC_ShadowFilterCompatible(uvec2 id,vec3 position,vec3 normal,ivec2 tap,ivec2 extent)
{
    uvec2 otherID; vec3 otherPosition,otherNormal;
    return RC_ReceiverGuide(tap,extent,otherID,otherPosition,otherNormal) &&
        RC_ReceiverGeometry(id,position,normal,otherID,otherPosition,otherNormal);
}
/* Spatially filter one selected source's visibility, never combined RGB.
 * Other lights, subtractive contributions and local minima retain their
 * original values. The caller reconstructs checker holes from these actual
 * scheduled samples; unknown cells never become visibility evidence. */
bool RC_ShadowFilterDirect(ivec2 pixel,ivec2 extent,out vec3 value)
{
    vec4 center=RC_ResolveValue(pixel,0u);
    value=center.rgb;
    if(!(center.a>0.5) || !RC_ReceiverFinite(value) || any(greaterThan(abs(value),vec3(65504)))) return false;
    float strength=clamp(rcLighting.shadow_parameters.w,0.0,2.0);
    if(rcLighting.shadow_parameters.x<=0.5 || strength<=0.0 ||
        isnan(center.a) || isinf(center.a) || center.a<2.0 || center.a>3.0) return true;
    uint source=RC_ResolvePackedStatus(pixel)>>4u;
    if(source==0u || source-1u>=rcLighting.counts.x) return true;
    int stride=rcLighting.receiver_filter.y>0.5?2:1;
    /* Most neighborhoods are completely lit or have constant fractional
     * visibility. Inspect cheap tags first: their correction is exactly zero
     * regardless of which geometry tests would subsequently reject a tap. */
    float differences[9]; uint candidates=0u; bool changed=false;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        if(x==0 && y==0) continue;
        ivec2 tap=pixel+ivec2(x,y)*stride;
        if(any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,extent))) continue;
        uint packed=RC_ResolvePackedStatus(tap);
        if((packed&3u)!=1u || (packed>>4u)!=source) continue;
        vec4 sampleValue=RC_ResolveValue(tap,0u);
        if(isnan(sampleValue.a) || isinf(sampleValue.a) || sampleValue.a<2.0 || sampleValue.a>3.0 ||
            !RC_ReceiverFinite(sampleValue.rgb) || any(greaterThan(abs(sampleValue.rgb),vec3(65504)))) continue;
        uint index=uint((y+1)*3+x+1);
        differences[index]=sampleValue.a-center.a;
        candidates|=1u<<index;
        changed=changed || differences[index]!=0.0;
    }
    if(!changed) return true;
    uvec2 id; vec3 position,normal;
    if(!RC_ReceiverGuide(pixel,extent,id,position,normal)) return true;
    float neighborWeight=RC_FilterKernel(vec3(1),strength).x;
    float difference=0.0,total=1.0; /* Exact center, kernel(0)==1. */
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        uint index=uint((y+1)*3+x+1);
        if((candidates&(1u<<index))==0u) continue;
        ivec2 tap=pixel+ivec2(x,y)*stride;
        if(!RC_ShadowFilterCompatible(id,position,normal,tap,extent)) continue;
        /* A checker tap spans an unscheduled cell. Do not jump a visible
         * geometry/coverage barrier merely because the endpoints agree. */
        if(stride==2 && !RC_ShadowFilterCompatible(id,position,normal,pixel+ivec2(x,y),extent)) continue;
        float weight=(x==0?1.0:neighborWeight)*(y==0?1.0:neighborWeight);
        difference+=differences[index]*weight; total+=weight;
    }
    difference/=total;
    if(strength<1.0) difference*=strength;
    if(difference==0.0) return true;
    vec3 direction; float distance;
    vec3 unshadowed=RC_EvaluateUnshadowedLight(rcLights[source-1u],position,normal,direction,distance);
    if(!RC_ReceiverFinite(unshadowed) || any(greaterThan(abs(unshadowed),vec3(65504)))) return true;
    vec3 corrected=value+unshadowed*(difference*.3183098861837907);
    if(RC_ReceiverFinite(corrected) && !any(greaterThan(abs(corrected),vec3(65504)))) value=corrected;
    return true;
}
#endif
