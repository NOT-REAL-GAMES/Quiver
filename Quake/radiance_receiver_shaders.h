#ifndef RADIANCE_RECEIVER_SHADERS_H
#define RADIANCE_RECEIVER_SHADERS_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
typedef struct { VkDevice device; VkShaderModule vertex, prepass, classify, direct, forward, reconstruct; } rc_receiver_shaders_t;
/* The driver must have enabled the ray-query feature when hardware is true.
 * texture_filter chooses the bounded probe producer, guided forward sampler,
 * and texture-space postprocess. Modules are created transactionally and can be destroyed after pipelines. */
bool RCReceiverShaders_Create(rc_receiver_shaders_t *out,VkDevice device,bool hardware,bool texture_filter,bool multisampled,bool full_light);
/* Collapses multisampled prepass targets into the single-sample ones. Reads the
 * classifier's descriptor set, so it shares that pipeline layout. */
bool RCReceiverShaders_CreateResolvePipeline(VkPipeline *out,VkDevice device,VkPipelineCache cache,VkPipelineLayout layout,VkRenderPass pass);
/* Explicit opt-in only: create one optional compute pipeline and release its
 * temporary shader module. A failed attempt leaves the null output untouched. */
bool RCReceiverShaders_CreateSoftPipeline(VkPipeline *out,VkDevice device,VkPipelineCache cache,VkPipelineLayout layout,bool hardware);
void RCReceiverShaders_Destroy(rc_receiver_shaders_t *shaders);
#endif
