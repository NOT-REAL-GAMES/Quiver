#ifndef RC_RECEIVER_SHADOW_METADATA_GLSL
#define RC_RECEIVER_SHADOW_METADATA_GLSL
/* Small per-invocation export state. Selection never performs a scene query;
 * its centre bit comes from the already evaluated positive hard RGB. */
uint rcShadowSource,rcShadowCenter;
float rcShadowBest,rcShadowSecond;
void RC_ReceiverShadowReset()
{
    rcShadowSource=0u; rcShadowCenter=0u; rcShadowBest=0.0; rcShadowSecond=0.0;
}
void RC_ReceiverShadowSelect(uint index,RCLight light,vec3 position,vec3 normal,vec3 hardRGB)
{
    if(rcLighting.shadow_parameters.x<=0.5 || index>=0x0fffffffu || !RC_ReceiverSoftEligible(light)) return;
    vec3 direction; float distance;
    vec3 unshadowed=RC_EvaluateUnshadowedLight(light,position,normal,direction,distance);
    float score=dot(unshadowed,vec3(.2126,.7152,.0722));
    if(isnan(score) || isinf(score) || score<=0.0) return;
    if(score>rcShadowBest) {
        rcShadowSecond=rcShadowBest; rcShadowBest=score; rcShadowSource=index+1u;
        rcShadowCenter=any(greaterThan(hardRGB,vec3(0)))?8u:0u;
    } else rcShadowSecond=max(rcShadowSecond,score);
}
uint RC_ReceiverShadowMetadata() { return (rcShadowSource<<4u)|rcShadowCenter; }
float RC_ReceiverShadowDominance()
{
    return rcShadowSource==0u?0.0:smoothstep(0.0,.25,(rcShadowBest-rcShadowSecond)/max(rcShadowBest,1e-20));
}
#endif
