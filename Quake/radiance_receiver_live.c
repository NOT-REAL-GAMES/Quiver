#include "radiance_receiver_live.h"
#include "radiance_receiver_capture.h"
#include "radiance_receiver_select.h"
static rc_receiver_slot_t slots[RC_RECEIVER_LIVE_SLOTS];
static bool used[RC_RECEIVER_LIVE_SLOTS];
void RCReceiverLive_Begin(uint32_t slot)
{
	RCReceiverCapture_Reset(); RCReceiverFrame_Stage(NULL); RCReceiverSelect_Reset();
	if(slot<RC_RECEIVER_LIVE_SLOTS)used[slot]=false;
}
bool RCReceiverLive_Prepare(uint32_t slot,const rc_receiver_slot_config_t *config)
{
	return slot<RC_RECEIVER_LIVE_SLOTS && !used[slot] && RCReceiverSlot_Prepare(&slots[slot],config);
}
rc_receiver_slot_t *RCReceiverLive_Get(uint32_t slot) { return slot<RC_RECEIVER_LIVE_SLOTS ? &slots[slot] : NULL; }
bool RCReceiverLive_CanUse(uint32_t slot,const rc_receiver_targets_t *targets)
{
	return slot<RC_RECEIVER_LIVE_SLOTS && !used[slot] && slots[slot].ready && !slots[slot].last_fence &&
		targets==&slots[slot].resources.targets;
}
bool RCReceiverLive_MarkUsed(uint32_t slot,const rc_receiver_targets_t *targets)
{
	if(!RCReceiverLive_CanUse(slot,targets)) return false;
	used[slot]=true; return true;
}
bool RCReceiverLive_BeforeSubmit(uint32_t slot,VkFence fence)
{
	if(slot>=RC_RECEIVER_LIVE_SLOTS)return false;
	if(!used[slot])return true;
	if(!RCReceiverSlot_MarkSubmitted(&slots[slot],fence))return false;
	used[slot]=false; return true;
}
bool RCReceiverLive_Shutdown(void)
{
	/* Verify every slot is idle before releasing any, preserving consistency. */
	for(uint32_t i=0;i<RC_RECEIVER_LIVE_SLOTS;++i)
		if(slots[i].last_fence && (!slots[i].device || vkGetFenceStatus(slots[i].device,slots[i].last_fence)!=VK_SUCCESS))return false;
	for(uint32_t i=0;i<RC_RECEIVER_LIVE_SLOTS;++i) if(!RCReceiverSlot_Release(&slots[i]))return false;
	RCReceiverCapture_Reset(); RCReceiverFrame_Stage(NULL); RCReceiverSelect_Reset();
	for(uint32_t i=0;i<RC_RECEIVER_LIVE_SLOTS;++i)used[i]=false;
	return true;
}
