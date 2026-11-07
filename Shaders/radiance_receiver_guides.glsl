#ifndef RC_RECEIVER_GUIDES_GLSL
#define RC_RECEIVER_GUIDES_GLSL
#include "radiance_receiver_scale.glsl"
#ifndef RC_RECEIVER_REPRESENTATIVE
#define RC_RECEIVER_REPRESENTATIVE(p) ivec2(0)
#endif
/* Geometry-only guides for the approximate texture-filtered receiver path.
 * A valid black sample is data; missing evidence is never an ambient value. */
bool RC_ReceiverFinite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}
float RC_ReceiverMaxDistance()
{
    float spacing=rcLevels[0].origin_spacing.w;
    return isnan(spacing) || isinf(spacing) || spacing<=0.0 ? 0.0 : 2.0*spacing;
}
bool RC_ReceiverGuide(ivec2 p,ivec2 extent,out uvec2 id,out vec3 position,out vec3 normal)
{
    id=uvec2(0); position=vec3(0); normal=vec3(0);
    if(any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,extent)) || RC_ResolveStatus(p)!=1u) return false;
    id=RC_ResolveID(rcReceiver.viewport.xy+RC_ReceiverFirstPixel(p,rcReceiver.normalThreshold.z)+RC_RECEIVER_REPRESENTATIVE(p));
    position=RC_ResolvePosition(p); normal=RC_ResolveNormal(p);
    return !any(equal(id,uvec2(0))) && RC_ReceiverFinite(position) && RC_ReceiverFinite(normal) && dot(normal,normal)>0.5;
}
/* Adjacent triangles of a curved surface rarely share a plane. Normals within
 * the angle limit bound how fast such a surface can rise, so allow that slope
 * over the tap distance. A real step rises faster and stays rejected. */
bool RC_ReceiverSmooth(vec3 position,vec3 normal,vec3 otherPosition,vec3 otherNormal)
{
    vec3 delta=otherPosition-position;
    float cosine=rcReceiver.normalThreshold.x;
    float allowance=rcReceiver.normalThreshold.y+length(delta)*sqrt(max(1.0-cosine*cosine,0.0));
    return dot(normal,otherNormal)>=cosine && abs(dot(delta,normal))<=allowance && abs(dot(delta,otherNormal))<=allowance;
}
bool RC_ReceiverGeometry(uvec2 id,vec3 position,vec3 normal,uvec2 otherID,vec3 otherPosition,vec3 otherNormal)
{
    vec3 delta=otherPosition-position;
    float limit=RC_ReceiverMaxDistance();
    /* An exact native triangle is planar even when its interpolated smooth
     * normal tilts away from that plane. Other primitives use the slope bound. */
    bool continuous=all(equal(id,otherID)) ? dot(normal,otherNormal)>=rcReceiver.normalThreshold.x :
        RC_ReceiverSmooth(position,normal,otherPosition,otherNormal);
    return !any(equal(id,uvec2(0))) && !any(equal(otherID,uvec2(0))) &&
        RC_ReceiverFinite(position) && RC_ReceiverFinite(normal) &&
        RC_ReceiverFinite(otherPosition) && RC_ReceiverFinite(otherNormal) && continuous &&
        limit>0.0 && dot(delta,delta)<=limit*limit;
}
/* RC_ReceiverGeometry as a weight. A wide upscale kernel on a curved surface
 * crosses the accept/reject boundary at every texel, so a hard test stipples;
 * fading to zero at the same limits keeps the result continuous. */
float RC_ReceiverGeometryWeight(uvec2 id,vec3 position,vec3 normal,uvec2 otherID,vec3 otherPosition,vec3 otherNormal)
{
    vec3 delta=otherPosition-position;
    float limit=RC_ReceiverMaxDistance(),distance=length(delta),cosine=rcReceiver.normalThreshold.x;
    if(any(equal(id,uvec2(0))) || any(equal(otherID,uvec2(0))) || limit<=0.0) return 0.0;
    float weight=(1.0-smoothstep(limit*.75,limit,distance))*smoothstep(cosine,mix(cosine,1.0,.5),dot(normal,otherNormal));
    if(all(equal(id,otherID))) return weight;
    float allowance=rcReceiver.normalThreshold.y+distance*sqrt(max(1.0-cosine*cosine,0.0));
    return weight*(1.0-smoothstep(allowance*.5,allowance,max(abs(dot(delta,normal)),abs(dot(delta,otherNormal)))));
}
bool RC_ReceiverGuidesCompatible(ivec2 dst,ivec2 src,ivec2 extent)
{
    uvec2 a,b; vec3 p,q,n,m;
    return RC_ReceiverGuide(dst,extent,a,p,n) && RC_ReceiverGuide(src,extent,b,q,m) && RC_ReceiverGeometry(a,p,n,b,q,m);
}
#endif
