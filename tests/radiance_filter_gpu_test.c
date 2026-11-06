#include <vulkan/vulkan.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OK(x) assert ((x) == VK_SUCCESS)
int main (int argc, char **argv)
{
	assert (argc == 2 || (argc == 3 && strcmp (argv[2], "--gated") == 0));
	if (argc == 3 && !getenv ("QUIVER_TEST_VULKAN"))
	{
		puts ("SKIP: set QUIVER_TEST_VULKAN to enable Vulkan tests");
		return 77;
	}
	FILE *f = fopen (argv[1], "rb");
	assert (f);
	fseek (f, 0, SEEK_END);
	long n = ftell (f);
	rewind (f);
	unsigned *spv = malloc (n);
	assert (fread (spv, 1, n, f) == (size_t)n);
	fclose (f);
	VkInstance ins;
	OK (vkCreateInstance (
		&(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
								.pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1}},
		NULL, &ins));
	uint32_t count = 0;
	OK (vkEnumeratePhysicalDevices (ins, &count, NULL));
	assert (count > 0);
	VkPhysicalDevice *devices = calloc (count, sizeof (*devices));
	assert (devices);
	OK (vkEnumeratePhysicalDevices (ins, &count, devices));
	VkPhysicalDevice phys = devices[0];
	free (devices);
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties (phys, &nq, NULL);
	VkQueueFamilyProperties *q = calloc (nq, sizeof (*q));
	vkGetPhysicalDeviceQueueFamilyProperties (phys, &nq, q);
	uint32_t qi = 0;
	while (qi < nq && !(q[qi].queueFlags & VK_QUEUE_COMPUTE_BIT))
		qi++;
	assert (qi < nq);
	free (q);
	float	 pri = 1;
	VkDevice dev;
	OK (vkCreateDevice (
		phys,
		&(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
							  .queueCreateInfoCount = 1,
							  .pQueueCreateInfos =
								  &(VkDeviceQueueCreateInfo){
									  .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &pri}},
		NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue (dev, qi, 0, &queue);
	VkBuffer buf;
	OK (vkCreateBuffer (
		dev,
		&(VkBufferCreateInfo){
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 48, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
		NULL, &buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements (dev, buf, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties (phys, &mp);
	uint32_t mi = 0;
	while (mi < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << mi)) ||
									   (mp.memoryTypes[mi].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
										   (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
		mi++;
	assert (mi < mp.memoryTypeCount);
	VkDeviceMemory mem;
	OK (vkAllocateMemory (
		dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mi}, NULL, &mem));
	OK (vkBindBufferMemory (dev, buf, mem, 0));
	VkDescriptorSetLayout layout;
	OK (vkCreateDescriptorSetLayout (
		dev,
		&(VkDescriptorSetLayoutCreateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
										   .bindingCount = 1,
										   .pBindings = &(VkDescriptorSetLayoutBinding){.binding = 0,
																						.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
																						.descriptorCount = 1,
																						.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}},
		NULL, &layout));
	VkDescriptorPool pool;
	OK (vkCreateDescriptorPool (
		dev,
		&(VkDescriptorPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
									  .maxSets = 1,
									  .poolSizeCount = 1,
									  .pPoolSizes = &(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}},
		NULL, &pool));
	VkDescriptorSet set;
	OK (vkAllocateDescriptorSets (
		dev,
		&(VkDescriptorSetAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &layout},
		&set));
	vkUpdateDescriptorSets (
		dev, 1,
		&(VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
								.dstSet = set,
								.dstBinding = 0,
								.descriptorCount = 1,
								.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
								.pBufferInfo = &(VkDescriptorBufferInfo){buf, 0, 48}},
		0, NULL);
	VkShaderModule mod;
	OK (vkCreateShaderModule (dev, &(VkShaderModuleCreateInfo){.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = spv}, NULL, &mod));
	free (spv);
	VkPipelineLayout pl;
	OK (vkCreatePipelineLayout (
		dev, &(VkPipelineLayoutCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layout}, NULL, &pl));
	VkPipeline pipe;
	OK (vkCreateComputePipelines (
		dev, VK_NULL_HANDLE, 1,
		&(VkComputePipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"},
			.layout = pl},
		NULL, &pipe));
	VkCommandPool cp;
	OK (vkCreateCommandPool (dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi}, NULL, &cp));
	VkCommandBuffer cmd;
	OK (vkAllocateCommandBuffers (
		dev,
		&(VkCommandBufferAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1},
		&cmd));
	OK (vkBeginCommandBuffer (cmd, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	vkCmdDispatch (cmd, 1, 1, 1);
	vkCmdPipelineBarrier (
		cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
		&(VkMemoryBarrier){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT}, 0,
		NULL, 0, NULL);
	OK (vkEndCommandBuffer (cmd));
	OK (vkQueueSubmit (queue, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd}, VK_NULL_HANDLE));
	OK (vkQueueWaitIdle (queue));
	float *v;
	OK (vkMapMemory (dev, mem, 0, 48, 0, (void **)&v));
	printf ("uniform=%g fully_blocked=%g partial=%g boundary_delta=%g narrow=%g/%g wide=%g/%g\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
	assert (fabsf (v[0] - 1) < 1e-5f && v[1] == 0 && v[2] > 0 && v[2] < 1 && v[3] < 1e-4f && fabsf (v[6] - v[7]) < fabsf (v[4] - v[5]) * .15f);
	assert (v[8] < 1e-4f && v[9] == 0);
	vkUnmapMemory (dev, mem);
	vkDestroyCommandPool (dev, cp, NULL);
	vkDestroyPipeline (dev, pipe, NULL);
	vkDestroyPipelineLayout (dev, pl, NULL);
	vkDestroyShaderModule (dev, mod, NULL);
	vkDestroyDescriptorPool (dev, pool, NULL);
	vkDestroyDescriptorSetLayout (dev, layout, NULL);
	vkDestroyBuffer (dev, buf, NULL);
	vkFreeMemory (dev, mem, NULL);
	vkDestroyDevice (dev, NULL);
	vkDestroyInstance (ins, NULL);
	puts ("PASS");
	return 0;
}
