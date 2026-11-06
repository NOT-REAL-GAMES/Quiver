#ifndef RADIANCE_RECEIVER_SELECT_H
#define RADIANCE_RECEIVER_SELECT_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#define RC_RECEIVER_ALTERNATE_LIMIT 32
/* Alternative secondary command buffers must be completed and compatible with
 * the active world subpass, and cover every ordinary world secondary. They and
 * their referenced resources remain alive until the frame fence signals. */
bool RCReceiverSelect_Stage(uint64_t serial,const VkCommandBuffer *secondary,uint32_t count,bool complete_world,
	VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass);
/* Returns false without recording commands unless both the receiver pass and
 * complete compatible alternates are available for this exact frame. */
bool RCReceiverSelect_Submit(VkCommandBuffer primary,uint64_t serial,bool receiver_active,
	VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass);
void RCReceiverSelect_Reset(void);
#endif
