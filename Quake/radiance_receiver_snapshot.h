#ifndef RADIANCE_RECEIVER_SNAPSHOT_H
#define RADIANCE_RECEIVER_SNAPSHOT_H
#include "radiance_receiver_frame.h"
/* Storage belongs to the caller and must remain fenced until both passes have
 * recorded and finished executing. Separate render workers need disjoint token
 * ranges and storage; tokens must also be unique across renderer families. */
typedef struct {
	uint64_t frame_serial;
	rc_receiver_world_draw_t *draws;
	uint32_t count;
} rc_receiver_world_snapshot_t;
/* Validate the complete input before writing storage or replacing snapshot. */
bool RCReceiverSnapshot_Build(rc_receiver_world_snapshot_t *snapshot, uint64_t frame_serial,
	rc_receiver_world_draw_t *storage, uint32_t capacity, const rc_receiver_world_draw_t *input,
	uint32_t count, uint32_t first_token, VkCommandBuffer command, VkPipelineLayout layout,
	const VkPipeline pipelines[2]);
/* Makes a one-shot copy of plan with the owned snapshot attached. */
bool RCReceiverSnapshot_Stage(const rc_receiver_world_snapshot_t *snapshot, const rc_receiver_frame_plan_t *plan);
#endif
