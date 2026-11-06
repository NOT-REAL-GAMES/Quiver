/* Optional Linux unit test of the actual tracked allocator, built with
 * function/data sections and --gc-sections; see vulkan_color_target_test.md. */
#include "../Quake/gl_rmisc.c"
#include <assert.h>
#include <stdint.h>
static bool fail_allocate;
static bool fail_descriptor;
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets (VkDevice device, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *set)
{
	(void)device; (void)info;
	if (fail_descriptor) { *set = (VkDescriptorSet)(uintptr_t)127; return VK_ERROR_OUT_OF_POOL_MEMORY; }
	*set = (VkDescriptorSet)(uintptr_t)2; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets (VkDevice device, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet *sets)
{ (void)device; (void)pool; assert (count == 1 && (uintptr_t)*sets == 2); return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory (VkDevice device, const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator, VkDeviceMemory *handle)
{
	(void)device; (void)info; (void)allocator;
	if (fail_allocate) { *handle = (VkDeviceMemory)(uintptr_t)127; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
	*handle = (VkDeviceMemory)(uintptr_t)1; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory (VkDevice device, VkDeviceMemory handle, const VkAllocationCallbacks *allocator)
{ (void)device; (void)allocator; assert ((uintptr_t)handle == 1); }
int main(void)
{
	VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4096};
	vulkan_memory_t memory = {0}, before = memory;
	fail_allocate = true;
	assert (R_TryAllocateVulkanMemory (&memory, &info, VULKAN_MEMORY_TYPE_DEVICE, &num_vulkan_misc_allocations) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert (!memcmp (&memory, &before, sizeof (memory)));
	assert (!Atomic_LoadUInt32 (&num_vulkan_misc_allocations) && !Atomic_LoadUInt64 (&total_device_vulkan_allocation_size));
	fail_allocate = false;
	assert (R_TryAllocateVulkanMemory (&memory, &info, VULKAN_MEMORY_TYPE_DEVICE, &num_vulkan_misc_allocations) == VK_SUCCESS);
	assert (Atomic_LoadUInt32 (&num_vulkan_misc_allocations) == 1 && Atomic_LoadUInt64 (&total_device_vulkan_allocation_size) == 4096);
	R_FreeVulkanMemory (&memory, &num_vulkan_misc_allocations);
	assert (!Atomic_LoadUInt32 (&num_vulkan_misc_allocations) && !Atomic_LoadUInt64 (&total_device_vulkan_allocation_size));
	vulkan_desc_set_layout_t layout = {0};
	layout.num_combined_image_samplers = 3; layout.num_input_attachments = 2;
	VkDescriptorSet descriptor = VK_NULL_HANDLE;
	fail_descriptor = true;
	assert (R_TryAllocateDescriptorSet (&layout, &descriptor) == VK_ERROR_OUT_OF_POOL_MEMORY);
	assert (!descriptor && !Atomic_LoadUInt32 (&num_vulkan_combined_image_samplers) && !Atomic_LoadUInt32 (&num_vulkan_input_attachments));
	fail_descriptor = false;
	assert (R_TryAllocateDescriptorSet (&layout, &descriptor) == VK_SUCCESS);
	assert (Atomic_LoadUInt32 (&num_vulkan_combined_image_samplers) == 3 && Atomic_LoadUInt32 (&num_vulkan_input_attachments) == 2);
	assert (R_TryAllocateDescriptorSet (&layout, &descriptor) == VK_ERROR_INITIALIZATION_FAILED);
	R_FreeDescriptorSet (descriptor, &layout);
	assert (!Atomic_LoadUInt32 (&num_vulkan_combined_image_samplers) && !Atomic_LoadUInt32 (&num_vulkan_input_attachments));
	puts ("PASS: failed Vulkan allocation preserves output and allocation accounting"); return 0;
}
