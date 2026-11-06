#ifndef RC_RECEIVER_TEXTURE_FILTER_GLSL
#define RC_RECEIVER_TEXTURE_FILTER_GLSL
#include "radiance_filter.glsl"
bool RC_TextureRaw(ivec2 p,uint field,out vec3 value)
{
    vec4 sampleValue=RC_ResolveValue(p,field);
    value=sampleValue.rgb;
    return sampleValue.a>0.5 && RC_ReceiverFinite(value) && !any(greaterThan(abs(value),vec3(65504.0)));
}
bool RC_TextureIndirectScheduled(ivec2 p)
{
    return rcLighting.receiver_filter.y<=0.5 || RC_CheckerHasSample(p,1u);
}
/* Gaussian zero retains the original four-cardinal checker reconstruction.
 * This is also the fractional-strength baseline at an unscheduled hole. */
bool RC_TextureFilterIndirect(ivec2 pixel,ivec2 extent,out vec3 value)
{
    value=vec3(0);
    uvec2 id; vec3 position,normal;
    if(!RC_ReceiverGuide(pixel,extent,id,position,normal)) return false;
    if(RC_TextureIndirectScheduled(pixel)) return RC_TextureRaw(pixel,1u,value);
    const ivec2 offsets[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    float count=0.0;
    for(int i=0;i<4;++i) {
        ivec2 tap=pixel+offsets[i]; vec3 raw;
        if(!RC_ReceiverGuidesCompatible(pixel,tap,extent) || !RC_TextureRaw(tap,1u,raw)) continue;
        value+=raw; count+=1.0;
    }
    if(count<=0.0) return false;
    value/=count;
    return RC_ReceiverFinite(value) && !any(greaterThan(abs(value),vec3(65504.0)));
}
#endif
