#ifndef RC_RECEIVER_CHECKERBOARD_BATCHED_GLSL
#define RC_RECEIVER_CHECKERBOARD_BATCHED_GLSL
/* Validate geometry and sample availability first, then share the probe
 * lookup for all surviving checkerboard neighbors. */
bool RC_ReconstructIndirect(ivec2 pixel, ivec2 extent, out vec3 value) {
 value=vec3(0);
 if(any(lessThan(pixel,ivec2(0))) || any(greaterThanEqual(pixel,extent))) return false;
 if(RC_CheckerHasSample(pixel,1u)) {
   if(!RC_CheckerFetch(pixel,1u,value) || any(isnan(value)) || any(isinf(value))) { value=vec3(0); return false; }
   return true;
 }
 const ivec2 offsets[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
 vec3 neighbors[4]; int count=0;
 for(int j=0;j<4;j++) {
   ivec2 p=pixel+offsets[j];
   if(any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,extent))) continue;
   vec3 v;
   if(!RC_CheckerFetch(p,1u,v) || !RC_CheckerCompatible(pixel,p,0u) || any(isnan(v)) || any(isinf(v))) {value=vec3(0);return false;}
   neighbors[count++]=RC_ResolvePosition(p); value+=v;
 }
 if(count==0 || !RC_ReceiverVisibilityCompatibleAll(RC_ResolvePosition(pixel),neighbors,count,rcLighting.receiver_filter.x)) {value=vec3(0);return false;}
 value/=float(count); return true;
}
#endif
