#ifndef RC_RECEIVER_CHECKERBOARD_GLSL
#define RC_RECEIVER_CHECKERBOARD_GLSL
/* Experimental spatial checkerboard reconstruction. The producer evaluates
 * direct components at parity 0 and indirect at parity 1, and marks each
 * successfully written field independently. The caller supplies hooks below.
 * All coordinates are local to the receiver viewport. Never use history.
 *
 * bool RC_CheckerFetch(ivec2 p, uint field, out vec3 value)
 * bool RC_CheckerCompatible(ivec2 dst, ivec2 src, uint field)
 *
 * Compatible must check primitive identity, depth/plane and normal, and (for
 * indirect) receiver-to-neighbor scene visibility. On any rejected in-bounds
 * neighbor return false and evaluate the field at full resolution instead.
 */
bool RC_CheckerHasSample(ivec2 pixel, uint field)
{
    return uint((pixel.x + pixel.y) & 1) == (field & 1u);
}

bool RC_CheckerReconstruct(ivec2 pixel, ivec2 extent, uint field, out vec3 value)
{
    value = vec3(0.0);
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, extent))) return false;
    if (RC_CheckerHasSample(pixel, field)) {
        if (!RC_CheckerFetch(pixel, field, value) || any(isnan(value)) || any(isinf(value))) {
            value = vec3(0.0);
            return false;
        }
        return true;
    }
    const ivec2 offsets[4] = ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    float count = 0.0;
    for (int i=0;i<4;i++) {
        ivec2 p = pixel + offsets[i];
        if (any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,extent))) continue;
        vec3 sampleValue;
        if (!RC_CheckerFetch(p,field,sampleValue) || !RC_CheckerCompatible(pixel,p,field) ||
            any(isnan(sampleValue)) || any(isinf(sampleValue))) { value=vec3(0); return false; }
        value += sampleValue;
        count += 1.0;
    }
    if (count == 0.0) return false;
    value /= count;
    return true;
}

/* Direct light and local minimum share the same parity and geometry checks,
 * but their validity is independent. Reconstruct them in one traversal. */
void RC_CheckerReconstructDirectPair(ivec2 pixel, ivec2 extent,
                                     out vec3 direct, out bool directValid,
                                     out vec3 minimum, out bool minimumValid)
{
    direct=vec3(0); minimum=vec3(0); directValid=false; minimumValid=false;
    if (any(lessThan(pixel,ivec2(0))) || any(greaterThanEqual(pixel,extent))) return;
    if (RC_CheckerHasSample(pixel,0u)) {
        directValid=RC_CheckerFetch(pixel,0u,direct) && !any(isnan(direct)) && !any(isinf(direct));
        minimumValid=RC_CheckerFetch(pixel,2u,minimum) && !any(isnan(minimum)) && !any(isinf(minimum));
    } else {
        const ivec2 offsets[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
        float count=0.0; directValid=true; minimumValid=true;
        for(int i=0;i<4;i++) {
            ivec2 p=pixel+offsets[i];
            if(any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,extent))) continue;
            if(!RC_CheckerCompatible(pixel,p,0u)) { directValid=false; minimumValid=false; break; }
            vec3 value;
            if(directValid) {
                if(!RC_CheckerFetch(p,0u,value) || any(isnan(value)) || any(isinf(value))) directValid=false;
                else direct+=value;
            }
            if(minimumValid) {
                if(!RC_CheckerFetch(p,2u,value) || any(isnan(value)) || any(isinf(value))) minimumValid=false;
                else minimum+=value;
            }
            count+=1.0;
            if(!directValid && !minimumValid) break;
        }
        if(count==0.0) { directValid=false; minimumValid=false; }
        else { if(directValid) direct/=count; if(minimumValid) minimum/=count; }
    }
    if(!directValid) direct=vec3(0);
    if(!minimumValid) minimum=vec3(0);
}
#endif
