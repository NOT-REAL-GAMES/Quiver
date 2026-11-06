/* Fault-inject the real SSAO resource creation and rollback path. */
#include "../Quake/r_ssao.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

vulkanglobals_t vulkan_globals;
viddef_t vid;
atomic_uint32_t num_vulkan_misc_allocations;
static unsigned step, fail_at, serial, images, views_live, memory_live, descriptors_live, samplers;
static bool live_image[256], live_view[256], live_memory[256], live_descriptor[256], live_sampler[256];
#define ID(h) ((unsigned)(uintptr_t)(h))
#define HANDLE(t,n) ((t)(uintptr_t)(n))
static bool fault(void) { return ++step == fail_at; }
static unsigned next_id(void) { assert (++serial < 255); return serial; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice d,const VkSamplerCreateInfo *i,const VkAllocationCallbacks *a,VkSampler *out)
{ (void)d;(void)i;(void)a; if(fault()){*out=HANDLE(VkSampler,255);return VK_ERROR_OUT_OF_DEVICE_MEMORY;} unsigned n=next_id();live_sampler[n]=true;++samplers;*out=HANDLE(VkSampler,n);return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice d,VkSampler h,const VkAllocationCallbacks *a)
{ (void)d;(void)a;if(!h)return;unsigned n=ID(h);assert(n<255&&live_sampler[n]);live_sampler[n]=false;--samplers; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d,const VkImageCreateInfo *i,const VkAllocationCallbacks *a,VkImage *out)
{ (void)d;(void)i;(void)a;if(fault()){*out=HANDLE(VkImage,255);return VK_ERROR_OUT_OF_DEVICE_MEMORY;}unsigned n=next_id();live_image[n]=true;++images;*out=HANDLE(VkImage,n);return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d,VkImage h,const VkAllocationCallbacks *a)
{ (void)d;(void)a;if(!h)return;unsigned n=ID(h);assert(n<255&&live_image[n]);live_image[n]=false;--images; }
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice d,VkImage h,VkMemoryRequirements *r)
{ (void)d;assert(live_image[ID(h)]);*r=(VkMemoryRequirements){.size=4096,.alignment=64,.memoryTypeBits=1}; }
int GL_TryMemoryTypeFromProperties(uint32_t bits,VkFlags required,VkFlags preferred)
{ (void)bits;(void)required;(void)preferred;return fault()?-1:0; }
VkResult R_TryAllocateVulkanMemory(vulkan_memory_t *m,const VkMemoryAllocateInfo *i,vulkan_memory_type_t t,atomic_uint32_t *counter)
{ (void)counter;if(fault())return VK_ERROR_OUT_OF_DEVICE_MEMORY;unsigned n=next_id();live_memory[n]=true;++memory_live;*m=(vulkan_memory_t){.handle=HANDLE(VkDeviceMemory,n),.size=i->allocationSize,.type=t};return VK_SUCCESS; }
void R_FreeVulkanMemory(vulkan_memory_t *m,atomic_uint32_t *counter)
{ (void)counter;unsigned n=ID(m->handle);assert(n<255&&live_memory[n]);live_memory[n]=false;--memory_live;memset(m,0,sizeof(*m)); }
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o)
{ (void)d;(void)o;assert(live_image[ID(i)]&&live_memory[ID(m)]);return fault()?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *i,const VkAllocationCallbacks *a,VkImageView *out)
{ (void)d;(void)a;assert(i->image==HANDLE(VkImage,254)||live_image[ID(i->image)]);if(fault()){*out=HANDLE(VkImageView,255);return VK_ERROR_OUT_OF_DEVICE_MEMORY;}unsigned n=next_id();live_view[n]=true;++views_live;*out=HANDLE(VkImageView,n);return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d,VkImageView h,const VkAllocationCallbacks *a)
{ (void)d;(void)a;if(!h)return;unsigned n=ID(h);assert(n<255&&live_view[n]);live_view[n]=false;--views_live; }
VkResult R_TryAllocateDescriptorSet(vulkan_desc_set_layout_t *layout,VkDescriptorSet *out)
{ (void)layout;assert(!*out);if(fault())return VK_ERROR_OUT_OF_POOL_MEMORY;unsigned n=next_id();live_descriptor[n]=true;++descriptors_live;*out=HANDLE(VkDescriptorSet,n);return VK_SUCCESS; }
void R_FreeDescriptorSet(VkDescriptorSet h,vulkan_desc_set_layout_t *layout)
{ (void)layout;unsigned n=ID(h);assert(n<255&&live_descriptor[n]);live_descriptor[n]=false;--descriptors_live; }
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t c,const VkCopyDescriptorSet *p)
{ (void)d;(void)c;(void)p;for(uint32_t j=0;j<n;++j)assert(live_descriptor[ID(w[j].dstSet)]); }
static void clean(void)
{ assert(!images&&!views_live&&!memory_live&&!descriptors_live&&!samplers&&!scene_depth&&!sampler); }
int main(void)
{
	vid.render_width=640;vid.render_height=360;vulkan_globals.depth_format=VK_FORMAT_D32_SFLOAT_S8_UINT;vulkan_globals.sample_count=VK_SAMPLE_COUNT_4_BIT;r_ssao.value=1;
	unsigned total=0;
	for(unsigned failure=1;failure<256;++failure) {
		step=serial=0;fail_at=failure;VkResult result=R_TryCreateSSAO(HANDLE(VkImage,254));
		if(result==VK_SUCCESS){total=step;R_DestroySSAO();clean();break;}
		clean();
		/* Every failed attempt permits an immediate full retry. */
		step=serial=fail_at=0;assert(R_TryCreateSSAO(HANDLE(VkImage,254))==VK_SUCCESS);
		assert(R_TryCreateSSAO(HANDLE(VkImage,254))==VK_ERROR_INITIALIZATION_FAILED);
		R_DestroySSAO();clean();
	}
	assert(total>50);r_ssao.value=0;step=serial=fail_at=0;
	assert(R_TryCreateSSAO(HANDLE(VkImage,254))==VK_SUCCESS && step==0);clean();
	printf("PASS: SSAO rollback/retry at %u resource failure points\n",total);return 0;
}
