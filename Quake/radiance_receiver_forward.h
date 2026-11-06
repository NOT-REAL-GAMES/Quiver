#ifndef RADIANCE_RECEIVER_FORWARD_H
#define RADIANCE_RECEIVER_FORWARD_H
#include "radiance_receiver_snapshot.h"
#define RC_RECEIVER_FORWARD_HASH_SEED UINT64_C(14695981039346656037)
typedef struct rc_receiver_forward_material_s {
	VkDescriptorSet lightmap_set, fullbright_set, reuse_set;
	float fog_color[3], fog_density, alpha;
	bool fullbright;
} rc_receiver_forward_material_t;
/* pipelines index = fullbright + 2*alpha_test. The renderer must select this
 * path before recording secondaries only when the matching prepass is certain
 * to execute; otherwise it must record its ordinary full-resolution commands. */
bool RCReceiverForward_CanRecord(VkCommandBuffer command, VkPipelineLayout layout,
	const VkPipeline pipelines[4], const rc_receiver_world_snapshot_t *snapshot,
	const rc_receiver_forward_material_t *materials, uint64_t frame_serial);
uint64_t RCReceiverForward_Fingerprint(uint64_t seed,const rc_receiver_world_snapshot_t *snapshot,
	const rc_receiver_forward_material_t *materials);
void RCReceiverForward_Record(VkCommandBuffer command, VkPipelineLayout layout,
	const VkPipeline pipelines[4], const rc_receiver_world_snapshot_t *snapshot,
	const rc_receiver_forward_material_t *materials);
#endif
