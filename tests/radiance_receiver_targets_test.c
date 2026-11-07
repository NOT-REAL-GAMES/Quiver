/* CPU-only failure injection for the experimental receiver target manager. */
#include "radiance_receiver_targets.h"
#include "radiance_receiver_scale.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned operation, fail_at, serial, images, allocations, views, passes, framebuffers;
static bool supported=true;
static VkExtent3D extents[RC_RECEIVER_TARGET_COUNT];
static unsigned extent_count;
static void scale_partitions(void)
{
	const float divisors[]={1,1.5f,2,3,4};
	for(unsigned d=0;d<5;++d) {
		float divisor=divisors[d]; assert(RCReceiver_ValidDivisor(divisor));
		for(uint32_t full=0;full<=257;++full) {
			uint32_t extent=RCReceiver_ScaledExtent(full,divisor),cursor=0;
			assert(extent<=full);
			for(uint32_t low=0;low<extent;++low) {
				uint32_t first=divisor==1.5f?low*3u/2u:low*(uint32_t)divisor;
				uint32_t end=divisor==1.5f?(low+1u)*3u/2u:(low+1u)*(uint32_t)divisor;
				if(end>full)end=full;
				assert(first==cursor && end>first && end-first<=(divisor==1.5f?2u:(uint32_t)divisor));
				for(uint32_t pixel=first;pixel<end;++pixel) {
					uint32_t inverse=divisor==1.5f?(pixel*2u+1u)/3u:pixel/(uint32_t)divisor;
					assert(inverse==low);
				}
				cursor=end;
			}
			assert(cursor==full); /* Exact coverage, no overlap or missing odd edge. */
		}
		const uint32_t large[]={UINT32_MAX,UINT32_MAX-1u,UINT32_MAX-2u,16777215u,16777216u,16777217u};
		for(unsigned n=0;n<sizeof(large)/sizeof(large[0]);++n) {
			uint32_t full=large[n];
			uint32_t expected=divisor==1.5f?full-full/3u:(uint32_t)(((uint64_t)full+(uint32_t)divisor-1u)/(uint32_t)divisor);
			assert(RCReceiver_ScaledExtent(full,divisor)==expected);
		}
	}
	const float invalid[]={0,.5f,.99f,-1,16.5f,17,NAN,INFINITY};
	for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
		assert(!RCReceiver_ValidDivisor(invalid[i]) && RCReceiver_ScaledExtent(123,invalid[i])==0);
}
static bool fail(void) { return ++operation==fail_at; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *out)
{ (void)p;(void)f; memset(out,0,sizeof(*out)); out->optimalTilingFeatures=supported ? ~0u : 0; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice p, VkFormat f, VkImageType type,
	VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *out)
{ (void)p;(void)f;(void)type;(void)tiling;(void)usage;(void)flags; memset(out,0,sizeof(*out)); out->maxExtent=(VkExtent3D){10000,10000,1}; out->sampleCounts=VK_SAMPLE_COUNT_1_BIT|VK_SAMPLE_COUNT_4_BIT; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d,const VkImageCreateInfo *ci,const VkAllocationCallbacks *a,VkImage *out)
{ (void)d;(void)a; if(fail()) return VK_ERROR_OUT_OF_HOST_MEMORY; *out=(VkImage)(uintptr_t)++serial; ++images; if(extent_count<RC_RECEIVER_TARGET_COUNT) extents[extent_count++]=ci->extent; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks *a) { (void)d;(void)image;(void)a; assert(images); --images; }
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice d,VkImage image,VkMemoryRequirements *out)
{ (void)d;(void)image; *out=(VkMemoryRequirements){.size=1024,.alignment=1,.memoryTypeBits=1}; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *out)
{ (void)d;(void)i;(void)a; if(fail()) return VK_ERROR_OUT_OF_HOST_MEMORY; *out=(VkDeviceMemory)(uintptr_t)++serial; ++allocations; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a) { (void)d;(void)m;(void)a; assert(allocations); --allocations; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o)
{ (void)d;(void)i;(void)m;(void)o; return fail() ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *i,const VkAllocationCallbacks *a,VkImageView *out)
{ (void)d;(void)i;(void)a; if(fail()) return VK_ERROR_OUT_OF_HOST_MEMORY; *out=(VkImageView)(uintptr_t)++serial; ++views; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d,VkImageView v,const VkAllocationCallbacks *a) { (void)d;(void)v;(void)a; assert(views); --views; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *i,const VkAllocationCallbacks *a,VkRenderPass *out)
{ (void)d;(void)a; assert(i->attachmentCount==3 && i->subpassCount==1 && i->pSubpasses[0].colorAttachmentCount==2); if(fail()) return VK_ERROR_OUT_OF_HOST_MEMORY; *out=(VkRenderPass)(uintptr_t)++serial; ++passes; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice d,VkRenderPass p,const VkAllocationCallbacks *a) { (void)d;(void)p;(void)a; assert(passes); --passes; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo *i,const VkAllocationCallbacks *a,VkFramebuffer *out)
{ (void)d;(void)a; assert(i->attachmentCount==3 && i->width==1279 && i->height==719); if(fail()) return VK_ERROR_OUT_OF_HOST_MEMORY; *out=(VkFramebuffer)(uintptr_t)++serial; ++framebuffers; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d,VkFramebuffer f,const VkAllocationCallbacks *a) { (void)d;(void)f;(void)a; assert(framebuffers); --framebuffers; }

int main(void)
{
	scale_partitions();
	VkPhysicalDevice p=(VkPhysicalDevice)(uintptr_t)1; VkDevice d=(VkDevice)(uintptr_t)1;
	VkPhysicalDeviceMemoryProperties mem={.memoryTypeCount=1,.memoryTypes={{.propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT}}};
	rc_receiver_targets_t target={0};
	assert(RCReceiverTargets_Create(&target,p,d,&mem,1279,719,VK_SAMPLE_COUNT_1_BIT));
	assert(operation==50 && extent_count==RC_RECEIVER_TARGET_COUNT && extents[0].width==1279 && extents[2].height==719 &&
		extents[3].width==1279 && extents[RC_RECEIVER_TARGET_COUNT-1].height==719);
	VkFramebuffer original=target.framebuffer;
	for(unsigned i=1;i<=50;++i) {
		operation=0; fail_at=i;
		assert(!RCReceiverTargets_Create(&target,p,d,&mem,1279,719,VK_SAMPLE_COUNT_1_BIT));
		assert(target.framebuffer==original && images==RC_RECEIVER_TARGET_COUNT && allocations==RC_RECEIVER_TARGET_COUNT && views==RC_RECEIVER_TARGET_COUNT && passes==1 && framebuffers==1);
	}
	fail_at=0; operation=0;
	assert(!RCReceiverTargets_Create(&target,p,d,&mem,1279,719,(VkSampleCountFlagBits)3) && operation==0);
	assert(!RCReceiverTargets_Create(&target,p,d,&mem,1279,719,VK_SAMPLE_COUNT_16_BIT) && operation==0);
	assert(!RCReceiverTargets_Create(&target,p,d,&mem,1279,719,VK_SAMPLE_COUNT_8_BIT) && target.framebuffer==original);
	{ /* Multisampled targets add three images, a second pass and its framebuffer. */
		const unsigned count=RC_RECEIVER_TARGET_COUNT+3;
		rc_receiver_targets_t multi={0}; operation=0;
		assert(RCReceiverTargets_Create(&multi,p,d,&mem,1279,719,VK_SAMPLE_COUNT_4_BIT));
		assert(multi.samples==VK_SAMPLE_COUNT_4_BIT && multi.pass && multi.resolve_pass && multi.framebuffer && multi.resolve_framebuffer);
		for(unsigned i=0;i<3;++i) assert(multi.multisampled[i].view && multi.multisampled[i].view!=multi.images[i].view);
		assert(images==RC_RECEIVER_TARGET_COUNT+count && passes==3 && framebuffers==3);
		const unsigned steps=operation;
		RCReceiverTargets_Destroy(&multi);
		assert(images==RC_RECEIVER_TARGET_COUNT && passes==1 && framebuffers==1);
		for(unsigned i=1;i<=steps;++i) {
			operation=0; fail_at=i;
			assert(!RCReceiverTargets_Create(&multi,p,d,&mem,1279,719,VK_SAMPLE_COUNT_4_BIT));
			assert(images==RC_RECEIVER_TARGET_COUNT && views==RC_RECEIVER_TARGET_COUNT && passes==1 && framebuffers==1);
		}
		fail_at=0; operation=0;
	}
	supported=false;
	assert(!RCReceiverTargets_Create(&target,p,d,&mem,1279,719,VK_SAMPLE_COUNT_1_BIT) && target.framebuffer==original);
	supported=true; operation=0;
	assert(RCReceiverTargets_Create(&target,p,d,&mem,1279,719,VK_SAMPLE_COUNT_1_BIT));
	assert(target.framebuffer!=original && images==RC_RECEIVER_TARGET_COUNT && allocations==RC_RECEIVER_TARGET_COUNT && views==RC_RECEIVER_TARGET_COUNT && passes==1 && framebuffers==1);
	RCReceiverTargets_Destroy(&target);
	assert(!images && !allocations && !views && !passes && !framebuffers);
	puts("PASS: receiver targets preserve fallback across allocation failures and reject MSAA");
	return 0;
}
