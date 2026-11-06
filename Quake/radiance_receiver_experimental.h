#ifndef QUIVER_RADIANCE_RECEIVER_EXPERIMENTAL_H
#define QUIVER_RADIANCE_RECEIVER_EXPERIMENTAL_H
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
/* Internal, per-frame borrowed bindings. Valid until frame recording ends. */
typedef struct {
	VkDescriptorSetLayout graphics, cascade, lighting;
	VkDescriptorSet cascade_set, lighting_set;
	bool hardware;
} rc_experimental_bindings_t;
bool R_RadianceExperimentalBindings(uint32_t slot, rc_experimental_bindings_t *out);
bool R_ReceiverExperimentalModelsPrepare(uint32_t slot,VkRenderPass pass,uint32_t subpass);
bool R_ReceiverExperimentalPrepare(uint32_t slot, VkRenderPass pass, uint32_t width, uint32_t height,
	const rc_experimental_bindings_t *bindings, const int32_t viewport[4], float divisor, bool texture_filter, bool soft_shadows, VkCommandPool *pool);
void R_ReceiverExperimentalShutdown(void);
void R_ReceiverExperimentalDiagnosticsCollect(uint32_t slot);
void R_ReceiverExperimentalDiagnosticsRecord(uint32_t slot,VkCommandBuffer command,uint64_t serial);
bool R_ReceiverExperimentalDiagnosticsSubmitted(uint32_t slot,VkFence fence);
#endif
