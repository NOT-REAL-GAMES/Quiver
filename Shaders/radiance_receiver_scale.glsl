#ifndef RC_RECEIVER_SCALE_GLSL
#define RC_RECEIVER_SCALE_GLSL
/* Divisor 1 is the full-resolution grid; any divisor up to 16 is valid. A block
 * starts at floor(block*divisor), and the extent and pixel mappings are derived
 * from that rather than by division, so they agree exactly with the host. */
int RC_ReceiverFirst(int block,float divisor) { return int(floor(float(block)*divisor)); }
int RC_ReceiverExtent1(int full,float divisor)
{
    int extent=int(ceil(float(full)/divisor));
    while(RC_ReceiverFirst(extent,divisor)<full) ++extent;
    while(extent>0 && RC_ReceiverFirst(extent-1,divisor)>=full) --extent;
    return extent;
}
int RC_ReceiverBlock1(int pixel,float divisor)
{
    int block=int(floor(float(pixel)/divisor));
    while(RC_ReceiverFirst(block+1,divisor)<=pixel) ++block;
    while(RC_ReceiverFirst(block,divisor)>pixel) --block;
    return block;
}
ivec2 RC_ReceiverExtent(ivec2 fullSize,float divisor)
{ return ivec2(RC_ReceiverExtent1(fullSize.x,divisor),RC_ReceiverExtent1(fullSize.y,divisor)); }
ivec2 RC_ReceiverFirstPixel(ivec2 block,float divisor)
{ return ivec2(RC_ReceiverFirst(block.x,divisor),RC_ReceiverFirst(block.y,divisor)); }
ivec2 RC_ReceiverPixelBlock(ivec2 pixel,float divisor)
{ return ivec2(RC_ReceiverBlock1(pixel.x,divisor),RC_ReceiverBlock1(pixel.y,divisor)); }
#ifdef RC_RECEIVER_TILE_PIXEL
/* Checkerboard parities take different paths, direct light and GI. Each 32-lane
 * half of an 8x8 tile gets one parity, so a warp runs one path, not both.
 * Without a checkerboard the plain layout keeps neighbouring rays together. */
ivec2 RC_ReceiverTilePixel(bool checker)
{
    if(!checker) return ivec2(gl_GlobalInvocationID.xy);
    int lane=int(gl_LocalInvocationIndex),y=(lane&31)>>2,parity=lane>>5;
    return ivec2(gl_WorkGroupID.xy)*8+ivec2((lane&3)*2+((y+parity)&1),y);
}
#endif
vec2 RC_ReceiverSourceCoord(ivec2 pixel,float divisor)
{
    if(fract(divisor)==0.0) return vec2(pixel)/divisor;
    /* Interpolate between actual top-left representatives, which keeps uneven
     * block widths free of a phase error. */
    ivec2 base=RC_ReceiverPixelBlock(pixel,divisor);
    ivec2 first=RC_ReceiverFirstPixel(base,divisor);
    ivec2 next=RC_ReceiverFirstPixel(base+1,divisor);
    return vec2(base)+vec2(pixel-first)/vec2(next-first);
}
#endif
