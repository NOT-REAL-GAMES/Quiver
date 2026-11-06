/* Opt-in raster receiver pipelines. Include after glquake.h. */
#ifndef QUIVER_RADIANCE_GRAPHICS_H
#define QUIVER_RADIANCE_GRAPHICS_H

typedef struct rc_graphics_family_s
{
	vulkan_pipeline_t main[MAIN_RENDER_PASS_VARIANT_COUNT][MODEL_PIPELINE_COUNT];
	vulkan_pipeline_t wboit[MODEL_PIPELINE_COUNT], moment[MODEL_PIPELINE_COUNT], composite[MODEL_PIPELINE_COUNT];
} rc_graphics_family_t;

typedef struct rc_graphics_s
{
	vulkan_pipeline_layout_t world_layout, alias_layout, md5_layout;
	VkDescriptorSetLayout empty_layout;
	vulkan_pipeline_t world[MAIN_RENDER_PASS_VARIANT_COUNT][WORLD_PIPELINE_COUNT];
	vulkan_pipeline_t world_wboit[WORLD_PIPELINE_COUNT], world_moment[WORLD_PIPELINE_COUNT], world_composite[WORLD_PIPELINE_COUNT];
	rc_graphics_family_t alias, md5, md5_8;
	vulkan_pipeline_t instanced[MAIN_RENDER_PASS_VARIANT_COUNT];
	uint32_t variants_ready, variants_attempted;
	bool hardware;
	bool ready;
} rc_graphics_t;

/* Receiver resources must outlive these pipelines. On failure, the object is
 * cleaned and classic rendering remains available. Destroy only after GPU idle. */
bool RCGraphics_Create (rc_graphics_t *graphics, VkDescriptorSetLayout receiver_layout, bool hardware);
/* Prepare the current OIT/render-pass variant before dispatching draw tasks. */
bool RCGraphics_EnsureCurrentVariant (rc_graphics_t *graphics);
void RCGraphics_Destroy (rc_graphics_t *graphics);
/* Draw code should use classic showtris and mesh/task pipelines only with the
 * classic path. Bind set 5 after selecting the returned radiance pipeline. */
vulkan_pipeline_t RCGraphics_WorldPipeline (const rc_graphics_t *graphics, subpass_type_t subpass, int variant, int index);
/* model_kind: 0 alias, 1 MD5, 2 eight-weight MD5. */
vulkan_pipeline_t RCGraphics_ModelPipeline (const rc_graphics_t *graphics, subpass_type_t subpass, int variant, int index, int model_kind);
bool RCGraphics_BindReceiver (cb_context_t *context, VkDescriptorSet descriptor);
/* Publish only at a frame boundary after fencing and descriptor update, before
 * recording begins. Keep resources immutable until all recording tasks finish.
 * Pass NULL/VK_NULL_HANDLE to restore classic rendering. */
VkDescriptorSet RCGraphics_FrameDescriptor (void);
bool RCGraphics_SetFrame (const rc_graphics_t *graphics, VkDescriptorSet descriptor);
bool RCGraphics_Active (void);
const rc_graphics_t *RCGraphics_Frame (void);
bool RCGraphics_BindFrame (cb_context_t *context);

#endif
