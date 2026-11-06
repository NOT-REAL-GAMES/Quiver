#ifndef RADIANCE_RECEIVER_STAGE_H
#define RADIANCE_RECEIVER_STAGE_H
#include "radiance_receiver_resources.h"
#include "radiance_receiver_forward.h"
#include "radiance_receiver_alternates.h"
/* All external resources, descriptors and completed alternate secondary command
 * buffers are borrowed until the submitted frame's fence signals. The caller
 * must update the viewport UBO before execution, and may assert edge coverage
 * only after providing conservative same-primitive shadow-edge fallback.
 * texture_filter selects the dedicated shader bundle; checkerboard describes
 * raw sampling parity independently of whether a postprocess runs. smooth_indirect
 * selects the three-pass texture Gaussian and requires its reverse descriptor.
 * smooth_shadows independently requests the first texture filter pass and
 * requires an active optional soft-shadow pipeline.
 * The chosen
 * receiver descriptor determines whether raw or resolved fields are consumed.
 * Optional six-query timing storage is owned by the same fenced frame slot. */
bool RCReceiverResources_StageFrame(const rc_receiver_resources_t *resources,uint64_t serial,VkCommandBuffer primary,
	VkDescriptorSet cascade_set,VkDescriptorSet lighting_set,const rc_receiver_params_t *params,float depth_clear,
	const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *materials,
	const rc_receiver_alternates_t *alternate,bool complete_world,bool edge_fallback_enabled,bool texture_filter,bool checkerboard,bool smooth_indirect,bool smooth_shadows,bool soft_shadows,VkQueryPool timing_pool,uint32_t timing_first_query,float shadow_filter_strength);
/* Models share the depth/normal/ID prepass. Their forward alternative must be
 * ready before staging; world-empty/model-only frames are supported. */
bool RCReceiverResources_StageFrameModels(const rc_receiver_resources_t *resources,uint64_t serial,VkCommandBuffer primary,
	VkDescriptorSet cascade_set,VkDescriptorSet lighting_set,const rc_receiver_params_t *params,float depth_clear,
	const rc_receiver_world_snapshot_t *snapshot,const rc_receiver_forward_material_t *materials,
	const rc_receiver_alternates_t *alternate,bool complete_world,bool edge_fallback_enabled,bool texture_filter,bool checkerboard,bool smooth_indirect,bool smooth_shadows,bool soft_shadows,VkQueryPool timing_pool,uint32_t timing_first_query,float shadow_filter_strength,
	const rc_receiver_model_snapshot_t *models,const rc_receiver_model_alternates_t *model_alternate);
#endif
