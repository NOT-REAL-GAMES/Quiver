/* Headless Vulkan test: actual atlas upload/readback and fenced frame slots. */
#include "radiance_gpu_inputs.h"
#include "radiance_vulkan.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; } readback_t;

static rc_vk_shader_t ReadShader (const char *path)
{
	FILE *file = fopen (path, "rb"); assert (file);
	assert (fseek (file, 0, SEEK_END) == 0);
	long size = ftell (file); assert (size > 0 && size % 4 == 0);
	assert (fseek (file, 0, SEEK_SET) == 0);
	uint32_t *words = malloc ((size_t)size); assert (words);
	assert (fread (words, 1, (size_t)size, file) == (size_t)size);
	fclose (file);
	return (rc_vk_shader_t){words, (size_t)size};
}

static void ReadAtlas (VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer command,
	radiance_gpu_inputs_t *inputs, unsigned frame, const readback_t *readback, const uint8_t *expected, size_t size)
{
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	assert (RadianceInputs_RecordUpload (inputs, frame, command));
	radiance_input_frame_t *f = &inputs->frame[frame];
	VkImageMemoryBarrier image = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = f->atlas, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, f->layers}};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image);
	VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, f->layers},
		.imageExtent = {f->width, f->height, 1}};
	vkCmdCopyImageToBuffer (command, f->atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &copy);
	image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; image.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; image.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkBufferMemoryBarrier buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = readback->buffer, .offset = 0, .size = VK_WHOLE_SIZE};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 1, &buffer, 1, &image);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
	assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
	RadianceInputs_MarkSubmitted (inputs, frame);
	assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
	assert (memcmp (readback->mapped, expected, size) == 0);
}

int main (int argc, char **argv)
{
	if (argc > 1 && !strcmp (argv[argc - 1], "--gated"))
	{
		const char *enabled = getenv ("QUIVER_TEST_VULKAN");
		if (!enabled || strcmp (enabled, "1")) { puts ("Vulkan test disabled: skipped"); return 77; }
		--argc;
	}
	assert (argc >= 3 && argc <= 6); /* trace, merge, optional bounce, receiver and topology SPIR-V */
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkInstance instance = VK_NULL_HANDLE;
	assert (vkCreateInstance (&instance_info, NULL, &instance) == VK_SUCCESS);
	uint32_t count = 0; assert (vkEnumeratePhysicalDevices (instance, &count, NULL) == VK_SUCCESS && count);
	VkPhysicalDevice *devices = calloc (count, sizeof (*devices)); assert (devices);
	assert (vkEnumeratePhysicalDevices (instance, &count, devices) == VK_SUCCESS);
	VkPhysicalDevice physical = devices[0]; free (devices);
	VkPhysicalDeviceProperties properties; VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceProperties (physical, &properties); vkGetPhysicalDeviceMemoryProperties (physical, &memory);
	uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties (physical, &family_count, NULL);
	VkQueueFamilyProperties *families = calloc (family_count, sizeof (*families)); assert (families);
	vkGetPhysicalDeviceQueueFamilyProperties (physical, &family_count, families);
	uint32_t family;
	for (family = 0; family < family_count; ++family)
		if ((families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
			(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) break;
	free (families); assert (family < family_count);
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
	VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
	VkDevice device = VK_NULL_HANDLE; assert (vkCreateDevice (physical, &device_info, NULL, &device) == VK_SUCCESS);
	VkQueue queue; vkGetDeviceQueue (device, family, 0, &queue);
	VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
	VkCommandPool pool = VK_NULL_HANDLE; assert (vkCreateCommandPool (device, &pool_info, NULL, &pool) == VK_SUCCESS);
	VkCommandBufferAllocateInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer command; assert (vkAllocateCommandBuffers (device, &cmd_info, &command) == VK_SUCCESS);
	readback_t readback = {0};
	VkBufferCreateInfo binfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	assert (vkCreateBuffer (device, &binfo, NULL, &readback.buffer) == VK_SUCCESS);
	VkMemoryRequirements req; vkGetBufferMemoryRequirements (device, readback.buffer, &req);
	uint32_t type;
	for (type = 0; type < memory.memoryTypeCount; ++type)
		if ((req.memoryTypeBits & (1u << type)) &&
			(memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
	assert (type < memory.memoryTypeCount);
	VkMemoryAllocateInfo minfo = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
	assert (vkAllocateMemory (device, &minfo, NULL, &readback.memory) == VK_SUCCESS);
	assert (vkBindBufferMemory (device, readback.buffer, readback.memory, 0) == VK_SUCCESS);
	assert (vkMapMemory (device, readback.memory, 0, VK_WHOLE_SIZE, 0, &readback.mapped) == VK_SUCCESS);
	radiance_gpu_inputs_t inputs;
	assert (RadianceInputs_Create (&inputs, physical, device, &memory, &properties.limits, 2));
	radiance_light_t light = {0}; light.position_radius[3] = 100; light.color_intensity[0] = 1;
	radiance_material_t material = {0}; material.albedo_alpha[0] = material.albedo_alpha[3] = 1;
	uint8_t pixels[32]; for (int i = 0; i < 32; ++i) pixels[i] = (uint8_t)(i * 7);
	uint32_t offsets[] = {0, 1}, indices[] = {0};
	radiance_input_snapshot_t snapshot = {.lights = &light, .light_count = 1, .materials = &material, .material_count = 1,
		.grid_offsets = offsets, .grid_offset_count = 2, .grid_indices = indices, .grid_index_count = 1,
		.atlas_rgba = pixels, .atlas_width = 2, .atlas_height = 2, .atlas_layers = 2};
	snapshot.parameters.counts[0] = 1; snapshot.parameters.counts[1] = 1; snapshot.parameters.counts[2] = 2; snapshot.parameters.counts[3] = 1;
	for (int i = 0; i < 3; ++i) { snapshot.parameters.grid_dimensions[i] = 1; snapshot.parameters.grid_inv_cell_size[i] = 1; }
	snapshot.parameters.grid_dimensions[3] = 1;
	radiance_input_snapshot_t invalid_gain = snapshot;
	invalid_gain.parameters.counts[3] |= 8u;
	invalid_gain.parameters.bounce_parameters[0] = NAN;
	assert (!RadianceInputs_Update (&inputs, 0, &invalid_gain));
	invalid_gain.parameters.bounce_parameters[0] = 1000.0f;
	assert (RadianceInputs_Update (&inputs, 0, &invalid_gain));
	invalid_gain.parameters.bounce_parameters[0] = -.01f;
	assert (!RadianceInputs_Update (&inputs, 0, &invalid_gain));
	invalid_gain = snapshot;
	invalid_gain.parameters.bounce_parameters[1] = NAN;
	assert (!RadianceInputs_Update (&inputs, 0, &invalid_gain));
	invalid_gain.parameters.bounce_parameters[1] = 1.01f;
	assert (!RadianceInputs_Update (&inputs, 0, &invalid_gain));
	invalid_gain = snapshot;
	invalid_gain.parameters.gi_camera_fade[2] = NAN;
	assert (!RadianceInputs_Update (&inputs, 0, &invalid_gain));
	invalid_gain = snapshot;
	invalid_gain.parameters.gi_camera_fade[3] = -1.0f;
	assert (!RadianceInputs_Update (&inputs, 0, &invalid_gain));
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	assert (RadianceInputs_Descriptor (&inputs, 0) && inputs.frame[0].atlas_dirty && RadianceInputs_AllocationBytes (&inputs) > 0);
	/* Dropping a recorded command buffer must leave the atlas undefined and
	 * dirty; a retry must still perform the initial upload transition. */
	VkCommandBufferBeginInfo abandoned = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RadianceInputs_RecordUpload (&inputs, 0, command) && inputs.frame[0].atlas_recorded);
	assert (!RadianceInputs_Update (&inputs, 0, &snapshot));
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	RadianceInputs_DiscardRecorded (&inputs, 0);
	assert (inputs.frame[0].atlas_dirty && !inputs.frame[0].atlas_uploaded);
	ReadAtlas (device, queue, pool, command, &inputs, 0, &readback, pixels, sizeof (pixels));
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && !inputs.frame[0].atlas_dirty);
	pixels[0] ^= 0xff;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && inputs.frame[0].atlas_dirty);
	ReadAtlas (device, queue, pool, command, &inputs, 0, &readback, pixels, sizeof (pixels));
	assert (RadianceInputs_Update (&inputs, 1, &snapshot));
	assert (RadianceInputs_Descriptor (&inputs, 0) != RadianceInputs_Descriptor (&inputs, 1));
	ReadAtlas (device, queue, pool, command, &inputs, 1, &readback, pixels, sizeof (pixels));
	/* Versioned snapshots avoid reading the GPU staging allocation. Every
	 * slot tracks its own copy; changed revisions and discarded uploads must
	 * still replace the actual image. Zero switches back to byte comparison. */
	snapshot.atlas_revision = 1;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && inputs.frame[0].atlas_dirty);
	ReadAtlas (device, queue, pool, command, &inputs, 0, &readback, pixels, sizeof (pixels));
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && !inputs.frame[0].atlas_dirty);
	pixels[1] ^= 0x80; snapshot.atlas_revision = 2;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && inputs.frame[0].atlas_dirty);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RadianceInputs_RecordUpload (&inputs, 0, command));
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	RadianceInputs_DiscardRecorded (&inputs, 0);
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && inputs.frame[0].atlas_dirty);
	ReadAtlas (device, queue, pool, command, &inputs, 0, &readback, pixels, sizeof (pixels));
	assert (RadianceInputs_Update (&inputs, 1, &snapshot) && inputs.frame[1].atlas_dirty);
	ReadAtlas (device, queue, pool, command, &inputs, 1, &readback, pixels, sizeof (pixels));
	/* Each fenced frame slot retains its own coherent grid snapshot. The
	 * sentinel detects a redundant copy without reading write-combined memory
	 * in the production path. New identities, resizing and unversioned input
	 * must still copy, even after a discarded recording. */
	snapshot.grid_revision = 101;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	assert (inputs.frame[0].grid_revision == 101 && ((uint32_t *)inputs.frame[0].buffers[3].mapped)[1] == 1);
	((uint32_t *)inputs.frame[0].buffers[3].mapped)[1] = 1234;
	((uint32_t *)inputs.frame[0].buffers[4].mapped)[0] = 4321;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	assert (((uint32_t *)inputs.frame[0].buffers[3].mapped)[1] == 1234 &&
		((uint32_t *)inputs.frame[0].buffers[4].mapped)[0] == 4321);
	assert (RadianceInputs_Update (&inputs, 1, &snapshot));
	assert (inputs.frame[1].grid_revision == 101 && ((uint32_t *)inputs.frame[1].buffers[3].mapped)[1] == 1);
	snapshot.grid_revision = 102;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	assert (((uint32_t *)inputs.frame[0].buffers[3].mapped)[1] == 1 &&
		((uint32_t *)inputs.frame[0].buffers[4].mapped)[0] == 0);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RadianceInputs_RecordUpload (&inputs, 0, command));
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	RadianceInputs_DiscardRecorded (&inputs, 0);
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && inputs.frame[0].grid_revision == 102);
	uint32_t large_offsets[] = {0,1,2,3,4,5}, large_indices[] = {0,0,0,0,0};
	snapshot.grid_offsets = large_offsets; snapshot.grid_offset_count = 6;
	snapshot.grid_indices = large_indices; snapshot.grid_index_count = 5;
	snapshot.parameters.grid_dimensions[0] = 5; snapshot.parameters.grid_dimensions[3] = 5;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot)); /* same identity, changed size */
	assert (((uint32_t *)inputs.frame[0].buffers[3].mapped)[5] == 5 &&
		inputs.frame[0].buffers[3].size >= sizeof (large_offsets) && inputs.frame[0].buffers[4].size >= sizeof (large_indices));
	snapshot.grid_offsets = offsets; snapshot.grid_offset_count = 2;
	snapshot.grid_indices = indices; snapshot.grid_index_count = 1;
	snapshot.parameters.grid_dimensions[0] = 1; snapshot.parameters.grid_dimensions[3] = 1;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	uint32_t failure_offsets[65] = {0};
	snapshot.grid_revision = 103; snapshot.grid_offsets = failure_offsets; snapshot.grid_offset_count = 65;
	snapshot.grid_indices = NULL; snapshot.grid_index_count = 0;
	snapshot.parameters.grid_dimensions[0] = 64; snapshot.parameters.grid_dimensions[3] = 0;
	uint32_t saved_max_storage = inputs.limits.maxStorageBufferRange;
	inputs.limits.maxStorageBufferRange = 128; /* force grid-buffer growth failure */
	assert (!RadianceInputs_Update (&inputs, 0, &snapshot) && !inputs.frame[0].grid_revision);
	inputs.limits.maxStorageBufferRange = saved_max_storage;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && inputs.frame[0].grid_revision == 103);
	snapshot.grid_offsets = offsets; snapshot.grid_offset_count = 2;
	snapshot.grid_indices = indices; snapshot.grid_index_count = 1;
	snapshot.parameters.grid_dimensions[0] = 1; snapshot.parameters.grid_dimensions[3] = 1;
	snapshot.grid_revision = 0;
	((uint32_t *)inputs.frame[0].buffers[3].mapped)[1] = 5678;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	assert (((uint32_t *)inputs.frame[0].buffers[3].mapped)[1] == 1);
	snapshot.atlas_revision = 0;
	pixels[2] ^= 0x40;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot) && inputs.frame[0].atlas_dirty);
	ReadAtlas (device, queue, pool, command, &inputs, 0, &readback, pixels, sizeof (pixels));
	snapshot.atlas_width = 4; snapshot.atlas_height = 1;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	ReadAtlas (device, queue, pool, command, &inputs, 0, &readback, pixels, sizeof (pixels));
	indices[0] = 1; assert (!RadianceInputs_Update (&inputs, 0, &snapshot));
	assert (!RadianceInputs_Descriptor (&inputs, 0));
	/* Dispatch the actual sparse trace and far-to-near merge shaders against
	 * an empty scene. The outer interval must terminate in the explicit sky,
	 * proving real descriptors, atlas upload, compute and readback all work. */
	indices[0] = 0;
	snapshot.parameters.sky_radiance[0] = snapshot.parameters.ground_radiance[0] = 1;
	snapshot.parameters.sky_radiance[1] = snapshot.parameters.ground_radiance[1] = 0.5f;
	snapshot.parameters.sky_radiance[2] = snapshot.parameters.ground_radiance[2] = 0.25f;
	snapshot.parameters.sky_radiance[3] = snapshot.parameters.ground_radiance[3] = 1;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	VkDescriptorSetLayoutBinding scene_bindings[4] = {0};
	for (uint32_t i = 0; i < 4; ++i)
	{
		scene_bindings[i].binding = i; scene_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		scene_bindings[i].descriptorCount = 1; scene_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo slinfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 4, .pBindings = scene_bindings};
	VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE;
	assert (vkCreateDescriptorSetLayout (device, &slinfo, NULL, &scene_layout) == VK_SUCCESS);
	VkDescriptorPoolSize scene_pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
	VkDescriptorPoolCreateInfo spinfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &scene_pool_size};
	VkDescriptorPool scene_pool = VK_NULL_HANDLE; assert (vkCreateDescriptorPool (device, &spinfo, NULL, &scene_pool) == VK_SUCCESS);
	VkDescriptorSetAllocateInfo sdinfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene_pool, .descriptorSetCount = 1, .pSetLayouts = &scene_layout};
	VkDescriptorSet scene_set; assert (vkAllocateDescriptorSets (device, &sdinfo, &scene_set) == VK_SUCCESS);
	/* A zeroed storage-capable buffer supplies empty nodes, indices, triangles
	 * and counts. Its usage includes TRANSFER_DST for the earlier readback. */
	VkBufferCreateInfo empty_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkBuffer empty = VK_NULL_HANDLE; assert (vkCreateBuffer (device, &empty_info, NULL, &empty) == VK_SUCCESS);
	VkMemoryRequirements empty_req; vkGetBufferMemoryRequirements (device, empty, &empty_req);
	for (type = 0; type < memory.memoryTypeCount; ++type)
		if ((empty_req.memoryTypeBits & (1u << type)) &&
			(memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
	assert (type < memory.memoryTypeCount);
	VkMemoryAllocateInfo empty_alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = empty_req.size, .memoryTypeIndex = type};
	VkDeviceMemory empty_memory = VK_NULL_HANDLE; assert (vkAllocateMemory (device, &empty_alloc, NULL, &empty_memory) == VK_SUCCESS);
	assert (vkBindBufferMemory (device, empty, empty_memory, 0) == VK_SUCCESS);
	void *empty_mapped; assert (vkMapMemory (device, empty_memory, 0, VK_WHOLE_SIZE, 0, &empty_mapped) == VK_SUCCESS);
	memset (empty_mapped, 0, 16);
	VkDescriptorBufferInfo empty_descriptor = {empty, 0, 16}; VkWriteDescriptorSet scene_writes[4] = {0};
	for (uint32_t i = 0; i < 4; ++i)
	{
		scene_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; scene_writes[i].dstSet = scene_set;
		scene_writes[i].dstBinding = i; scene_writes[i].descriptorCount = 1;
		scene_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; scene_writes[i].pBufferInfo = &empty_descriptor;
	}
	vkUpdateDescriptorSets (device, 4, scene_writes, 0, NULL);
	uint32_t capacities[RC_CASCADE_COUNT] = {1, 1, 1, 1, 1};
	/* Match the renderer's coarsest 2x2 angular sampling. */
	rc_sparse_t *sparse = RC_SparseCreate (32, 2, 32, 64, capacities); assert (sparse);
	float camera[3] = {0}, mins[3] = {-1, -1, -1}, maxs[3] = {1, 1, 1};
	assert (RC_SparseBeginFrame (sparse, camera) && RC_SparseMarkAABB (sparse, mins, maxs, 0) && RC_SparseResolve (sparse));
	rc_vk_shader_t trace = ReadShader (argv[1]), merge = ReadShader (argv[2]);
	rc_vulkan_t cascade;
	assert (RCVk_Create (&cascade, device, &memory, &properties.limits, sparse, 2, scene_layout,
		VK_NULL_HANDLE, inputs.layout, trace, (rc_vk_shader_t){0}, merge, VK_NULL_HANDLE));
	for (unsigned i = 0; i < 6; ++i) assert (cascade.records[i].allocation_size >= cascade.records[i].size);
	for (unsigned i = 0; i < cascade.frames; ++i)
		assert (cascade.metadata[i].allocation_size >= cascade.metadata[i].size &&
			cascade.brick_table[i].allocation_size >= cascade.brick_table[i].size &&
			cascade.active_bricks[i].allocation_size >= cascade.active_bricks[i].size);
	free ((void *)trace.words); free ((void *)merge.words);
	assert (RCVk_UpdateSparse (&cascade, 0, sparse));
	uint32_t active_base, active_count, records_per_brick, overflow, all_count;
	assert (RC_SparseLevelWork (sparse, 4, &active_base, &active_count, &records_per_brick, &overflow) && active_count);
	const rc_sparse_brick_t *active = RC_SparseActive (sparse, &all_count);
	assert (active && active_base < all_count && records_per_brick >= 64);
	uint32_t record = active[active_base].record_base;
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	/* Recording is not submission: a discarded command buffer must not consume
	 * either the initial clear or a newly reused brick's pending clears. */
	assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 4, 0, 64));
	assert (cascade.needs_clear && !RCVk_UpdateSparse (&cascade, 0, sparse));
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	RCVk_DiscardRecorded (&cascade, command);
	assert (cascade.needs_clear);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	assert (RadianceInputs_RecordUpload (&inputs, 0, command));
	assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 4, 0, 64));
	VkMemoryBarrier compute_to_copy = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &compute_to_copy, 0, NULL, 0, NULL);
	VkBufferCopy copies[4] = {{(VkDeviceSize)record * 16, 0, 16}, {(VkDeviceSize)record * 16, 16, 16},
		{(VkDeviceSize)record * 4, 32, 4}, {(VkDeviceSize)record * 4, 36, 4}};
	vkCmdCopyBuffer (command, cascade.records[0].buffer, readback.buffer, 1, &copies[0]);
	vkCmdCopyBuffer (command, cascade.records[1].buffer, readback.buffer, 1, &copies[1]);
	vkCmdCopyBuffer (command, cascade.records[4].buffer, readback.buffer, 1, &copies[2]);
	vkCmdCopyBuffer (command, cascade.records[5].buffer, readback.buffer, 1, &copies[3]);
	VkMemoryBarrier copy_to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &copy_to_host, 0, NULL, 0, NULL);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
	assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
	RadianceInputs_MarkSubmitted (&inputs, 0);
	RCVk_MarkSubmitted (&cascade, command);
	assert (!cascade.needs_clear);
	assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
	const float *observed = readback.mapped;
	assert (fabsf (observed[0] - 1) < 1e-5 && fabsf (observed[1] - 0.5f) < 1e-5 &&
		fabsf (observed[2] - 0.25f) < 1e-5 && observed[3] == 0);
	assert (memcmp (observed, observed + 4, 16) == 0);
	uint32_t valid[2]; memcpy (valid, (const uint8_t *)readback.mapped + 32, sizeof (valid));
	assert (valid[0] == 1 && valid[1] == 1);

	/* A global sky change clears both raw and merged history before tracing
	 * the next budgeted interval. Untouched intervals must not retain old
	 * skylight, and a discarded recording must not consume the clear. */
	cascade.needs_clear = true;
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 4, 0, 1));
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	RCVk_DiscardRecorded (&cascade, command);
	assert (cascade.needs_clear);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 4, 0, 1));
	VkMemoryBarrier clear_to_copy = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &clear_to_copy, 0, NULL, 0, NULL);
	VkBufferCopy cleared[2] = {{(VkDeviceSize)(record + 1) * 4, 0, 4}, {(VkDeviceSize)(record + 1) * 4, 4, 4}};
	vkCmdCopyBuffer (command, cascade.records[4].buffer, readback.buffer, 1, &cleared[0]);
	vkCmdCopyBuffer (command, cascade.records[5].buffer, readback.buffer, 1, &cleared[1]);
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &copy_to_host, 0, NULL, 0, NULL);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
	RCVk_MarkSubmitted (&cascade, command);
	assert (!cascade.needs_clear && vkQueueWaitIdle (queue) == VK_SUCCESS);
	memcpy (valid, readback.mapped, sizeof (valid));
	assert (valid[0] == 0 && valid[1] == 0);

	/* Compare the actual optimized GPU dispatch with a forced full merge.
	 * Unchanged raw records already have identical merged values. */
	unsigned char merge_reference[128];
	for (unsigned pass = 0; pass < 2; ++pass)
	{
		cascade.submitted_dirty_mask = pass ? 1u : 0u;
		assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
		assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
		vkCmdFillBuffer (command, cascade.records[0].buffer, 0, cascade.records[0].size, 0);
		vkCmdFillBuffer (command, cascade.records[1].buffer, 0, cascade.records[1].size, 0);
		vkCmdFillBuffer (command, cascade.records[2].buffer, 0, cascade.records[2].size, 0x461c4000u);
		vkCmdFillBuffer (command, cascade.records[3].buffer, 0, cascade.records[3].size, 0x461c4000u);
		vkCmdFillBuffer (command, cascade.records[4].buffer, 0, cascade.records[4].size, 1);
		vkCmdFillBuffer (command, cascade.records[5].buffer, 0, cascade.records[5].size, 1);
		VkMemoryBarrier fill_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
		vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &fill_barrier, 0, NULL, 0, NULL);
		assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 0, 0, 1));
		assert (cascade.recorded_merge_intervals == (pass ? cascade.work[0][0].records_per_brick + 1 : 1));
		vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 1, &compute_to_copy, 0, NULL, 0, NULL);
		VkBufferCopy result_copy = {0, 0, sizeof (merge_reference)};
		vkCmdCopyBuffer (command, cascade.records[1].buffer, readback.buffer, 1, &result_copy);
		vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			0, 1, &copy_to_host, 0, NULL, 0, NULL);
		assert (vkEndCommandBuffer (command) == VK_SUCCESS);
		assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
		RCVk_MarkSubmitted (&cascade, command);
		assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
		if (!pass) memcpy (merge_reference, readback.mapped, sizeof (merge_reference));
		else assert (memcmp (merge_reference, readback.mapped, sizeof (merge_reference)) == 0);
	}
	/* A coarse update must propagate through the entire next-finer level. */
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	vkCmdFillBuffer (command, cascade.records[0].buffer, 0, cascade.records[0].size, 0);
	vkCmdFillBuffer (command, cascade.records[1].buffer, 0, cascade.records[1].size, 0x3f800000u);
	vkCmdFillBuffer (command, cascade.records[4].buffer, 0, cascade.records[4].size, 1);
	VkMemoryBarrier fine_fill = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &fine_fill, 0, NULL, 0, NULL);
	assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 1, 0, 1));
	assert (cascade.recorded_merge_intervals == 1 + cascade.work[0][0].active_count * cascade.work[0][0].records_per_brick);
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &compute_to_copy, 0, NULL, 0, NULL);
	VkBufferCopy fine_result = {0, 0, 16};
	vkCmdCopyBuffer (command, cascade.records[1].buffer, readback.buffer, 1, &fine_result);
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &copy_to_host, 0, NULL, 0, NULL);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
	RCVk_MarkSubmitted (&cascade, command);
	assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
	assert (((const float *)readback.mapped)[0] == 0.0f);

	/* Recenter preserves retained bricks but invalidates interpolation topology.
	 * Discard must not consume dirty state; a level-zero submission must merge
	 * dirty ancestors first before publishing any finer result. */
	uint64_t old_revision = RC_SparseTopologyRevision (sparse);
	camera[0] = 128;
	assert (RC_SparseBeginFrame (sparse, camera) && RC_SparseMarkAABB (sparse, mins, maxs, 0) && RC_SparseResolve (sparse));
	assert (RC_SparseTopologyRevision (sparse) != old_revision && RCVk_UpdateSparse (&cascade, 0, sparse));
	uint32_t topology_merges = 0;
	for (uint32_t level = 0; level < RC_CASCADE_COUNT; ++level)
		topology_merges += cascade.work[0][level].active_count * cascade.work[0][level].records_per_brick;
	for (unsigned pass = 0; pass < 2; ++pass)
	{
		assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
		assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
		assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 0, 0, 1));
		assert (cascade.recorded_merge_intervals == topology_merges + 1);
		assert (vkEndCommandBuffer (command) == VK_SUCCESS);
		if (!pass) RCVk_DiscardRecorded (&cascade, command);
		else
		{
			assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
			RCVk_MarkSubmitted (&cascade, command);
			assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
			assert (cascade.submitted_dirty_mask == 0);
		}
	}
	/* Dirty coarse merges must not be exposed to finer levels after a
	 * topology change. Model a retained, clear level-three raw interval with
	 * a formerly valid merged result, while its level-four support has been
	 * invalidated. The old merged result must become invalid before a level
	 * zero update can use it. */
	uint32_t stale_base, stale_count, stale_per, stale_overflow;
	assert (RC_SparseLevelWork (sparse, 3, &stale_base, &stale_count, &stale_per, &stale_overflow) && stale_count);
	active = RC_SparseActive (sparse, &all_count);
	assert (active && stale_base < all_count);
	const uint32_t stale_record = active[stale_base].record_base;
	uint32_t trace_base, trace_count, trace_per, trace_overflow;
	assert (RC_SparseLevelWork (sparse, 0, &trace_base, &trace_count, &trace_per, &trace_overflow) && trace_count);
	assert (trace_base < all_count);
	const uint32_t trace_record = active[trace_base].record_base;
	VkPipeline original_trace = cascade.trace_software, topology_trace = VK_NULL_HANDLE;
	VkShaderModule topology_module = VK_NULL_HANDLE;
	if (argc >= 6)
	{
		rc_vk_shader_t shader = ReadShader (argv[5]);
		VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = shader.size_bytes, .pCode = shader.words};
		assert (vkCreateShaderModule (device, &mi, NULL, &topology_module) == VK_SUCCESS);
		free ((void *)shader.words);
		VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = topology_module, .pName = "main"},
			.layout = cascade.software_layout};
		assert (vkCreateComputePipelines (device, VK_NULL_HANDLE, 1, &pi, NULL, &topology_trace) == VK_SUCCESS);
		cascade.trace_software = topology_trace;
	}
	/* Simulate a frame with pending dirty ancestors and stale merged records. */
	cascade.submitted_dirty_mask = ((1u << RC_CASCADE_COUNT) - 2u);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	vkCmdFillBuffer (command, cascade.records[4].buffer, 0, cascade.records[4].size, 0);
	vkCmdFillBuffer (command, cascade.records[5].buffer, 0, cascade.records[5].size, 1);
	VkMemoryBarrier fill_order = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &fill_order, 0, NULL, 0, NULL);
	vkCmdFillBuffer (command, cascade.records[4].buffer, (VkDeviceSize)stale_record * sizeof (uint32_t), sizeof (uint32_t), 1);
	vkCmdFillBuffer (command, cascade.records[0].buffer, (VkDeviceSize)stale_record * 16, 16, 0x3f800000u);
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &fine_fill, 0, NULL, 0, NULL);
	assert (RCVk_Record (&cascade, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 0, 0, 1));
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &compute_to_copy, 0, NULL, 0, NULL);
	VkBufferCopy stale_copy = {(VkDeviceSize)stale_record * sizeof (uint32_t), 0, sizeof (uint32_t)};
	vkCmdCopyBuffer (command, cascade.records[5].buffer, readback.buffer, 1, &stale_copy);
	if (topology_trace)
	{
		VkBufferCopy trace_copy = {(VkDeviceSize)trace_record * 16, 4, sizeof (float)};
		vkCmdCopyBuffer (command, cascade.records[0].buffer, readback.buffer, 1, &trace_copy);
	}
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &copy_to_host, 0, NULL, 0, NULL);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
	RCVk_MarkSubmitted (&cascade, command);
	assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
	uint32_t stale_valid;
	memcpy (&stale_valid, readback.mapped, sizeof (stale_valid));
	assert (stale_valid == 0);
	if (topology_trace)
	{
		float observed_during_trace;
		memcpy (&observed_during_trace, (const uint8_t *)readback.mapped + 4, sizeof (observed_during_trace));
		assert (observed_during_trace == 0.0f);
		cascade.trace_software = original_trace;
		vkDestroyPipeline (device, topology_trace, NULL);
		vkDestroyShaderModule (device, topology_module, NULL);
	}
	assert (cascade.submitted_dirty_mask == 0);
	camera[0] = 0;
	assert (RC_SparseBeginFrame (sparse, camera) && RC_SparseMarkAABB (sparse, mins, maxs, 0) && RC_SparseResolve (sparse));
	assert (RCVk_UpdateSparse (&cascade, 0, sparse));

	/* A far hit distance is relative to its own probe. Merge must translate
	 * it conservatively to the near probe before visibility tests use it. */
	uint32_t near_base, near_count, near_per_brick, near_overflow;
	assert (RC_SparseLevelWork (sparse, 3, &near_base, &near_count, &near_per_brick, &near_overflow) && near_count);
	active = RC_SparseActive (sparse, &all_count);
	assert (near_base < all_count);
	const rc_sparse_brick_t near_brick = active[near_base];
	const rc_layout_t *sparse_layout = RC_SparseLayout (sparse);
	const rc_level_t *near_level = &sparse_layout->levels[3], *far_level = &sparse_layout->levels[4];
	float point[3], far_point[3];
	const uint32_t brick_coords[3] = {near_brick.x, near_brick.y, near_brick.z};
	float separation_sq = 0;
	for (int axis = 0; axis < 3; ++axis)
	{
		point[axis] = near_level->origin_spacing[axis] + (brick_coords[axis] * near_level->layout[3] + .5f) * near_level->origin_spacing[3];
		float coord = (point[axis] - far_level->origin_spacing[axis]) / far_level->origin_spacing[3] - .5f;
		/* The selected near probe lies outside the low corner of this 2^3 far grid;
		 * all contributing clamped far corners refer to its first probe. */
		assert (coord < 0 && coord > -1);
		far_point[axis] = far_level->origin_spacing[axis] + .5f * far_level->origin_spacing[3];
		separation_sq += (point[axis] - far_point[axis]) * (point[axis] - far_point[axis]);
	}
	const float expected_distance = fmaxf (near_level->interval[1], 530 - sqrtf (separation_sq));
	assert (expected_distance == near_level->interval[1] && expected_distance < 530);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
	/* raw/merged radiance=vec4(1), raw distance=10000, merged distance=530,
	 * validity=1. This isolates merge interpolation from scene tracing. */
	const uint32_t fills[6] = {0x3f800000u, 0x3f800000u, 0x461c4000u, 0x44048000u, 1u, 1u};
	for (int i = 0; i < 6; ++i)
		vkCmdFillBuffer (command, cascade.records[i].buffer, 0, cascade.records[i].size, fills[i]);
	VkMemoryBarrier transfer_to_compute = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &transfer_to_compute, 0, NULL, 0, NULL);
	VkDescriptorSet sets[3] = {RCVk_DescriptorSet (&cascade, 0), scene_set, RadianceInputs_Descriptor (&inputs, 0)};
	vkCmdBindPipeline (command, VK_PIPELINE_BIND_POINT_COMPUTE, cascade.merge_software);
	vkCmdBindDescriptorSets (command, VK_PIPELINE_BIND_POINT_COMPUTE, cascade.software_layout, 0, 3, sets, 0, NULL);
	const uint32_t push[4] = {3, 0, 1, near_base};
	vkCmdPushConstants (command, cascade.software_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push), push);
	vkCmdDispatch (command, 1, 1, 1);
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &compute_to_copy, 0, NULL, 0, NULL);
	VkBufferCopy distance_copy = {(VkDeviceSize)near_brick.record_base * 4, 0, 4};
	VkBufferCopy validity_copy = {(VkDeviceSize)near_brick.record_base * 4, 4, 4};
	vkCmdCopyBuffer (command, cascade.records[3].buffer, readback.buffer, 1, &distance_copy);
	vkCmdCopyBuffer (command, cascade.records[5].buffer, readback.buffer, 1, &validity_copy);
	vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &copy_to_host, 0, NULL, 0, NULL);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle (queue) == VK_SUCCESS);
	uint32_t merged_valid; memcpy (&merged_valid, (const uint8_t *)readback.mapped + 4, sizeof (merged_valid));
	assert (merged_valid == 1 && fabsf (((float *)readback.mapped)[0] - expected_distance) < .01f);
	if (argc >= 4)
	{
		/* The synthetic hit has no direct or emissive source. It is black
		 * with invalid probes and red when prior indirect radiance is valid. */
		rc_vk_shader_t bounce_shader = ReadShader (argv[3]);
		VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = bounce_shader.size_bytes, .pCode = bounce_shader.words};
		VkShaderModule module; assert (vkCreateShaderModule (device, &mi, NULL, &module) == VK_SUCCESS);
		free ((void *)bounce_shader.words);
		VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = module, .pName = "main"}, .layout = cascade.software_layout};
		VkPipeline bounce; assert (vkCreateComputePipelines (device, VK_NULL_HANDLE, 1, &pi, NULL, &bounce) == VK_SUCCESS);
		VkShaderModule partial_module = VK_NULL_HANDLE; VkPipeline partial = VK_NULL_HANDLE;
		if (argc >= 5)
		{
			rc_vk_shader_t partial_shader = ReadShader (argv[4]);
			mi.codeSize = partial_shader.size_bytes; mi.pCode = partial_shader.words;
			assert (vkCreateShaderModule (device, &mi, NULL, &partial_module) == VK_SUCCESS);
			free ((void *)partial_shader.words);
			pi.stage.module = partial_module;
			assert (vkCreateComputePipelines (device, VK_NULL_HANDLE, 1, &pi, NULL, &partial) == VK_SUCCESS);
		}
		uint32_t zero_base, zero_count, zero_per, zero_overflow;
		assert (RC_SparseLevelWork (sparse, 0, &zero_base, &zero_count, &zero_per, &zero_overflow) && zero_count && zero_base == 0);
		active = RC_SparseActive (sparse, &all_count);
		VkBufferCopy bounce_copy = {(VkDeviceSize)(active[0].record_base + 64u) * 16, 0, 6 * 16};
		VkBufferCopy visibility_copy = {(VkDeviceSize)(active[0].record_base + 80u) * 16, 6 * 16, 6 * 16};
		VkBufferCopy gaussian_copy = {(VkDeviceSize)(active[0].record_base + 86u) * 16, 12 * 16, 2 * 16};
		VkBufferCopy fade_copy = {(VkDeviceSize)(active[0].record_base + 88u) * 16, 14 * 16, 16};
		for (uint32_t pass = 0; pass < (argc >= 5 ? 10u : 2u); ++pass)
		{
			const float gain = pass == 4 ? .5f : pass == 5 ? 4.0f : pass == 6 ? 0.0f : pass == 8 ? 1000.0f : 1.0f;
			snapshot.parameters.counts[3] = 1u | (pass >= 4 ? 8u : 0u);
			if (pass == 9) snapshot.parameters.counts[3] |= 32u;
			snapshot.parameters.gi_camera_fade[3] = pass == 9 ? 100.0f : 0.0f;
			snapshot.parameters.bounce_parameters[0] = gain;
			assert (RadianceInputs_Update (&inputs, 0, &snapshot));
			assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
			assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
			vkCmdFillBuffer (command, cascade.records[1].buffer, 0, cascade.records[1].size, 0x3f800000u);
			vkCmdFillBuffer (command, cascade.records[3].buffer, 0, cascade.records[3].size, pass == 7 ? 0x40800000u : 0x461c4000u);
			vkCmdFillBuffer (command, cascade.records[5].buffer, 0, cascade.records[5].size, (pass == 1 || pass >= 4) ? 1 : 0);
			vkCmdFillBuffer (command, cascade.records[4].buffer, 0, cascade.records[4].size, 0);
			if (pass == 2 || pass == 3)
			{
				VkMemoryBarrier fill_to_update = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
					.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
				vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
					&fill_to_update, 0, NULL, 0, NULL);
				const float opaque[4] = {1, 1, 1, 0}; const uint32_t valid_raw = 1;
				const uint32_t known = pass == 2 ? 4 : 1;
				for (uint32_t i = 0; i < known; ++i)
				{
					const uint32_t index = active[0].record_base + i;
					vkCmdUpdateBuffer (command, cascade.records[0].buffer, (VkDeviceSize)index * 16, 16, opaque);
					vkCmdUpdateBuffer (command, cascade.records[4].buffer, (VkDeviceSize)index * 4, 4, &valid_raw);
				}
			}
			vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
				&transfer_to_compute, 0, NULL, 0, NULL);
			vkCmdBindPipeline (command, VK_PIPELINE_BIND_POINT_COMPUTE, pass == 2 || pass == 3 || pass == 7 || pass == 9 ? partial : bounce);
			vkCmdBindDescriptorSets (command, VK_PIPELINE_BIND_POINT_COMPUTE, cascade.software_layout, 0, 3, sets, 0, NULL);
			vkCmdDispatch (command, 1, 1, 1);
			vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
				&compute_to_copy, 0, NULL, 0, NULL);
			vkCmdCopyBuffer (command, cascade.records[0].buffer, readback.buffer, 1, &bounce_copy);
			if (pass == 7) vkCmdCopyBuffer (command, cascade.records[0].buffer, readback.buffer, 1, &visibility_copy);
			if (pass == 9) vkCmdCopyBuffer (command, cascade.records[0].buffer, readback.buffer, 1, &gaussian_copy);
			if (pass == 9) vkCmdCopyBuffer (command, cascade.records[0].buffer, readback.buffer, 1, &fade_copy);
			vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
				&copy_to_host, 0, NULL, 0, NULL);
			assert (vkEndCommandBuffer (command) == VK_SUCCESS);
			assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle (queue) == VK_SUCCESS);
			const float (*bounce_color)[4] = readback.mapped;
			if (pass == 9)
			{
				const float (*g)[4] = bounce_color + 12;
				for (int side = 0; side < 6; ++side)
					assert (fabsf (bounce_color[side][0] - .95f) < .003f); /* transport unaffected by receiver mode */
				assert (fabsf (g[0][0] - 1.0f) < .002f); /* isotropic uniform field */
				assert (fabsf (g[0][1] - g[0][2]) < .002f); /* cell boundary */
				assert (g[0][3] > .0001f && fabsf (g[1][0]) < .0001f); /* broader impulse */
				assert (g[1][1] > .001f && g[1][1] < .99f);
				assert (fabsf (g[1][1] - g[1][2]) < .002f); /* absent weights retained */
				assert (fabsf (g[1][1] - g[1][3]) < .002f); /* blocked weights retained */
				assert (fabsf (bounce_color[14][0] - 1.0f) < .001f && fabsf (bounce_color[14][1] - 1.0f) < .001f);
				assert (fabsf (bounce_color[14][2] - .5f) < .001f && fabsf (bounce_color[14][3]) < .001f);
				continue;
			}
			if (pass == 7)
			{
				const float (*visibility_color)[4] = bounce_color + 6;
				assert (visibility_color[0][0] > .99f);
				for (int i = 1; i < 6; ++i)
					assert (visibility_color[i][0] > 0 && visibility_color[i][0] < visibility_color[i - 1][0]);
				continue;
			}
			bool partial_positive = false;
			for (int side = 0; side < 6; ++side)
			{
				assert (fabsf (bounce_color[side][1]) < .001f && fabsf (bounce_color[side][2]) < .001f);
				assert (fabsf (bounce_color[side][3] - gain) < .001f);
				/* Valid, bright probe data must contribute exactly nothing when
				 * bounce is disabled, even on the zero-gain lookup fast path. */
				if (pass == 6)
					assert (bounce_color[side][0] == 0.0f && bounce_color[side][1] == 0.0f && bounce_color[side][2] == 0.0f);
				if (pass != 3)
				{
					float expected = pass == 0 ? 0 : fminf (gain, .95f);
					if (fabsf (bounce_color[side][0] - expected) >= .003f)
						fprintf (stderr, "bounce pass %u side %d: %f expected %f\n", pass, side, bounce_color[side][0], expected);
					assert (fabsf (bounce_color[side][0] - expected) < .003f);
				}
				else
				{
					assert (bounce_color[side][0] >= 0 && bounce_color[side][0] <= .951f);
					partial_positive |= bounce_color[side][0] > .01f;
				}
			}
			if (pass == 3) assert (partial_positive);
		}
		if (partial) vkDestroyPipeline (device, partial, NULL);
		if (partial_module) vkDestroyShaderModule (device, partial_module, NULL);
		vkDestroyPipeline (device, bounce, NULL); vkDestroyShaderModule (device, module, NULL);
	}
	if (getenv ("RC_MERGE_BENCH"))
	{
		const uint32_t caps[RC_CASCADE_COUNT] = {512, 64, 8, 1, 1};
		rc_sparse_t *large = RC_SparseCreate (32, 2, 32, 64, caps); assert (large);
		const float lo[3] = {-1e6f, -1e6f, -1e6f}, hi[3] = {1e6f, 1e6f, 1e6f};
		assert (RC_SparseBeginFrame (large, camera) && RC_SparseMarkAABB (large, lo, hi, 0) && RC_SparseResolve (large));
		rc_vk_shader_t t = ReadShader (argv[1]), m = ReadShader (argv[2]);
		rc_vulkan_t bench;
		assert (RCVk_Create (&bench, device, &memory, &properties.limits, large, 1, scene_layout,
			VK_NULL_HANDLE, inputs.layout, t, (rc_vk_shader_t){0}, m, VK_NULL_HANDLE));
		free ((void *)t.words); free ((void *)m.words);
		assert (RCVk_UpdateSparse (&bench, 0, large));
		assert (bench.work[0][0].active_count == 512);
		VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
		VkQueryPool queries; assert (vkCreateQueryPool (device, &qi, NULL, &queries) == VK_SUCCESS);
		double sums[3] = {0, 0, 0};
		for (unsigned run = 0; run < 27; ++run)
		{
			const unsigned mode = run % 3u;
			/* Compare the stable partial path, an invalid fine level, and
			 * the hierarchy-wide invalidation used on topology changes. */
			if (run) bench.submitted_dirty_mask = mode == 0 ? 0 : mode == 1 ? 1 : (1u << RC_CASCADE_COUNT) - 1u;
			assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
			assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
			vkCmdResetQueryPool (command, queries, 0, 2);
			vkCmdWriteTimestamp (command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
			assert (RCVk_Record (&bench, command, 0, scene_set, RadianceInputs_Descriptor (&inputs, 0), false, 0, 0, 256));
			vkCmdWriteTimestamp (command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
			assert (vkEndCommandBuffer (command) == VK_SUCCESS);
			assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
			RCVk_MarkSubmitted (&bench, command);
			assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
			uint64_t times[2]; assert (vkGetQueryPoolResults (device, queries, 0, 2, sizeof (times), times,
				sizeof (uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
			if (run >= 3) sums[mode] += (double)(times[1] - times[0]) * properties.limits.timestampPeriod / 1e6;
		}
		printf ("Sparse merge benchmark, invalid/empty probes: bounded %.3f ms, fine %.3f ms, hierarchy %.3f ms (8 warm sets; %s)\n",
			sums[0] / 8, sums[1] / 8, sums[2] / 8, properties.deviceName);
		vkDestroyQueryPool (device, queries, NULL);
		RCVk_Destroy (&bench); RC_SparseDestroy (large);
	}
	RCVk_Destroy (&cascade); RC_SparseDestroy (sparse);
	vkUnmapMemory (device, empty_memory); vkDestroyBuffer (device, empty, NULL); vkFreeMemory (device, empty_memory, NULL);
	vkDestroyDescriptorPool (device, scene_pool, NULL); vkDestroyDescriptorSetLayout (device, scene_layout, NULL);
	assert (vkDeviceWaitIdle (device) == VK_SUCCESS);
	RadianceInputs_Destroy (&inputs);
	vkUnmapMemory (device, readback.memory); vkDestroyBuffer (device, readback.buffer, NULL); vkFreeMemory (device, readback.memory, NULL);
	vkDestroyCommandPool (device, pool, NULL); vkDestroyDevice (device, NULL); vkDestroyInstance (instance, NULL);
	puts ("PASS: Vulkan frame-slot descriptors, atlas uploads, refresh, resize and invalid input");
	return 0;
}
