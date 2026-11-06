/* Rasterize a wall through the production world-normal function. Requires a
 * Vulkan implementation with fragment stores; no window or game data needed. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

static VkShaderModule LoadShader (VkDevice device, const char *path)
{
	FILE *f = fopen (path, "rb"); assert (f);
	assert (!fseek (f, 0, SEEK_END)); long size = ftell (f);
	assert (size > 0 && !(size % 4)); rewind (f);
	uint32_t *data = malloc ((size_t)size); assert (data);
	assert (fread (data, 1, (size_t)size, f) == (size_t)size); fclose (f);
	VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = (size_t)size, .pCode = data};
	VkShaderModule result = VK_NULL_HANDLE;
	assert (vkCreateShaderModule (device, &info, NULL, &result) == VK_SUCCESS); free (data);
	return result;
}

int main (int argc, char **argv)
{
	assert (argc == 3);
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkInstance instance; assert (vkCreateInstance (&ii, NULL, &instance) == VK_SUCCESS);
	uint32_t count = 0; assert (vkEnumeratePhysicalDevices (instance, &count, NULL) == VK_SUCCESS && count);
	VkPhysicalDevice *devices = calloc (count, sizeof (*devices)); assert (devices);
	assert (vkEnumeratePhysicalDevices (instance, &count, devices) == VK_SUCCESS);
	VkPhysicalDevice physical = devices[0]; free (devices);
	VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures (physical, &features);
	if (!features.fragmentStoresAndAtomics) { vkDestroyInstance (instance, NULL); puts ("fragment stores unsupported: skipped"); return 77; }
	uint32_t families_count = 0; vkGetPhysicalDeviceQueueFamilyProperties (physical, &families_count, NULL);
	VkQueueFamilyProperties *families = calloc (families_count, sizeof (*families)); assert (families);
	vkGetPhysicalDeviceQueueFamilyProperties (physical, &families_count, families);
	uint32_t family = 0; while (family < families_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
	free (families); assert (family < families_count);
	float priority = 1;
	VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
		.queueCount = 1, .pQueuePriorities = &priority};
	VkPhysicalDeviceFeatures enabled = {.fragmentStoresAndAtomics = VK_TRUE};
	VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qi, .pEnabledFeatures = &enabled};
	VkDevice device; assert (vkCreateDevice (physical, &di, NULL, &device) == VK_SUCCESS);
	VkQueue queue; vkGetDeviceQueue (device, family, 0, &queue);
	VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties (physical, &memory);
	VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
	VkBuffer buffer; assert (vkCreateBuffer (device, &bi, NULL, &buffer) == VK_SUCCESS);
	VkMemoryRequirements req; vkGetBufferMemoryRequirements (device, buffer, &req);
	uint32_t type = 0;
	while (type < memory.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
		(memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
		(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) ++type;
	assert (type < memory.memoryTypeCount);
	VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
	VkDeviceMemory mem; assert (vkAllocateMemory (device, &ai, NULL, &mem) == VK_SUCCESS);
	assert (vkBindBufferMemory (device, buffer, mem, 0) == VK_SUCCESS);
	float *result; assert (vkMapMemory (device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&result) == VK_SUCCESS);
	for (int i = 0; i < 4; ++i) result[i] = -5;
	VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
	VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
	VkDescriptorSetLayout layout; assert (vkCreateDescriptorSetLayout (device, &li, NULL, &layout) == VK_SUCCESS);
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
	VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
	VkDescriptorPool pool; assert (vkCreateDescriptorPool (device, &pi, NULL, &pool) == VK_SUCCESS);
	VkDescriptorSetAllocateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
		.descriptorSetCount = 1, .pSetLayouts = &layout};
	VkDescriptorSet set; assert (vkAllocateDescriptorSets (device, &si, &set) == VK_SUCCESS);
	VkDescriptorBufferInfo db = {buffer, 0, 16};
	VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &db};
	vkUpdateDescriptorSets (device, 1, &write, 0, NULL);
	VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layout};
	VkPipelineLayout pipeline_layout; assert (vkCreatePipelineLayout (device, &pli, NULL, &pipeline_layout) == VK_SUCCESS);
	VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
	VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &subpass};
	VkRenderPass render_pass; assert (vkCreateRenderPass (device, &ri, NULL, &render_pass) == VK_SUCCESS);
	VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = render_pass,
		.width = 1, .height = 1, .layers = 1};
	VkFramebuffer framebuffer; assert (vkCreateFramebuffer (device, &fi, NULL, &framebuffer) == VK_SUCCESS);
	VkShaderModule vert = LoadShader (device, argv[1]), frag = LoadShader (device, argv[2]);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
	VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkViewport vp = {0, 0, 1, 1, 0, 1}; VkRect2D scissor = {{0, 0}, {1, 1}};
	VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &scissor};
	VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1};
	VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendStateCreateInfo bs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
		.pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &bs, .layout = pipeline_layout, .renderPass = render_pass};
	VkPipeline pipeline; assert (vkCreateGraphicsPipelines (device, VK_NULL_HANDLE, 1, &gi, NULL, &pipeline) == VK_SUCCESS);
	VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
	VkCommandPool command_pool; assert (vkCreateCommandPool (device, &ci, NULL, &command_pool) == VK_SUCCESS);
	VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer command; assert (vkAllocateCommandBuffers (device, &cai, &command) == VK_SUCCESS);
	VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	assert (vkBeginCommandBuffer (command, &cbi) == VK_SUCCESS);
	VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = render_pass,
		.framebuffer = framebuffer, .renderArea = {{0, 0}, {1, 1}}};
	vkCmdBeginRenderPass (command, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline (command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets (command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0, NULL);
	vkCmdDraw (command, 3, 1, 0, 0);
	vkCmdEndRenderPass (command);
	VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
	assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle (queue) == VK_SUCCESS);
	assert (fabsf (result[0] + 1) < 1e-5f && fabsf (result[1]) < 1e-5f && fabsf (result[2]) < 1e-5f && fabsf (result[3] - 1) < 1e-5f);
	vkDestroyCommandPool (device, command_pool, NULL); vkDestroyPipeline (device, pipeline, NULL);
	vkDestroyShaderModule (device, vert, NULL); vkDestroyShaderModule (device, frag, NULL);
	vkDestroyFramebuffer (device, framebuffer, NULL); vkDestroyRenderPass (device, render_pass, NULL);
	vkDestroyPipelineLayout (device, pipeline_layout, NULL); vkDestroyDescriptorPool (device, pool, NULL);
	vkDestroyDescriptorSetLayout (device, layout, NULL); vkUnmapMemory (device, mem); vkDestroyBuffer (device, buffer, NULL);
	vkFreeMemory (device, mem, NULL); vkDestroyDevice (device, NULL); vkDestroyInstance (instance, NULL);
	puts ("PASS: rasterized world normal and light face the viewer"); return 0;
}
