#ifndef RC_RECEIVER_SHADOWS_GLSL
#define RC_RECEIVER_SHADOWS_GLSL
/* Optional post only: consumes the exported centre visibility and adds at
 * most three outer queries. No all-light loop or extra centre query. */
float RC_ReceiverSoftVisibility(RCLight light,vec3 position,vec3 normal,vec3 direction,float distance,
    float center,float dominance)
{
    if(dominance<=0.0) return center;
    vec3 tangent,bitangent,emitterAxis;
    bool sun=light.metadata.x==RC_LIGHT_DIRECTIONAL,area=light.metadata.x==RC_LIGHT_EMISSIVE;
    float radius=sun?tan(rcLighting.shadow_parameters.z):rcLighting.shadow_parameters.y;
    vec3 source=light.position_radius.xyz;
    if(area) {
        if(!RC_EmissiveBasis(light,emitterAxis,tangent,bitangent)) return center;
        float facing=dot(position-source,emitterAxis)>=0.0?1.0:-1.0;
        source+=emitterAxis*(.1*facing);
    } else {
        if(radius<=0.0) return center;
        RC_ReceiverShadowBasis(direction,tangent,bitangent);
    }
    /* Centre plus an equilateral outer triangle: balanced, deterministic,
     * isotropic second moment. Disk radius sqrt(2/3), rectangle sqrt(8/9). */
    const vec2 points[3]=vec2[3](vec2(1,0),vec2(-.5,.8660254037844386),vec2(-.5,-.8660254037844386));
    float visible=center;
    for(int i=0;i<3;++i) {
        vec2 point=points[i]*(area?.9428090415820634:.816496580927726);
        vec3 offset=area?tangent*(point.x*light.area_axis.w)+bitangent*(point.y*light.direction_cone.w):
            (tangent*point.x+bitangent*point.y)*radius;
        vec3 rayDirection; float rayDistance;
        if(sun) { rayDirection=normalize(direction+offset); rayDistance=distance; }
        else {
            vec3 delta=source+offset-position;
            rayDistance=length(delta);
            if(rayDistance<=1e-5) { visible+=center; continue; }
            rayDirection=delta/rayDistance;
        }
        visible+=float(RC_LightVisible(position,normal,rayDirection,rayDistance,sun));
    }
    return mix(center,visible*.25,dominance);
}
bool RC_ReceiverShadowFinite16(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value)) && !any(greaterThan(abs(value),vec3(65504)));
}
/* Alpha1 is an ordinary valid direct sample. Alpha2..3 additionally exports
 * this source's final visibility for the query-free spatial postprocess.
 * Unscheduled checker cells keep alpha0, including their original RGB. */
vec4 RC_ReceiverSoftResult(uint packed,vec4 hard,vec3 position,vec3 normal)
{
    if(hard.a<=0.5) return hard;
    vec3 result=hard.rgb;
    float alpha=1.0,dominance=clamp(hard.a-1.0,0.0,1.0);
    uint source=packed>>4u;
    if(rcLighting.shadow_parameters.x>0.5 && dominance>0.0 && (packed&3u)==1u && source>0u && source-1u<rcLighting.counts.x) {
        RCLight light=rcLights[source-1u];
        if(RC_ReceiverSoftEligible(light) && !any(isnan(position)) && !any(isinf(position)) &&
            !any(isnan(normal)) && !any(isinf(normal)) && dot(normal,normal)>.5) {
            vec3 direction; float distance;
            vec3 unshadowed=RC_EvaluateUnshadowedLight(light,position,normal,direction,distance);
            if(RC_ReceiverShadowFinite16(unshadowed) && any(greaterThan(unshadowed,vec3(0)))) {
                float center=(packed&8u)!=0u?1.0:0.0;
                float visibility=RC_ReceiverSoftVisibility(light,position,normal,direction,distance,center,dominance);
                vec3 corrected=hard.rgb+unshadowed*((visibility-center)*.3183098861837907);
                if(RC_ReceiverShadowFinite16(corrected)) {
                    result=corrected;
                    if(rcLighting.shadow_parameters.w>0.0) alpha=2.0+visibility;
                }
            }
        }
    }
    return vec4(result,alpha);
}
#endif
