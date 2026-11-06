#include "radiance_receiver_snapshot.h"
#include <string.h>
bool RCReceiverSnapshot_Build(rc_receiver_world_snapshot_t *snapshot,uint64_t frame_serial,
	rc_receiver_world_draw_t *storage,uint32_t capacity,const rc_receiver_world_draw_t *input,
	uint32_t count,uint32_t first_token,VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline pipelines[2])
{
	if (!snapshot || !storage || !input || !count || count>capacity || !first_token ||
		first_token>UINT32_MAX-(count-1)) return false;
	/* Validate each draw with its assigned nonzero token, before any writes.
	 * Restrict overlap to exact alias: partially overlapping arrays are unsafe. */
	if (storage!=input) {
		uintptr_t dst=(uintptr_t)storage, src=(uintptr_t)input;
		size_t bytes=(size_t)count*sizeof(*storage);
		if (bytes/sizeof(*storage)!=count || dst>UINTPTR_MAX-bytes || src>UINTPTR_MAX-bytes ||
			(dst<src+bytes && src<dst+bytes)) return false;
	}
	for (uint32_t i=0;i<count;++i) {
		rc_receiver_world_draw_t draw=input[i]; draw.token=first_token+i;
		if (!RCReceiverWorld_CanRecord(command,layout,pipelines,&draw,1)) return false;
	}
	if (storage!=input) memcpy(storage,input,(size_t)count*sizeof(*storage));
	for (uint32_t i=0;i<count;++i) storage[i].token=first_token+i;
	*snapshot=(rc_receiver_world_snapshot_t){.frame_serial=frame_serial,.draws=storage,.count=count};
	return true;
}
bool RCReceiverSnapshot_Stage(const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_frame_plan_t *plan)
{
	if (!snapshot || !plan || (snapshot->count && !snapshot->draws) || (!snapshot->count && !plan->model_draw_count) || snapshot->frame_serial!=plan->frame_serial) {
		RCReceiverFrame_Stage(NULL); return false;
	}
	rc_receiver_frame_plan_t staged=*plan;
	staged.world_draws=snapshot->draws; staged.world_draw_count=snapshot->count;
	RCReceiverFrame_Stage(&staged); return true;
}
