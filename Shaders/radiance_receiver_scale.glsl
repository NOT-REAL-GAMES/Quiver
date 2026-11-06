#ifndef RC_RECEIVER_SCALE_GLSL
#define RC_RECEIVER_SCALE_GLSL
/* Divisor1 is the full-resolution receiver grid. Real divisor1.5 denotes two-thirds resolution. Integer arithmetic keeps
 * its alternating one/two-pixel partition exact at viewport edges. The
 * uniform/push ABI still carries the actual floating-point divisor. */
ivec2 RC_ReceiverExtent(ivec2 fullSize,float divisor)
{
    if(divisor==1.5) return (fullSize*2+2)/3;
    int d=int(divisor);
    return (fullSize+d-1)/d;
}
ivec2 RC_ReceiverFirstPixel(ivec2 block,float divisor)
{
    if(divisor==1.5) return (block*3)/2;
    return block*int(divisor);
}
ivec2 RC_ReceiverPixelBlock(ivec2 pixel,float divisor)
{
    if(divisor==1.5) return (pixel*2+1)/3;
    return pixel/int(divisor);
}
vec2 RC_ReceiverSourceCoord(ivec2 pixel,float divisor)
{
    /* Integer grids retain their established arithmetic exactly. The
     * rational grid interpolates between actual top-left representatives
     * 0,1,3,4,... rather than introducing a half-pixel phase error. */
    if(divisor!=1.5) return vec2(pixel)/divisor;
    ivec2 base=RC_ReceiverPixelBlock(pixel,divisor);
    ivec2 first=RC_ReceiverFirstPixel(base,divisor);
    ivec2 next=RC_ReceiverFirstPixel(base+1,divisor);
    return vec2(base)+vec2(pixel-first)/vec2(next-first);
}
#endif
