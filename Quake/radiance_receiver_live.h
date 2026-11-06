#ifndef RADIANCE_RECEIVER_LIVE_H
#define RADIANCE_RECEIVER_LIVE_H
#include "radiance_receiver_slot.h"
#define RC_RECEIVER_LIVE_SLOTS 2
/* Called only after the renderer has completed the slot's previous fence. */
void RCReceiverLive_Begin(uint32_t slot);
/* A future renderer provider supplies actual per-slot pipeline/descriptor
 * configuration. There is no default provider and no automatic activation. */
bool RCReceiverLive_Prepare(uint32_t slot,const rc_receiver_slot_config_t *config);
rc_receiver_slot_t *RCReceiverLive_Get(uint32_t slot);
/* The frame graph accepts only resources owned by the current live slot. */
bool RCReceiverLive_CanUse(uint32_t slot,const rc_receiver_targets_t *targets);
bool RCReceiverLive_MarkUsed(uint32_t slot,const rc_receiver_targets_t *targets);
/* Call before resetting/submitting this slot's fence. */
bool RCReceiverLive_BeforeSubmit(uint32_t slot,VkFence fence);
/* Call with device idle before destroying external renderer resources. */
bool RCReceiverLive_Shutdown(void);
#endif
