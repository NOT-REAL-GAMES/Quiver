#ifndef RC_RECEIVER_SCHEDULE_GLSL
#define RC_RECEIVER_SCHEDULE_GLSL
/* Fields 0/2 share direct parity; field 1 is bounce. */
bool RC_CheckerHasSample(ivec2 pixel, uint field)
{
    return uint((pixel.x + pixel.y) & 1) == (field & 1u);
}
bool RC_ReceiverShouldEvaluate(ivec2 pixel, uint field, bool checkerboard)
{
    return !checkerboard || RC_CheckerHasSample(pixel,field);
}
#endif
