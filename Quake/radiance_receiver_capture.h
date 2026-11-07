#ifndef RADIANCE_RECEIVER_CAPTURE_H
#define RADIANCE_RECEIVER_CAPTURE_H
#include "radiance_receiver_forward.h"
/* Each world worker owns one capture and two caller-owned frame arrays. Install
 * registrations before dispatching workers and wait for them before reading. */
typedef struct rc_receiver_world_capture_s {
	uint64_t frame_serial;
	rc_receiver_world_draw_t *draws;
	rc_receiver_forward_material_t *materials;
	uint32_t count, capacity, first_token;
	bool failed;
} rc_receiver_world_capture_t;
#define RC_RECEIVER_CAPTURE_SLOTS 32
#define RC_EXPERIMENTAL_CAPTURE_CAPACITY 512
bool RCReceiverCapture_Install(uint32_t slot, rc_receiver_world_capture_t *capture);
rc_receiver_world_capture_t *RCReceiverCapture_Acquire(uint32_t slot, uint64_t serial);
bool RCReceiverCapture_Add(rc_receiver_world_capture_t *capture, const rc_receiver_world_draw_t *draw,
	const rc_receiver_forward_material_t *material);
/* After all workers join, combine disjoint token-ordered captures into separate
 * caller-owned arrays. Failure leaves the previous snapshot and output intact. */
bool RCReceiverCapture_Collect(rc_receiver_world_snapshot_t *snapshot,uint64_t serial,
	rc_receiver_world_draw_t *draw_storage,rc_receiver_forward_material_t *material_storage,uint32_t capacity,
	rc_receiver_world_capture_t *const *captures,uint32_t capture_count);
void RCReceiverCapture_Reset(void);
#endif
