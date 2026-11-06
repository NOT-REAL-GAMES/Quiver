#ifndef RADIANCE_RECEIVER_FRAME_H
#define RADIANCE_RECEIVER_FRAME_H
#include "radiance_receiver_record.h"
#include "radiance_receiver_world.h"
#include "radiance_receiver_model.h"
/* SCR_DrawDone advances the counter before end-of-frame recording. */
static inline uint64_t RCReceiverFrame_CompletedSerial(uint32_t frame_count)
{
	return (uint64_t)(frame_count - 1u);
}
struct rc_receiver_forward_material_s;
/* All resources and the immutable draw snapshot remain owned by the caller
 * until command recording has finished. Stage on the render thread only. */
typedef struct {
	uint64_t frame_serial;
	bool enabled;
	/* True when the forward receiver resolves incompatible geometry with its full-resolution fallback.
	 * It does not prove direct-shadow visibility is constant within a surface. */
	bool edge_fallback_enabled;
	const rc_receiver_targets_t *targets;
	const rc_receiver_pipelines_t *pipelines;
	VkPipelineLayout classify_layout, direct_layout, world_layout;
	VkDescriptorSet classify_set, direct_set, cascade_set, lighting_set;
	VkPipeline reconstruct_pipeline;
	VkPipelineLayout reconstruct_layout;
	VkDescriptorSet reconstruct_set, reconstruct_reverse_set;
	bool postprocess,checkerboard,texture_filter,gaussian_filter,smooth_shadows;
	float shadow_filter_strength;
	VkPipeline soft_shadow_pipeline;
	bool soft_shadows;
	VkQueryPool timing_pool;
	uint32_t timing_first_query;
	VkPipeline world_pipelines[2];
	const rc_receiver_world_draw_t *world_draws;
	uint32_t world_draw_count;
	const struct rc_receiver_forward_material_s *forward_materials;
	uint64_t forward_fingerprint;
	const rc_receiver_model_pipelines_t *model_pipelines;
	const rc_receiver_model_draw_t *model_draws;
	uint32_t model_draw_count;
	uint64_t model_fingerprint;
	rc_receiver_params_t params;
	float depth_clear;
} rc_receiver_frame_plan_t;
/* One-shot plan: only a matching frame may consume it. A plan not staged for
 * the current frame and any failed preflight force full-resolution rendering. */
void RCReceiverFrame_Stage(const rc_receiver_frame_plan_t *plan);
bool RCReceiverFrame_Record(VkCommandBuffer command, uint64_t frame_serial);
const rc_receiver_targets_t *RCReceiverFrame_PendingTargets(uint64_t frame_serial);
bool RCReceiverFrame_Active(uint64_t frame_serial);
float RCReceiverFrame_ActiveDivisor(uint64_t frame_serial);
bool RCReceiverFrame_ActiveCheckerboard(uint64_t frame_serial);
uint32_t RCReceiverFrame_ActiveFilterMode(uint64_t frame_serial);
const rc_receiver_params_t *RCReceiverFrame_ActiveParams(uint64_t frame_serial);
bool RCReceiverFrame_ActivePostprocess(uint64_t frame_serial);
bool RCReceiverFrame_ActiveSoftShadows(uint64_t frame_serial);
float RCReceiverFrame_ActiveShadowFilter(uint64_t frame_serial);
const rc_receiver_targets_t *RCReceiverFrame_ActiveTargets(uint64_t frame_serial);
void RCReceiverFrame_Reset(void);
#endif
