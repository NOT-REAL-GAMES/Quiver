#ifndef RADIANCE_RECEIVER_LIGHT_COMMON_GLSL
#define RADIANCE_RECEIVER_LIGHT_COMMON_GLSL
/* Shared by the gather and miss passes of the full-resolution lighting stage.
 * Alpha in the output images: 0 no result, 1 reused, 2 evaluated by the miss pass. */
layout(set=0,binding=7) uniform sampler2D fullDepth;
layout(set=0,binding=8) uniform sampler2D fullNormal;
layout(rgba16f,set=0,binding=9) uniform writeonly image2D lightIndirect;
layout(rgba16f,set=0,binding=10) uniform writeonly image2D lightDirect;
layout(rgba16f,set=0,binding=11) uniform writeonly image2D lightMinimum;
layout(std430,set=0,binding=12) buffer RCLightMisses { uvec3 dispatchGroups; uint missCount; uint misses[]; };
layout(push_constant) uniform RCLightParams {
    mat4 inverseViewProjection;
    ivec4 viewport;
    vec4 ndcTransform;
    vec4 normalThreshold; /* cosine, plane tolerance, divisor, upscale extent */
} params;
const uint RC_LIGHT_MISS_INDIRECT=1u<<30,RC_LIGHT_MISS_DIRECT=1u<<31;
const int RC_LIGHT_MISS_GROUP=64;
bool RC_LightFinite(vec3 value) { return !any(isnan(value)) && !any(isinf(value)); }
/* The visible surface at a pixel, exactly as the classifier reconstructs it. */
bool RC_LightSurface(ivec2 pixel,out vec3 position,out vec3 normal)
{
    float depth=texelFetch(fullDepth,pixel,0).r;
    normal=texelFetch(fullNormal,pixel,0).xyz;
    vec2 ndc=(vec2(pixel)+.5)*params.ndcTransform.xy+params.ndcTransform.zw;
    vec4 world=params.inverseViewProjection*vec4(ndc,depth,1);
    position=vec3(0);
    if(isnan(depth) || isinf(depth) || !RC_LightFinite(normal) || dot(normal,normal)<1e-16 ||
        any(isnan(world)) || any(isinf(world)) || abs(world.w)<1e-8) return false;
    position=world.xyz/world.w;
    normal=normalize(normal);
    return RC_LightFinite(position);
}
#endif
