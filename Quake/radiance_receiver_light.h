#ifndef RADIANCE_RECEIVER_LIGHT_H
#define RADIANCE_RECEIVER_LIGHT_H
#include "radiance_receiver_descriptors.h"
#include "radiance_receiver_record.h"
/* Full-resolution receiver lighting. A gather pass upsamples GI, direct and
 * minimum once per visible pixel, and a second pass evaluates only the pixels
 * the gather missed. The forward shader then reads three texels, so it carries
 * neither the upscale kernel nor the fallback. Single-sample scenes only: an
 * MSAA edge pixel can hold two surfaces, and these images hold one. */
enum { RC_RECEIVER_LIGHT_INDIRECT, RC_RECEIVER_LIGHT_DIRECT, RC_RECEIVER_LIGHT_MINIMUM, RC_RECEIVER_LIGHT_COUNT };
typedef struct rc_receiver_light_s {
	VkDevice device;
	rc_receiver_image_t images[RC_RECEIVER_LIGHT_COUNT];
	VkBuffer misses;
	VkDeviceMemory misses_memory;
	VkDescriptorSetLayout layout;
	VkDescriptorPool pool;
	VkDescriptorSet gather[2]; /* raw, then postprocessed fields */
	VkDescriptorSet forward;
	VkPipelineLayout pipeline_layout;
	VkPipeline gather_pipeline, miss_pipeline;
	float upscale_extent; /* refreshed every frame, like the viewport UBO */
} rc_receiver_light_t;
/* targets and descriptors must outlive this object. forward_layout is the
 * receiver forward set layout; the forward set keeps its ABI with the three
 * field bindings pointing at these images. On failure out is untouched. */
bool RCReceiverLight_Create(rc_receiver_light_t *out,VkPhysicalDevice physical,VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory,const rc_receiver_targets_t *targets,VkDescriptorSetLayout forward_layout,
	VkSampler nearest,VkBuffer viewport_buffer,VkDeviceSize viewport_offset,VkDeviceSize viewport_range,
	VkDescriptorSetLayout radiance,VkPipelineCache cache,bool hardware);
void RCReceiverLight_Destroy(rc_receiver_light_t *light);
/* Records after every field is final, outside any render pass. postprocessed
 * selects the filter's output. Leaves the images ready for fragment reads. */
void RCReceiverLight_Record(VkCommandBuffer command,const rc_receiver_light_t *light,const rc_receiver_params_t *params,
	bool postprocessed,VkDescriptorSet radiance_set);
#endif
