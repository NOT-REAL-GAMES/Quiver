#ifndef RADIANCE_RECEIVER_RECORD_H
#define RADIANCE_RECEIVER_RECORD_H
#include "radiance_receiver_targets.h"
#include "radiance_receiver_pipelines.h"
#include "radiance_receiver_scale.h"

/* Matches the classifier's 112-byte push constant block. */
typedef struct {
	float inverse_view_projection[16];
	int32_t viewport[4];
	float ndc_transform[4];
	uint32_t samples;
	float plane_tolerance, normal_cosine;
	float divisor; /* actual inverse scale: 1.5, 2, 3, 4 */
} rc_receiver_params_t;

bool RCReceiver_CanRecordCompute(VkCommandBuffer command, const rc_receiver_targets_t *targets,
	const rc_receiver_pipelines_t *pipelines, VkPipelineLayout classify_layout,
	VkPipelineLayout direct_layout, VkDescriptorSet classify_set, VkDescriptorSet direct_set,
	VkDescriptorSet cascade_set, VkDescriptorSet lighting_set, const rc_receiver_params_t *params);

/* Called outside a graphics render pass after the receiver prepass. Descriptor
 * layouts and image layouts must match the prototype shaders. All resources
 * belong to a fenced frame slot. Failure occurs before any commands are
 * recorded, so the caller can safely use full-resolution rendering. */
bool RCReceiver_RecordCompute(VkCommandBuffer command, const rc_receiver_targets_t *targets,
	const rc_receiver_pipelines_t *pipelines, VkPipelineLayout classify_layout,
	VkPipelineLayout direct_layout, VkDescriptorSet classify_set, VkDescriptorSet direct_set,
	VkDescriptorSet cascade_set, VkDescriptorSet lighting_set, const rc_receiver_params_t *params);

/* draw_ready performs all fallible preparation before recording; draw must
 * only record valid commands and must not mutate the borrowed resources. */
bool RCReceiver_RecordFrame(VkCommandBuffer command, const rc_receiver_targets_t *targets,
	const rc_receiver_pipelines_t *pipelines, VkPipelineLayout classify_layout,
	VkPipelineLayout direct_layout, VkDescriptorSet classify_set, VkDescriptorSet direct_set,
	VkDescriptorSet cascade_set, VkDescriptorSet lighting_set, const rc_receiver_params_t *params,
	float depth_clear, bool (*draw_ready)(void *), void (*draw)(VkCommandBuffer, void *), void *user);
/* Six optional frame-slot queries: prepass/classify, shade and postprocess.
 * This recorder owns the first four; the frame recorder owns the last pair. */
bool RCReceiver_RecordFrameTimed(VkCommandBuffer command, const rc_receiver_targets_t *targets,
	const rc_receiver_pipelines_t *pipelines, VkPipelineLayout classify_layout,
	VkPipelineLayout direct_layout, VkDescriptorSet classify_set, VkDescriptorSet direct_set,
	VkDescriptorSet cascade_set, VkDescriptorSet lighting_set, const rc_receiver_params_t *params,
	float depth_clear, bool (*draw_ready)(void *), void (*draw)(VkCommandBuffer, void *), void *user,
	VkQueryPool timing_pool, uint32_t timing_first_query);
/* Optional pipeline is borrowed and already compatible with direct_layout.
 * Null retains the ordinary hard producer without another dispatch/barrier. */
bool RCReceiver_RecordFrameSoftTimed(VkCommandBuffer command, const rc_receiver_targets_t *targets,
	const rc_receiver_pipelines_t *pipelines, VkPipelineLayout classify_layout,
	VkPipelineLayout direct_layout, VkDescriptorSet classify_set, VkDescriptorSet direct_set,
	VkDescriptorSet cascade_set, VkDescriptorSet lighting_set, const rc_receiver_params_t *params,
	float depth_clear, bool (*draw_ready)(void *), void (*draw)(VkCommandBuffer, void *), void *user,
	VkQueryPool timing_pool, uint32_t timing_first_query, VkPipeline soft_shadow_pipeline);
#endif
