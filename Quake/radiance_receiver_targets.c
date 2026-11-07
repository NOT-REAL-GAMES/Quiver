#include "radiance_receiver_targets.h"
#include "radiance_receiver_scale.h"
#include <string.h>

static void destroy_image(VkDevice device, rc_receiver_image_t *image)
{
	if (image->view) vkDestroyImageView(device, image->view, NULL);
	if (image->image) vkDestroyImage(device, image->image, NULL);
	if (image->memory) vkFreeMemory(device, image->memory, NULL);
}

void RCReceiverTargets_Destroy(rc_receiver_targets_t *t)
{
	if (!t || !t->device) return;
	if (t->framebuffer) vkDestroyFramebuffer(t->device, t->framebuffer, NULL);
	if (t->resolve_framebuffer) vkDestroyFramebuffer(t->device, t->resolve_framebuffer, NULL);
	if (t->pass) vkDestroyRenderPass(t->device, t->pass, NULL);
	if (t->resolve_pass) vkDestroyRenderPass(t->device, t->resolve_pass, NULL);
	for (unsigned i=0; i<RC_RECEIVER_TARGET_COUNT; ++i) destroy_image(t->device,&t->images[i]);
	for (unsigned i=0; i<3; ++i) destroy_image(t->device,&t->multisampled[i]);
	memset(t,0,sizeof(*t));
}

bool RCReceiverTargets_Image(rc_receiver_targets_t *t, VkPhysicalDevice physical,
	const VkPhysicalDeviceMemoryProperties *memory, rc_receiver_image_t *image, VkFormat format,
	VkImageUsageFlags usage, VkFormatFeatureFlags required, VkImageAspectFlags aspect,
	uint32_t width, uint32_t height, VkSampleCountFlagBits samples)
{
	VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
	if ((properties.optimalTilingFeatures & required) != required) return false;
	VkImageFormatProperties supported;
	if (vkGetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&supported) != VK_SUCCESS ||
		!(supported.sampleCounts & samples) || supported.maxExtent.width < width || supported.maxExtent.height < height) return false;
	VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D,
		.format=format, .extent={width,height,1}, .mipLevels=1, .arrayLayers=1, .samples=samples,
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

static bool create_pass(VkDevice device, const VkFormat *formats, VkSampleCountFlagBits samples,
	VkAttachmentLoadOp load, VkRenderPass *out)
{
	VkAttachmentDescription attachments[3]={0};
	for (unsigned i=0;i<3;++i) {
		bool depth=i==RC_RECEIVER_DEPTH;
		attachments[i]=(VkAttachmentDescription){.format=formats[i],.samples=samples,
			.loadOp=load,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
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
	return vkCreateRenderPass(device,&pass,NULL,out)==VK_SUCCESS;
}

static bool create_framebuffer(VkDevice device, VkRenderPass pass, const rc_receiver_image_t *images,
	uint32_t width, uint32_t height, VkFramebuffer *out)
{
	VkImageView views[3]={images[0].view,images[1].view,images[2].view};
	VkFramebufferCreateInfo framebuffer={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
		.attachmentCount=3,.pAttachments=views,.width=width,.height=height,.layers=1};
	return vkCreateFramebuffer(device,&framebuffer,NULL,out)==VK_SUCCESS;
}

bool RCReceiverTargets_Create(rc_receiver_targets_t *out, VkPhysicalDevice physical, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory, uint32_t width, uint32_t height, VkSampleCountFlagBits samples)
{
	if (!out || !physical || !device || !memory || !width || !height || !RCReceiverTargets_ValidSamples(samples) ||
		memory->memoryTypeCount>VK_MAX_MEMORY_TYPES) return false;
	rc_receiver_targets_t pending={.device=device,.samples=samples,.width=width,.height=height};
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
		if (!RCReceiverTargets_Image(&pending,physical,memory,&pending.images[i],formats[i],usage,features,
			depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,w,h,VK_SAMPLE_COUNT_1_BIT)) goto fail;
		if (samples!=VK_SAMPLE_COUNT_1_BIT && i<3 &&
			!RCReceiverTargets_Image(&pending,physical,memory,&pending.multisampled[i],formats[i],usage,features,
				depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,w,h,samples)) goto fail;
	}
	const bool multisampled=samples!=VK_SAMPLE_COUNT_1_BIT;
	if (!create_pass(device,formats,samples,VK_ATTACHMENT_LOAD_OP_CLEAR,&pending.pass)) goto fail;
	if (!create_framebuffer(device,pending.pass,multisampled ? pending.multisampled : pending.images,width,height,&pending.framebuffer)) goto fail;
	/* The collapse pass writes every pixel, so the old contents do not matter. */
	if (multisampled && (!create_pass(device,formats,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_DONT_CARE,&pending.resolve_pass) ||
		!create_framebuffer(device,pending.resolve_pass,pending.images,width,height,&pending.resolve_framebuffer))) goto fail;
	RCReceiverTargets_Destroy(out); *out=pending; return true;
fail:
	RCReceiverTargets_Destroy(&pending); return false;
}
