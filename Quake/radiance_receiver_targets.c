#include "radiance_receiver_targets.h"
#include "radiance_receiver_scale.h"
#include <string.h>

void RCReceiverTargets_Destroy(rc_receiver_targets_t *t)
{
	if (!t || !t->device) return;
	if (t->framebuffer) vkDestroyFramebuffer(t->device, t->framebuffer, NULL);
	if (t->pass) vkDestroyRenderPass(t->device, t->pass, NULL);
	for (unsigned i=0; i<RC_RECEIVER_TARGET_COUNT; ++i) {
		if (t->images[i].view) vkDestroyImageView(t->device, t->images[i].view, NULL);
		if (t->images[i].image) vkDestroyImage(t->device, t->images[i].image, NULL);
		if (t->images[i].memory) vkFreeMemory(t->device, t->images[i].memory, NULL);
	}
	memset(t,0,sizeof(*t));
}

static bool RCReceiverTargets_Image(rc_receiver_targets_t *t, VkPhysicalDevice physical,
	const VkPhysicalDeviceMemoryProperties *memory, unsigned index, VkFormat format,
	VkImageUsageFlags usage, VkFormatFeatureFlags required, VkImageAspectFlags aspect,
	uint32_t width, uint32_t height)
{
	VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
	if ((properties.optimalTilingFeatures & required) != required) return false;
	VkImageFormatProperties supported;
	if (vkGetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&supported) != VK_SUCCESS ||
		!(supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT) || supported.maxExtent.width < width || supported.maxExtent.height < height) return false;
	rc_receiver_image_t *image=&t->images[index];
	VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D,
		.format=format, .extent={width,height,1}, .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
		.tiling=VK_IMAGE_TILING_OPTIMAL, .usage=usage, .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
	if (vkCreateImage(t->device,&info,NULL,&image->image)!=VK_SUCCESS) return false;
	VkMemoryRequirements req; vkGetImageMemoryRequirements(t->device,image->image,&req);
	uint32_t type=0;
	while (type<memory->memoryTypeCount && (!(req.memoryTypeBits & (1u<<type)) ||
		!(memory->memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++type;
	if (type==memory->memoryTypeCount) return false;
	VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
	if (vkAllocateMemory(t->device,&allocation,NULL,&image->memory)!=VK_SUCCESS ||
		vkBindImageMemory(t->device,image->image,image->memory,0)!=VK_SUCCESS) return false;
	VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image->image,
		.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={aspect,0,1,0,1}};
	return vkCreateImageView(t->device,&view,NULL,&image->view)==VK_SUCCESS;
}

bool RCReceiverTargets_Create(rc_receiver_targets_t *out, VkPhysicalDevice physical, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory, uint32_t width, uint32_t height, VkSampleCountFlagBits samples)
{
	if (!out || !physical || !device || !memory || !width || !height || samples!=VK_SAMPLE_COUNT_1_BIT ||
		memory->memoryTypeCount>VK_MAX_MEMORY_TYPES) return false;
	rc_receiver_targets_t pending={.device=device,.width=width,.height=height};
	const VkFormat formats[RC_RECEIVER_TARGET_COUNT]={VK_FORMAT_D32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32_UINT,
		VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
	for (unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i) {
		bool half=i>=RC_RECEIVER_HALF_POSITION, depth=i==RC_RECEIVER_DEPTH;
		VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT | (half ? VK_IMAGE_USAGE_STORAGE_BIT :
			depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		VkFormatFeatureFlags features=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (half ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT :
			depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
		/* Full-resolution deferred lighting and every reduced scale share this
		 * backing capacity. Dispatches and texture lookups use the active grid. */
		uint32_t w=width, h=height;
		if (!RCReceiverTargets_Image(&pending,physical,memory,i,formats[i],usage,features,
			depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,w,h)) goto fail;
	}
	VkAttachmentDescription attachments[3]={0};
	for (unsigned i=0;i<3;++i) {
		bool depth=i==RC_RECEIVER_DEPTH;
		attachments[i]=(VkAttachmentDescription){.format=formats[i],.samples=VK_SAMPLE_COUNT_1_BIT,
			.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	}
	VkAttachmentReference colors[2]={{RC_RECEIVER_NORMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{RC_RECEIVER_ID,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
	VkAttachmentReference depth={RC_RECEIVER_DEPTH,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=2,.pColorAttachments=colors,.pDepthStencilAttachment=&depth};
	VkSubpassDependency dependency={.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,
		.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		.dstStageMask=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
	VkRenderPassCreateInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=3,.pAttachments=attachments,
		.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dependency};
	if (vkCreateRenderPass(device,&pass,NULL,&pending.pass)!=VK_SUCCESS) goto fail;
	VkImageView views[3]={pending.images[0].view,pending.images[1].view,pending.images[2].view};
	VkFramebufferCreateInfo framebuffer={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pending.pass,
		.attachmentCount=3,.pAttachments=views,.width=width,.height=height,.layers=1};
	if (vkCreateFramebuffer(device,&framebuffer,NULL,&pending.framebuffer)!=VK_SUCCESS) goto fail;
	RCReceiverTargets_Destroy(out); *out=pending; return true;
fail:
	RCReceiverTargets_Destroy(&pending); return false;
}
