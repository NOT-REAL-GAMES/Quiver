#ifndef RADIANCE_RECEIVER_ALTERNATES_H
#define RADIANCE_RECEIVER_ALTERNATES_H
#include "radiance_receiver_forward.h"
#include "radiance_receiver_select.h"
typedef struct {
	const rc_receiver_world_snapshot_t *snapshot;
	const rc_receiver_forward_material_t *materials;
} rc_receiver_alternate_part_t;
typedef struct {
	VkDevice device;
	VkCommandPool pool;
	uint64_t serial, fingerprint;
	VkRenderPass pass; VkFramebuffer framebuffer; uint32_t subpass;
	VkCommandBuffer buffers[RC_RECEIVER_ALTERNATE_LIMIT];
	uint32_t count;
} rc_receiver_alternates_t;
/* pool, pipelines, snapshots and framebuffer must be externally fenced. The
 * complete list of parts must reproduce all original direct-world secondaries.
 * Optional world timestamps use the two queries starting at world_first_query;
 * only the selected world path may execute these queries in a frame.
 * Creation does not stage or select the commands. On failure old buffers are
 * preserved and all newly allocated command buffers are freed. */
bool RCReceiverAlternates_Create(rc_receiver_alternates_t *out,VkDevice device,VkCommandPool pool,uint64_t serial,
	VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass,VkPipelineLayout layout,const VkPipeline pipelines[4],
	const VkViewport *viewport,const VkRect2D *scissor,const rc_receiver_alternate_part_t *parts,uint32_t count,
	VkQueryPool world_timestamps,uint32_t world_first_query);
void RCReceiverAlternates_Destroy(rc_receiver_alternates_t *alternates);
#endif
