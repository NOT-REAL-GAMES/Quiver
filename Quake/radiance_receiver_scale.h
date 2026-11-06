#ifndef RADIANCE_RECEIVER_SCALE_H
#define RADIANCE_RECEIVER_SCALE_H
#include <stdbool.h>
#include <stdint.h>

/* Actual inverse scales, shared by allocation, recording and diagnostics.
 * Exact comparisons also reject every nonfinite or unsupported value. */
static inline bool RCReceiver_ValidDivisor(float divisor)
{
	return divisor==1.0f || divisor==1.5f || divisor==2.0f || divisor==3.0f || divisor==4.0f;
}
static inline uint32_t RCReceiver_ScaledExtent(uint32_t full_extent,float divisor)
{
	if(divisor==1.5f) return (uint32_t)(((uint64_t)full_extent*2u+2u)/3u);
	if(!RCReceiver_ValidDivisor(divisor)) return 0;
	uint32_t integer=(uint32_t)divisor;
	return full_extent/integer+(full_extent%integer!=0u);
}
#endif
