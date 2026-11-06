#include "radiance_receiver_live.h"
#include "radiance_receiver_frame.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H(type,n) ((type)(uintptr_t)(n))
static bool fence_ready=true; static unsigned resets, preparations, marks, releases;
void RCReceiverCapture_Reset(void) { ++resets; }
void RCReceiverFrame_Stage(const rc_receiver_frame_plan_t *p) { assert(!p); ++resets; }
void RCReceiverSelect_Reset(void) { ++resets; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice d,VkFence f) { (void)d;(void)f;return fence_ready?VK_SUCCESS:VK_NOT_READY; }
bool RCReceiverSlot_Prepare(rc_receiver_slot_t *s,const rc_receiver_slot_config_t *c)
{ ++preparations; if(!c)return false;s->device=c->device;s->resources.device=c->device;s->ready=true;return true; }
bool RCReceiverSlot_MarkSubmitted(rc_receiver_slot_t *s,VkFence f)
{ if(!s->ready || !f)return false;s->last_fence=f;++marks;return true; }
bool RCReceiverSlot_Release(rc_receiver_slot_t *s)
{ ++releases;memset(s,0,sizeof(*s));return true; }
int main(void)
{
	rc_receiver_slot_config_t config={.device=H(VkDevice,1)}; VkFence fence=H(VkFence,1);
	RCReceiverLive_Begin(0); assert(resets==3 && !RCReceiverLive_Get(2));
	assert(!RCReceiverLive_Prepare(2,&config)); assert(RCReceiverLive_Prepare(0,&config));
	rc_receiver_slot_t *slot=RCReceiverLive_Get(0); assert(slot && preparations==1);
	assert(!RCReceiverLive_CanUse(0,NULL)); assert(RCReceiverLive_CanUse(0,&slot->resources.targets));
	assert(!RCReceiverLive_MarkUsed(0,NULL)); assert(RCReceiverLive_MarkUsed(0,&slot->resources.targets));
	assert(!RCReceiverLive_CanUse(0,&slot->resources.targets));
	assert(!RCReceiverLive_Prepare(0,&config)); assert(!RCReceiverLive_BeforeSubmit(0,0));
	assert(RCReceiverLive_BeforeSubmit(0,fence) && marks==1 && slot->last_fence==fence);
	assert(!RCReceiverLive_CanUse(0,&slot->resources.targets));
	fence_ready=false; assert(!RCReceiverLive_Shutdown() && !releases);
	fence_ready=true; assert(RCReceiverLive_Shutdown() && releases==2 && !slot->ready);
	puts("PASS: live receiver slots track exact ownership and submitted fence before shutdown"); return 0;
}
