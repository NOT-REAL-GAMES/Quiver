#ifndef RADIANCE_RECEIVER_SCALE_H
#define RADIANCE_RECEIVER_SCALE_H
#include <stdbool.h>
#include <math.h>
#include <stdint.h>

/* Actual inverse scales, shared by allocation, recording and diagnostics. */
/* Receiver blocks need not be whole pixels wide. A block starts at
 * floor(block*divisor), in single precision to match the shaders, and every
 * other mapping is derived from that so C and GLSL cannot disagree. */
#define RC_RECEIVER_MAX_DIVISOR 16.0f
static inline bool RCReceiver_ValidDivisor(float divisor)
{
	return isfinite(divisor) && divisor>=1.0f && divisor<=RC_RECEIVER_MAX_DIVISOR;
}
static inline uint32_t RCReceiver_FirstPixel(uint32_t block,float divisor)
{
	return (uint32_t)((float)block*divisor);
}
static inline uint32_t RCReceiver_ScaledExtent(uint32_t full_extent,float divisor)
{
	if(!RCReceiver_ValidDivisor(divisor)) return 0;
	/* These grids are exact at any size. Fractional ones stay far below the
	 * range where consecutive floats differ by more than a block. */
	if(divisor==1.5f) return (uint32_t)(((uint64_t)full_extent*2u+2u)/3u);
	if(divisor==(float)(uint32_t)divisor) {
		uint32_t integer=(uint32_t)divisor;
		return full_extent/integer+(full_extent%integer!=0u);
	}
	uint32_t extent=(uint32_t)((float)full_extent/divisor);
	while(RCReceiver_FirstPixel(extent,divisor)<full_extent) ++extent;
	while(extent && RCReceiver_FirstPixel(extent-1u,divisor)>=full_extent) --extent;
	return extent;
}
#endif
