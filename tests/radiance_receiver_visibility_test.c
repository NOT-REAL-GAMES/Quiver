/* Standalone test of the prototype's actual Vulkan compute classifier. */
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_OK(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d\n", #expr, (int)r_); exit(1); } } while (0)

int main(int argc, char **argv)
{
	if (argc == 3) {
        if (strcmp(argv[2], "--gated") != 0) return 2;
        if (!getenv("QUIVER_TEST_VULKAN")) { puts("SKIP: set QUIVER_TEST_VULKAN=1 to run"); return 77; }
    } else if (argc != 2) return 2;
	FILE *f = fopen(argv[1], "rb"); assert(f);
	assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); assert(size > 0 && !(size % 4)); rewind(f);
	uint32_t *words = malloc((size_t)size); assert(words && fread(words, 1, (size_t)size, f) == (size_t)size); fclose(f);
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkInstance instance; VK_OK(vkCreateInstance(&ici, NULL, &instance));
	uint32_t n = 0; VK_OK(vkEnumeratePhysicalDevices(instance, &n, NULL)); assert(n);
	VkPhysicalDevice *devices = malloc(n * sizeof(*devices)); assert(devices); VK_OK(vkEnumeratePhysicalDevices(instance, &n, devices));
	VkPhysicalDevice physical = devices[0]; free(devices);
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL); assert(n);
	VkQueueFamilyProperties *families = malloc(n * sizeof(*families)); assert(families);
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families);
	uint32_t family = 0; while (family < n && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
	free(families); assert(family < n);
	float priority = 1;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
		.queueCount = 1, .pQueuePriorities = &priority};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
	VkDevice device; VK_OK(vkCreateDevice(physical, &dci, NULL, &device));
	VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
	VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16 * sizeof(uint32_t),
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkBuffer buffer; VK_OK(vkCreateBuffer(device, &bi, NULL, &buffer));
	VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffer, &req);
	VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(physical, &props);
	uint32_t memory_type = 0;
	while (memory_type < props.memoryTypeCount && (!(req.memoryTypeBits & (1u << memory_type)) ||
		(props.memoryTypes[memory_type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
		(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) ++memory_type;
	assert(memory_type < props.memoryTypeCount);
	VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = memory_type};
	VkDeviceMemory memory; VK_OK(vkAllocateMemory(device, &ai, NULL, &memory)); VK_OK(vkBindBufferMemory(device, buffer, memory, 0));
	void *mapped; VK_OK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
	VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
	VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
	VkDescriptorSetLayout layout; VK_OK(vkCreateDescriptorSetLayout(device, &li, NULL, &layout));
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
	VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
	VkDescriptorPool pool; VK_OK(vkCreateDescriptorPool(device, &pi, NULL, &pool));
	VkDescriptorSetAllocateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
		.descriptorSetCount = 1, .pSetLayouts = &layout};
	VkDescriptorSet set; VK_OK(vkAllocateDescriptorSets(device, &si, &set));
	VkDescriptorBufferInfo db = {buffer, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &db};
	vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
	VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = words};
	VkShaderModule module; VK_OK(vkCreateShaderModule(device, &sm, NULL, &module)); free(words);
	VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layout};
	VkPipelineLayout pipeline_layout; VK_OK(vkCreatePipelineLayout(device, &pli, NULL, &pipeline_layout));
	VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module, .pName = "main"}, .layout = pipeline_layout};
	VkPipeline pipeline; VK_OK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
	VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
	VkCommandPool command_pool; VK_OK(vkCreateCommandPool(device, &cpi, NULL, &command_pool));
	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer command; VK_OK(vkAllocateCommandBuffers(device, &cbai, &command));
	VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK_OK(vkBeginCommandBuffer(command, &begin));
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, NULL);
	vkCmdDispatch(command, 1, 1, 1);
	VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	VK_OK(vkEndCommandBuffer(command));
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
	VK_OK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); VK_OK(vkQueueWaitIdle(queue));
	const uint32_t expected[16] = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	const uint32_t *result = mapped;
	for (unsigned i = 0; i < 16; ++i) if (result[i] != expected[i]) {
		fprintf(stderr, "case %u: got %u, expected %u\n", i, result[i], expected[i]); return 1;
	}
	puts("PASS: receiver probe visibility compatibility");
	vkDestroyCommandPool(device, command_pool, NULL); vkDestroyPipeline(device, pipeline, NULL);
	vkDestroyPipelineLayout(device, pipeline_layout, NULL); vkDestroyShaderModule(device, module, NULL);
	vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, layout, NULL);
	vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, memory, NULL);
	vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
	return 0;
}
