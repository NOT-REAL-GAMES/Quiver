#ifndef RC_RECEIVER_GUIDES_GLSL
#define RC_RECEIVER_GUIDES_GLSL
#include "radiance_receiver_scale.glsl"
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
    id=RC_ResolveID(rcReceiver.viewport.xy+RC_ReceiverFirstPixel(p,rcReceiver.normalThreshold.z));
    position=RC_ResolvePosition(p); normal=RC_ResolveNormal(p);
    return !any(equal(id,uvec2(0))) && RC_ReceiverFinite(position) && RC_ReceiverFinite(normal) && dot(normal,normal)>0.5;
}
/* Neighbor primitives may differ across the same continuous wall. Exact
 * prepass/forward identity parity is still checked at the shaded fragment. */
bool RC_ReceiverContinuous(vec3 position,vec3 normal,vec3 otherPosition,vec3 otherNormal)
{
    vec3 delta=otherPosition-position;
    return dot(normal,otherNormal)>=rcReceiver.normalThreshold.x &&
        abs(dot(delta,normal))<=rcReceiver.normalThreshold.y &&
        abs(dot(delta,otherNormal))<=rcReceiver.normalThreshold.y;
}
bool RC_ReceiverGeometry(uvec2 id,vec3 position,vec3 normal,uvec2 otherID,vec3 otherPosition,vec3 otherNormal)
{
    vec3 delta=otherPosition-position;
    float limit=RC_ReceiverMaxDistance();
    /* An exact native triangle is planar even when its interpolated smooth
     * normal tilts away from that plane. Other primitives retain strict plane
     * checks; the axis filter has no identity evidence and remains unchanged. */
    bool continuous=all(equal(id,otherID)) ? dot(normal,otherNormal)>=rcReceiver.normalThreshold.x :
        RC_ReceiverContinuous(position,normal,otherPosition,otherNormal);
    return !any(equal(id,uvec2(0))) && !any(equal(otherID,uvec2(0))) &&
        RC_ReceiverFinite(position) && RC_ReceiverFinite(normal) &&
        RC_ReceiverFinite(otherPosition) && RC_ReceiverFinite(otherNormal) && continuous &&
        limit>0.0 && dot(delta,delta)<=limit*limit;
}
bool RC_ReceiverGuidesCompatible(ivec2 dst,ivec2 src,ivec2 extent)
{
    uvec2 a,b; vec3 p,q,n,m;
    return RC_ReceiverGuide(dst,extent,a,p,n) && RC_ReceiverGuide(src,extent,b,q,m) && RC_ReceiverGeometry(a,p,n,b,q,m);
}
#endif
