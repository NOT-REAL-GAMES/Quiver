#ifndef RADIANCE_RECEIVER_LAYOUTS_H
#define RADIANCE_RECEIVER_LAYOUTS_H
#include "radiance_receiver_descriptors.h"
typedef struct { VkDevice device; VkPipelineLayout classify, direct, prepass, forward, reconstruct; } rc_receiver_layouts_t;
/* world[0..4] must match the existing world renderer. The geometry feature is
 * needed by fragment PrimitiveID. radiance is the combined graphics receiver
 * layout; cascade and light remain legacy inputs. Old layouts must be
 * fence-idle on replace. */
bool RCReceiverLayouts_Create(rc_receiver_layouts_t *out,VkPhysicalDevice physical,VkDevice device,
	const rc_receiver_descriptors_t *descriptors,const VkDescriptorSetLayout world[5],
	VkDescriptorSetLayout radiance,VkDescriptorSetLayout cascade,VkDescriptorSetLayout light,bool geometry_enabled);
void RCReceiverLayouts_Destroy(rc_receiver_layouts_t *layouts);
#endif
