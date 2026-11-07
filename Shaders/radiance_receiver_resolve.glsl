#ifndef RADIANCE_RECEIVER_RESOLVE_GLSL
#define RADIANCE_RECEIVER_RESOLVE_GLSL
#include "radiance_receiver_scale.glsl"
/* A shader with the lighting parameters defines this as the scene's flag, so the
 * field goes unread where no light can write it. */
#ifndef RC_RECEIVER_MINIMUM_LIGHTS
#define RC_RECEIVER_MINIMUM_LIGHTS true
#endif
/* Records which lighting terms missed the receiver, for r_radiance_debug_fallback.
 * Bit 0: indirect, bit 1: direct. Always true, so it can sit in a condition. */
uint rcFallbackMask=0u;
bool RC_NoteFallback(uint term) { rcFallbackMask|=term; return true; }
#ifndef RC_RECEIVER_RESOLVE_TEST
#if !RC_RECEIVER_TEXTURE_FILTER
#include "radiance_receiver_visibility.glsl"
#endif
/* Single-sample opaque world/model receivers. Full geometry, ID, depth and draw parity
 * are required; unsupported cases use the original forward path. */
#ifndef RC_RECEIVER_SET
#define RC_RECEIVER_SET 6
#endif
layout(set = RC_RECEIVER_SET, binding = 0) uniform sampler2D rcHalfIndirect;
layout(set = RC_RECEIVER_SET, binding = 1) uniform sampler2D rcHalfDirect;
layout(set = RC_RECEIVER_SET, binding = 2) uniform usampler2D rcHalfStatus;
layout(set = RC_RECEIVER_SET, binding = 3) uniform usampler2D rcFullReceiverID;
layout(set = RC_RECEIVER_SET, binding = 4) uniform sampler2D rcHalfNormal;
layout(set = RC_RECEIVER_SET, binding = 5) uniform sampler2D rcHalfPosition;
layout(set = RC_RECEIVER_SET, binding = 6) uniform sampler2D rcHalfMinimum;
#if RC_RECEIVER_MODEL_CULL
layout(set = RC_RECEIVER_SET, binding = 8) uniform sampler2D rcFullReceiverDepth;
float RC_ResolveDepth(ivec2 p) { return texelFetch(rcFullReceiverDepth,p,0).r; }
#endif
layout(std140, set = RC_RECEIVER_SET, binding = 7) uniform RCReceiverViewport {
    ivec4 viewport;
    vec4 normalThreshold; /* x: normal cosine, y: plane tolerance, z: actual divisor (1,1.5,2,3,4) */
} rcReceiver;
uvec2 RC_ResolveID(ivec2 p) { return texelFetch(rcFullReceiverID,p,0).rg; }
uint RC_ResolveStatus(ivec2 p) {
#if RC_RECEIVER_TEXTURE_FILTER
    return texelFetch(rcHalfStatus,p,0).r&3u;
#else
    return texelFetch(rcHalfStatus,p,0).r;
#endif
}
vec3 RC_ResolvePosition(ivec2 p) { return texelFetch(rcHalfPosition,p,0).xyz; }
vec3 RC_ResolveNormal(ivec2 p) { return texelFetch(rcHalfNormal,p,0).xyz; }
vec4 RC_ResolveValue(ivec2 p, uint field) {
    if(field==1u) return texelFetch(rcHalfIndirect,p,0);
    if(field==2u) return texelFetch(rcHalfMinimum,p,0);
    return texelFetch(rcHalfDirect,p,0);
}
#endif
#if RC_RECEIVER_MODEL_CULL
/* Reversed depth: larger means nearer. Only the deferred opaque model path
 * may reject hidden fragments; unknown and near-equal evidence fails open. */
bool RC_ReceiverOccluded(ivec2 pixel,uvec2 id,float fragmentDepth)
{
    /* Receiver depth is one sample per pixel. Under MSAA a fragment can still
     * cover other samples, so culling here would cut holes in silhouettes. */
    if((rcLighting.counts.w&32u)!=0u) return false;
    ivec2 localPixel=pixel-rcReceiver.viewport.xy;
    if(any(lessThan(localPixel,ivec2(0))) || any(greaterThanEqual(localPixel,rcReceiver.viewport.zw)) ||
        any(equal(id,uvec2(0))) || isnan(fragmentDepth) || isinf(fragmentDepth) || fragmentDepth<0.0 || fragmentDepth>1.0) return false;
    uvec2 front=RC_ResolveID(pixel);
    if(any(equal(front,uvec2(0))) || all(equal(front,id))) return false;
    float depth=RC_ResolveDepth(pixel);
    if(isnan(depth) || isinf(depth) || depth<0.0 || depth>1.0) return false;
    float margin=max(1e-7,max(abs(depth),abs(fragmentDepth))*1e-6);
    return depth>fragmentDepth+margin;
}
#endif
#if RC_RECEIVER_TEXTURE_FILTER
#ifndef RC_RECEIVER_RESOLVE_TEST
ivec2 RC_ReceiverRepresentative(float packed) { int offset=max(int(packed)-1,0); return ivec2(offset&15,offset>>4); }
#define RC_RECEIVER_REPRESENTATIVE(p) RC_ReceiverRepresentative(texelFetch(rcHalfPosition,p,0).w)
#endif
#include "radiance_receiver_guides.glsl"
#endif
/* Mode 0: full half-resolution fields; mode 1: opposite parity fields.
 * Samples must come from the same primitive, plane, and normal; bounce also
 * uses the existing strict probe visibility compatibility test. */
bool RC_CheckerFetch(ivec2 p, uint field, out vec3 value) {
    if (RC_ResolveStatus(p)!=1u) return false;
    vec4 v=RC_ResolveValue(p,field); value=v.rgb; return v.a>0.5;
}
bool RC_CheckerCompatible(ivec2 dst, ivec2 src, uint field) {
#if RC_RECEIVER_TEXTURE_FILTER
    return RC_ReceiverGuidesCompatible(dst,src,RC_ReceiverExtent(rcReceiver.viewport.zw,rcReceiver.normalThreshold.z));
#else
    if(RC_ResolveStatus(dst)!=1u || RC_ResolveStatus(src)!=1u) return false;
    ivec2 a=rcReceiver.viewport.xy+RC_ReceiverFirstPixel(dst,rcReceiver.normalThreshold.z), b=rcReceiver.viewport.xy+RC_ReceiverFirstPixel(src,rcReceiver.normalThreshold.z);
    uvec2 id=RC_ResolveID(a);
    if(any(equal(id,uvec2(0))) || any(notEqual(id,RC_ResolveID(b)))) return false;
    vec3 p=RC_ResolvePosition(dst), q=RC_ResolvePosition(src);
    vec3 n=RC_ResolveNormal(dst), m=RC_ResolveNormal(src);
    if(any(isnan(p)) || any(isinf(p)) || any(isnan(q)) || any(isinf(q)) ||
       any(isnan(n)) || any(isinf(n)) || any(isnan(m)) || any(isinf(m)) ||
       dot(n,m)<rcReceiver.normalThreshold.x || abs(dot(q-p,n))>rcReceiver.normalThreshold.y) return false;
    return field!=1u || RC_ReceiverVisibilityCompatible(p,q,rcLighting.receiver_filter.x);
#endif
}
#include "radiance_receiver_checkerboard.glsl"
#if RC_RECEIVER_TEXTURE_FILTER
/* One texel of radius is bilinear. The upscale blur widens it, in receiver
 * texels, so its size on screen grows with the divisor. */
float RC_ReceiverUpscaleRadius()
{
    float radius=1.0;
#ifndef RC_RECEIVER_PLAIN_UPSCALE
    if(rcReceiver.normalThreshold.z>1.0) radius+=clamp(rcReceiver.normalThreshold.w,0.0,15.0);
#endif
    return radius;
}
bool RC_TextureReceiverTap(ivec2 tap,ivec2 extent,uvec2 id,vec3 position,vec3 normal)
{
    uvec2 otherID; vec3 otherPosition,otherNormal;
    return RC_ReceiverGuide(tap,extent,otherID,otherPosition,otherNormal) &&
        RC_ReceiverGeometry(id,position,normal,otherID,otherPosition,otherNormal);
}
#endif
bool RC_ReceiverField(ivec2 pixel, uvec2 id, vec3 position, vec3 normal, uint field, out vec3 result)
{
    result=vec3(0);
    ivec2 localPixel=pixel-rcReceiver.viewport.xy;
    if (any(lessThan(localPixel,ivec2(0))) || any(greaterThanEqual(localPixel,rcReceiver.viewport.zw)) ||
#if !RC_RECEIVER_TEXTURE_FILTER
        any(notEqual(RC_ResolveID(pixel),id)) ||
#endif
        any(equal(id,uvec2(0))) ||
        any(isnan(position)) || any(isinf(position)) || any(isnan(normal)) || any(isinf(normal))) return false;
    vec2 coord=RC_ReceiverSourceCoord(localPixel,rcReceiver.normalThreshold.z);
    ivec2 base=ivec2(floor(coord)); vec2 f=fract(coord);
    ivec2 extent=RC_ReceiverExtent(rcReceiver.viewport.zw,rcReceiver.normalThreshold.z);
#if RC_RECEIVER_TEXTURE_FILTER
    /* A rejected guide removes that tap, rather than forcing every fragment
     * near an edge through a second probe lookup. No probe visibility scan. */
    float total=0.0;
    float radius=RC_ReceiverUpscaleRadius();
    /* At most six taps per axis. Wider kernels use a fixed coarser lattice, so
     * a tap only enters or leaves at the kernel edge, where its weight is zero. */
    /* Probes can leave GI texels empty deep in folds. Search wider before
     * leaving the filtered path, whose result looks different. */
    for(int attempt=0;attempt<(field==1u?2:1) && total<=0.0;++attempt) {
    if(attempt>0) radius=max(radius*2.0,4.0);
    int stride=max(int(ceil(radius/3.0)),1),reach=int(ceil(radius/float(stride)));
    ivec2 lattice=(base/stride)*stride;
    for(int y=1-reach;y<=reach;y++) for(int x=1-reach;x<=reach;x++) {
        ivec2 p=lattice+ivec2(x,y)*stride; vec3 value;
        vec2 falloff=max(vec2(1)-abs(vec2(p)-coord)/radius,vec2(0));
        float weight=falloff.x*falloff.y;
        if(weight<=1e-7) continue;
        if(radius>1.0) {
            uvec2 otherID; vec3 otherPosition,otherNormal;
            if(!RC_ReceiverGuide(p,extent,otherID,otherPosition,otherNormal)) continue;
            weight*=RC_ReceiverGeometryWeight(id,position,normal,otherID,otherPosition,otherNormal);
            if(weight<=1e-7) continue;
        }
        else if(!RC_TextureReceiverTap(p,extent,id,position,normal)) continue;
        if(!RC_CheckerFetch(p,field,value) || !RC_ReceiverFinite(value) || any(greaterThan(abs(value),vec3(65504.0)))) continue;
        result+=weight*value; total+=weight;
    }
    }
    if(total<=0.0) { result=vec3(0); return false; }
    result/=total;
#else
    /* Reject incomplete neighborhoods before evaluating probe visibility. */
    for(int y=0;y<2;y++) for(int x=0;x<2;x++) {
        float weight=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
        if(weight<=1e-7)continue;
        ivec2 p=base+ivec2(x,y);
        if(any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,extent)) || RC_ResolveStatus(p)!=1u) return false;
        ivec2 representative=rcReceiver.viewport.xy+RC_ReceiverFirstPixel(p,rcReceiver.normalThreshold.z);
        if(any(notEqual(RC_ResolveID(representative),id)))return false;
        vec3 q=RC_ResolvePosition(p), n=RC_ResolveNormal(p), value;
        if(any(isnan(q)) || any(isinf(q)) || any(isnan(n)) || any(isinf(n)) ||
            dot(n,normal)<rcReceiver.normalThreshold.x ||
            abs(dot(q-position,normal))>rcReceiver.normalThreshold.y ||
            !RC_CheckerFetch(p,field,value)) return false;
        result+=weight*value;
    }
    if(field==1u) {
#ifndef RC_RECEIVER_RESOLVE_TEST
        vec3 neighbors[4]; int count=0;
        for(int y=0;y<2;y++) for(int x=0;x<2;x++) {
            float weight=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
            if(weight>1e-7) neighbors[count++]=RC_ResolvePosition(base+ivec2(x,y));
        }
        if(!RC_ReceiverVisibilityCompatibleAll(position,neighbors,count,rcLighting.receiver_filter.x)) return false;
#else
        for(int y=0;y<2;y++) for(int x=0;x<2;x++) {
            float weight=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
            if(weight>1e-7 && !RC_ReceiverVisibilityCompatible(position,RC_ResolvePosition(base+ivec2(x,y)),rcLighting.receiver_filter.x)) return false;
        }
#endif
    }
#endif
    return !(any(isnan(result)) || any(isinf(result)));
}
bool RC_ReceiverIndirect(ivec2 pixel, uvec2 id, vec3 position, vec3 normal, out vec3 indirect) {
    return RC_ReceiverField(pixel,id,position,normal,1u,indirect);
}
/* Direct and local-minimum samples share positions, visibility and parity.
 * Resolve both fields together so the full-resolution forward pass does not
 * repeat receiver classification and checkerboard neighbor validation. */
bool RC_CheckerDirectPair(ivec2 pixel, ivec2 extent, out vec3 direct, out vec3 minimum)
{
    direct=vec3(0); minimum=vec3(0);
    if(RC_CheckerHasSample(pixel,0u))
        return RC_CheckerFetch(pixel,0u,direct) && RC_CheckerFetch(pixel,2u,minimum) &&
            !any(isnan(direct)) && !any(isinf(direct)) && !any(isnan(minimum)) && !any(isinf(minimum));
    const ivec2 offsets[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    float count=0.0;
    for(int i=0;i<4;i++) {
        ivec2 p=pixel+offsets[i];
        if(any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,extent))) continue;
        vec3 d,m;
        if(!RC_CheckerCompatible(pixel,p,0u) || !RC_CheckerFetch(p,0u,d) || !RC_CheckerFetch(p,2u,m) ||
           any(isnan(d)) || any(isinf(d)) || any(isnan(m)) || any(isinf(m))) return false;
        direct+=d; minimum+=m; count+=1.0;
    }
    if(count==0.0) return false;
    direct/=count; minimum/=count;
    return true;
}
bool RC_ReceiverDirect(ivec2 pixel, uvec2 id, vec3 position, vec3 normal, out vec3 direct, out vec3 minimum)
{
    direct=vec3(0); minimum=vec3(0);
    ivec2 localPixel=pixel-rcReceiver.viewport.xy;
    /* A shared triangle edge can give this pixel to the neighbor in the prepass.
     * Texture taps judge geometry themselves, so the exact identity is only
     * required by the legacy path; demanding it here outlines every triangle. */
    if(any(lessThan(localPixel,ivec2(0))) || any(greaterThanEqual(localPixel,rcReceiver.viewport.zw)) ||
#if !RC_RECEIVER_TEXTURE_FILTER
       any(notEqual(RC_ResolveID(pixel),id)) ||
#endif
       any(equal(id,uvec2(0))) ||
       any(isnan(position)) || any(isinf(position)) || any(isnan(normal)) || any(isinf(normal))) return false;
    vec2 coord=RC_ReceiverSourceCoord(localPixel,rcReceiver.normalThreshold.z);
    ivec2 base=ivec2(floor(coord)); vec2 f=fract(coord);
    ivec2 extent=RC_ReceiverExtent(rcReceiver.viewport.zw,rcReceiver.normalThreshold.z);
#if RC_RECEIVER_TEXTURE_FILTER
    /* Same kernel as GI, so reduced scales do not leave direct light blocky. */
    float total=0.0, radius=RC_ReceiverUpscaleRadius();
    int stride=max(int(ceil(radius/3.0)),1),reach=int(ceil(radius/float(stride)));
    ivec2 lattice=(base/stride)*stride;
    for(int y=1-reach;y<=reach;y++) for(int x=1-reach;x<=reach;x++) {
        ivec2 p=lattice+ivec2(x,y)*stride; vec3 d,m;
        vec2 falloff=max(vec2(1)-abs(vec2(p)-coord)/radius,vec2(0));
        float weight=falloff.x*falloff.y;
        if(weight<=1e-7) continue;
        if(radius>1.0) {
            uvec2 otherID; vec3 otherPosition,otherNormal;
            if(!RC_ReceiverGuide(p,extent,otherID,otherPosition,otherNormal)) continue;
            weight*=RC_ReceiverGeometryWeight(id,position,normal,otherID,otherPosition,otherNormal);
            if(weight<=1e-7) continue;
        }
        else if(!RC_TextureReceiverTap(p,extent,id,position,normal)) continue;
        if(!RC_CheckerFetch(p,0u,d) || !RC_CheckerFetch(p,2u,m) || !RC_ReceiverFinite(d) || !RC_ReceiverFinite(m) ||
            any(greaterThan(abs(d),vec3(65504.0))) || any(greaterThan(abs(m),vec3(65504.0)))) continue;
        direct+=weight*d; minimum+=weight*m; total+=weight;
    }
    if(total<=0.0) { direct=vec3(0); minimum=vec3(0); return false; }
    direct/=total; minimum/=total;
#else
    for(int y=0;y<2;y++) for(int x=0;x<2;x++) {
        float weight=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
        if(weight<=1e-7) continue;
        ivec2 p=base+ivec2(x,y);
        if(any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,extent)) || RC_ResolveStatus(p)!=1u) return false;
        if(any(notEqual(RC_ResolveID(rcReceiver.viewport.xy+RC_ReceiverFirstPixel(p,rcReceiver.normalThreshold.z)),id))) return false;
        vec3 q=RC_ResolvePosition(p), n=RC_ResolveNormal(p);
        if(any(isnan(q)) || any(isinf(q)) || any(isnan(n)) || any(isinf(n)) ||
           dot(n,normal)<rcReceiver.normalThreshold.x ||
           abs(dot(q-position,normal))>rcReceiver.normalThreshold.y) return false;
        vec3 d,m;
        if(!RC_CheckerFetch(p,0u,d) || !RC_CheckerFetch(p,2u,m)) return false;
        direct+=weight*d; minimum+=weight*m;
    }
#endif
    return !(any(isnan(direct)) || any(isinf(direct)) || any(isnan(minimum)) || any(isinf(minimum)));
}
#if RC_RECEIVER_FULL_LIGHT
/* The gather and miss passes already resolved this pixel. Alpha 2 marks a miss. */
void RC_ReceiverFullLight(ivec2 pixel,out vec3 indirect,out vec3 direct,out vec3 minimum)
{
    vec4 gi=RC_ResolveValue(pixel,1u),light=RC_ResolveValue(pixel,0u);
    indirect=gi.rgb; direct=light.rgb;
    minimum=RC_RECEIVER_MINIMUM_LIGHTS?RC_ResolveValue(pixel,2u).rgb:vec3(0);
    if(gi.a>1.5) RC_NoteFallback(1u);
    if(light.a>1.5) RC_NoteFallback(2u);
}
#endif
/* The forward pass needs every field at one point. Guides dominate the kernel,
 * so one walk serves GI, direct and minimum instead of a walk per lookup. */
void RC_ReceiverLighting(ivec2 pixel,uvec2 id,vec3 position,vec3 normal,bool wantIndirect,
    out vec3 indirect,out bool indirectValid,out vec3 direct,out vec3 minimum,out bool directValid)
{
    indirect=vec3(0); direct=vec3(0); minimum=vec3(0); indirectValid=false; directValid=false;
#if RC_RECEIVER_TEXTURE_FILTER
    ivec2 localPixel=pixel-rcReceiver.viewport.xy;
    if(any(lessThan(localPixel,ivec2(0))) || any(greaterThanEqual(localPixel,rcReceiver.viewport.zw)) ||
       any(equal(id,uvec2(0))) ||
       any(isnan(position)) || any(isinf(position)) || any(isnan(normal)) || any(isinf(normal))) return;
    vec2 coord=RC_ReceiverSourceCoord(localPixel,rcReceiver.normalThreshold.z);
    ivec2 base=ivec2(floor(coord));
    ivec2 extent=RC_ReceiverExtent(rcReceiver.viewport.zw,rcReceiver.normalThreshold.z);
    float indirectTotal=0.0,directTotal=0.0,radius=RC_ReceiverUpscaleRadius();
    bool minimumLights=RC_RECEIVER_MINIMUM_LIGHTS;
    int stride=max(int(ceil(radius/3.0)),1),reach=int(ceil(radius/float(stride)));
    ivec2 lattice=(base/stride)*stride;
    for(int y=1-reach;y<=reach;y++) for(int x=1-reach;x<=reach;x++) {
        ivec2 p=lattice+ivec2(x,y)*stride;
        vec2 falloff=max(vec2(1)-abs(vec2(p)-coord)/radius,vec2(0));
        float weight=falloff.x*falloff.y;
        if(weight<=1e-7) continue;
        if(radius>1.0) {
            uvec2 otherID; vec3 otherPosition,otherNormal;
            if(!RC_ReceiverGuide(p,extent,otherID,otherPosition,otherNormal)) continue;
            weight*=RC_ReceiverGeometryWeight(id,position,normal,otherID,otherPosition,otherNormal);
            if(weight<=1e-7) continue;
        }
        else if(!RC_TextureReceiverTap(p,extent,id,position,normal)) continue;
        vec3 value,d,m=vec3(0);
        if(wantIndirect && RC_CheckerFetch(p,1u,value) && RC_ReceiverFinite(value) && !any(greaterThan(abs(value),vec3(65504.0)))) {
            indirect+=weight*value; indirectTotal+=weight;
        }
        if(RC_CheckerFetch(p,0u,d) && (!minimumLights || RC_CheckerFetch(p,2u,m)) && RC_ReceiverFinite(d) && RC_ReceiverFinite(m) &&
            !any(greaterThan(abs(d),vec3(65504.0))) && !any(greaterThan(abs(m),vec3(65504.0)))) {
            direct+=weight*d; minimum+=weight*m; directTotal+=weight;
        }
    }
    if(directTotal>0.0) {
        direct/=directTotal; minimum/=directTotal;
        directValid=RC_ReceiverFinite(direct) && RC_ReceiverFinite(minimum);
    } else { direct=vec3(0); minimum=vec3(0); }
    /* The rare empty kernel takes the GI lookup's wider search. */
    if(indirectTotal>0.0) { indirect/=indirectTotal; indirectValid=RC_ReceiverFinite(indirect); }
    else if(wantIndirect) indirectValid=RC_ReceiverField(pixel,id,position,normal,1u,indirect);
#else
    if(wantIndirect) indirectValid=RC_ReceiverIndirect(pixel,id,position,normal,indirect);
    directValid=RC_ReceiverDirect(pixel,id,position,normal,direct,minimum);
#endif
}
#endif
