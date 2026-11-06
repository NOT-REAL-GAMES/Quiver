#ifndef RC_RECEIVER_SHADOW_SOURCE_GLSL
#define RC_RECEIVER_SHADOW_SOURCE_GLSL
/* Query-free eligibility. Fractional quality>=1 emissive visibility cannot
 * be encoded in one centre bit and already has its existing four-ray path. */
bool RC_ReceiverSoftEligible(RCLight light)
{
    if(light.color_intensity.w<=0.0 || RC_IsLocalMinLight(light)) return false;
    if(light.metadata.x==RC_LIGHT_DIRECTIONAL) return rcLighting.shadow_parameters.z>0.0;
    if(light.metadata.x==RC_LIGHT_POINT || light.metadata.x==RC_LIGHT_SPOT) return rcLighting.shadow_parameters.y>0.0;
    if(light.metadata.x!=RC_LIGHT_EMISSIVE || rcLighting.bounce_parameters.y<0.5) return false;
    vec3 axis,tangent,bitangent;
    return RC_EmissiveBasis(light,axis,tangent,bitangent);
}
void RC_ReceiverShadowBasis(vec3 direction,out vec3 tangent,out vec3 bitangent)
{
    vec3 axis=abs(direction.z)<.999?vec3(0,0,1):vec3(0,1,0);
    tangent=normalize(cross(axis,direction)); bitangent=cross(direction,tangent);
}
#endif
