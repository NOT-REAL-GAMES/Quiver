/* Headless end-to-end software BVH, scene upload and material cutout test. */
#include "radiance_gpu_inputs.h"
#include "radiance_scene_vulkan.h"
#ifdef RC_TEST_HARDWARE
#include "radiance_scene_rt.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; } readback_t;
static void AppendPlane(rc_scene_t *scene, float z, unsigned material, unsigned flags)
{
    rc_scene_triangle_t tri = {0};
    tri.position[0][2] = tri.position[1][2] = tri.position[2][2] = z;
    tri.position[1][0] = tri.position[2][1] = 2;
    tri.uv01[2] = tri.uv2[1] = 1;
    tri.metadata[0] = material; tri.metadata[1] = flags;
    assert(RCScene_AppendTriangle(scene, &tri));
}
/* Stock Quake BSPs can contain a coincident sky face for each side of the
 * portal. Insert both orders: neither BVH traversal is required to pick a
 * particular normal when the intersections have equal distance. */
static void AppendSkyPair(rc_scene_t *scene, float x, bool reverse_order)
{
    rc_scene_triangle_t tri = {0};
    tri.position[0][0] = tri.position[1][0] = tri.position[2][0] = x;
    tri.position[1][0] += 2; tri.position[2][1] = 2;
    tri.metadata[1] = RC_SCENE_SKY | RC_SCENE_ORIENTED;
    if (reverse_order) {
        float tmp[4]; memcpy(tmp, tri.position[1], sizeof(tmp));
        memcpy(tri.position[1], tri.position[2], sizeof(tmp)); memcpy(tri.position[2], tmp, sizeof(tmp));
    }
    assert(RCScene_AppendTriangle(scene, &tri));
    float tmp[4]; memcpy(tmp, tri.position[1], sizeof(tmp));
    memcpy(tri.position[1], tri.position[2], sizeof(tmp)); memcpy(tri.position[2], tmp, sizeof(tmp));
    assert(RCScene_AppendTriangle(scene, &tri));
}
static bool Accept(const rc_scene_hit_t *hit, void *unused)
{
    (void) unused;
    if (hit->flags & RC_SCENE_SKY) return true;
    if (hit->flags & RC_SCENE_LIQUID) return false;
    /* Material 0 has a transparent left texel and opaque right texel;
     * material 1 has constant opaque albedo. */
    if (hit->flags & RC_SCENE_CUTOUT) return hit->material != 0 || hit->uv[0] >= .5f;
    return true;
}
/* The GPU's any-hit and closest-hit paths can share the same indexing bug.
 * Compare a grid of mixed-material rays to the independent CPU BVH instead. */
static void VerifyMaterialRays(const rc_scene_t *scene, const float (*observed)[4])
{
	const float direction[3] = {0, 0, -1};
	for (unsigned i = 0; i < 64; ++i)
	{
		float x=.2f+(float)((i*17u)%120u)*.01f;
		float y=.2f+(float)((i*29u)%60u)*.01f;
		if (fabsf (x-1.0f)<.02f) x+=.037f;
		if (fabsf (x+y-2.0f)<.015f) y-=.04f;
		float origin[3] = {x,y,10}, end = 1.25f+(float)(i%10u)*1.25f;
		rc_scene_hit_t expected = {0};
		bool found=RCScene_TraceReference (scene, origin, direction, 0, end, Accept, NULL, &expected);
		const float *sample=observed[32+i];
		if (found)
			assert (fabsf (sample[0]-expected.distance)<1e-4f && sample[1] == (float)expected.triangle &&
				sample[2] == (float)expected.flags && sample[3] == 1.0f);
		else assert (sample[0] == -1 && sample[3] == 0);
	}
}
static void VerifyTraceSlot(const rc_scene_t *scene, const float observed[4], const float origin[3], unsigned phase, unsigned slot)
{
	const float direction[3] = {0,0,-1};
	rc_scene_hit_t expected = {0};
	bool found=RCScene_TraceReference (scene, origin, direction, 0, 100, Accept, NULL, &expected);
	if (found)
	{
		if (!(fabsf (observed[0]-expected.distance)<1e-4f && observed[1] == (float)expected.material &&
			observed[2] == (float)expected.flags && observed[3] == (float)expected.triangle))
			fprintf (stderr, "phase %u slot %u: GPU %g %g %g %g, CPU %g %u %u %u\n", phase, slot,
				observed[0],observed[1],observed[2],observed[3], expected.distance,expected.material,expected.flags,expected.triangle);
		assert (fabsf (observed[0]-expected.distance)<1e-4f && observed[1] == (float)expected.material &&
			observed[2] == (float)expected.flags && observed[3] == (float)expected.triangle);
	}
	else assert (observed[0] == -1);
}
int main (int argc, char **argv)
{
	if (argc == 3 && !strcmp (argv[2], "--gated"))
	{
		const char *enabled = getenv ("QUIVER_TEST_VULKAN");
		if (!enabled || strcmp (enabled, "1")) { puts ("Vulkan test disabled: skipped"); return 77; }
		--argc;
	}
	assert (argc == 2 || (argc == 3 && !strcmp (argv[2], "--bench")));
	const bool benchmark = argc == 3;
	#ifdef RC_TEST_HARDWARE
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
#else
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
#endif
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
	assert (family < family_count);
	const uint32_t timestamp_bits = families[family].timestampValidBits;
	free (families);
	if (benchmark) assert (timestamp_bits > 0);
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
	VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
	#ifdef RC_TEST_HARDWARE
	VkPhysicalDeviceRayQueryFeaturesKHR ray_query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
	VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &ray_query};
	VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .pNext = &acceleration};
	VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &bda};
	vkGetPhysicalDeviceFeatures2 (physical, &features);
	if (!bda.bufferDeviceAddress || !acceleration.accelerationStructure || !ray_query.rayQuery) {
		vkDestroyInstance (instance, NULL); puts ("ray query unsupported: skipped"); return 77;
	}
	/* Enable only the features this test uses. */
	bda.bufferDeviceAddress = VK_TRUE; bda.bufferDeviceAddressCaptureReplay = VK_FALSE; bda.bufferDeviceAddressMultiDevice = VK_FALSE;
	acceleration.accelerationStructure = VK_TRUE; acceleration.accelerationStructureCaptureReplay = VK_FALSE;
	acceleration.accelerationStructureIndirectBuild = VK_FALSE; acceleration.accelerationStructureHostCommands = VK_FALSE;
	acceleration.descriptorBindingAccelerationStructureUpdateAfterBind = VK_FALSE; ray_query.rayQuery = VK_TRUE;
	const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME};
	device_info.pNext = &bda; device_info.enabledExtensionCount = 3; device_info.ppEnabledExtensionNames = extensions;
#endif
	VkDevice device = VK_NULL_HANDLE; assert (vkCreateDevice (physical, &device_info, NULL, &device) == VK_SUCCESS);
	VkQueue queue; vkGetDeviceQueue (device, family, 0, &queue);
	VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
	VkCommandPool pool = VK_NULL_HANDLE; assert (vkCreateCommandPool (device, &pool_info, NULL, &pool) == VK_SUCCESS);
	VkCommandBufferAllocateInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer command; assert (vkAllocateCommandBuffers (device, &cmd_info, &command) == VK_SUCCESS);
	readback_t readback = {0};
	VkBufferCreateInfo binfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2048,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
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
	rc_scene_t scene; RCScene_Init (&scene);
	AppendPlane (&scene, 8, 0, RC_SCENE_CUTOUT);
	AppendPlane (&scene, 4, 1, 0);
	AppendPlane (&scene, 9, 1, RC_SCENE_LIQUID);
	AppendPlane (&scene, 3, 0, RC_SCENE_SKY | RC_SCENE_CUTOUT | RC_SCENE_ORIENTED);
	/* Force internal BVH nodes and exercise escape links on real traversal. */
	for (int i = 0; i < 5; ++i) AppendPlane (&scene, -5.0f - 5.0f * i, 1, 0);
	AppendSkyPair (&scene, 10, false);
	AppendSkyPair (&scene, 20, true);
	assert (RCScene_Build (&scene) && scene.node_count > 1);
	rc_scene_vk_t gpu_scene;
	#ifdef RC_TEST_HARDWARE
	const bool hardware = true;
	rc_scene_rt_t rt; assert (RCSceneRT_Create (&rt, physical, device, 2));
	rc_scene_t empty; RCScene_Init (&empty);
	assert (!RCSceneRT_Build (&rt, 0, &empty, command) && !RCSceneRT_Acceleration (&rt, 0));
	RCScene_Clear (&empty);
#else
	const bool hardware = false;
#endif
	assert (RCSceneVk_Create (&gpu_scene, device, &memory, &properties.limits, 2, hardware));
	assert (RCSceneVk_UpdateSplit (&gpu_scene, 0, &scene));
	for (unsigned i = 0; i < 4; ++i)
		assert (gpu_scene.buffers[0][i].allocation_size >= gpu_scene.buffers[0][i].capacity);
	assert (gpu_scene.last_upload_bytes[0] > 0);
	assert (RCSceneVk_UpdateSplit (&gpu_scene, 0, &scene));
	assert (gpu_scene.last_upload_bytes[0] == 0);
	const uint32_t *remap = gpu_scene.buffers[0][1].mapped;
	for (uint32_t i = 0; i < scene.index_count; ++i)
		assert (remap[i] == scene.indices[i] && remap[scene.index_count+i] == i);
	assert (RCSceneVk_Update (&gpu_scene, 0, &scene));
	assert (RCSceneVk_UpdateSplit (&gpu_scene, 0, &scene));
	remap = gpu_scene.buffers[0][1].mapped;
	for (uint32_t i = 0; i < scene.index_count; ++i) assert (remap[scene.index_count+i] == i);
	assert (RCSceneVk_Descriptor (&gpu_scene, 0, false));
	radiance_gpu_inputs_t inputs;
	assert (RadianceInputs_Create (&inputs, physical, device, &memory, &properties.limits, 2));
	radiance_light_t light = {0};
	light.position_radius[0] = light.position_radius[1] = 100; light.position_radius[2] = 200; light.position_radius[3] = 350;
	light.color_intensity[0] = light.color_intensity[1] = light.color_intensity[2] = 1;
	light.color_intensity[3] = (float)(350.0 * 3.14159265358979323846 / 128.0);
	light.metadata[2] = RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC;
	radiance_material_t materials[2] = {0};
	for (int i = 0; i < 2; ++i) for (int j = 0; j < 4; ++j) materials[i].albedo_alpha[j] = 1;
	materials[1].emission[0]=2; materials[1].emission[1]=4; materials[1].emission[2]=6; materials[1].emission[3]=1;
	materials[0].layers_flags[2] = RADIANCE_MATERIAL_ALBEDO_TEXTURE;
	materials[0].albedo_rect[0] = 1; materials[0].albedo_rect[1] = 1;
	materials[0].albedo_rect[2] = 2; materials[0].albedo_rect[3] = 1;
	uint8_t pixels[4 * 3 * 4]; memset (pixels, 255, sizeof (pixels));
	pixels[(1 * 4 + 1) * 4 + 3] = 0;
	const uint32_t offsets[] = {0,1}, indices[] = {0};
	radiance_input_snapshot_t snapshot = {.lights = &light, .light_count = 1, .materials = materials, .material_count = 2,
		.grid_offsets = offsets, .grid_offset_count = 2, .grid_indices = indices, .grid_index_count = 1,
		.atlas_rgba = pixels, .atlas_width = 4, .atlas_height = 3, .atlas_layers = 1};
	snapshot.parameters.counts[0] = 1; snapshot.parameters.counts[1] = 2; snapshot.parameters.counts[2] = 1; snapshot.parameters.counts[3] = 1;
	snapshot.parameters.sky_radiance[0] = snapshot.parameters.ground_radiance[0] = 1.0f;
	snapshot.parameters.sky_radiance[3] = snapshot.parameters.ground_radiance[3] = 1.0f;
	for (int i = 0; i < 3; ++i) { snapshot.parameters.grid_dimensions[i] = 1; snapshot.parameters.grid_inv_cell_size[i] = 1; }
	snapshot.parameters.grid_dimensions[3] = 1;
	assert (RadianceInputs_Update (&inputs, 0, &snapshot));
	VkDescriptorSetLayoutBinding output_binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
	VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &output_binding};
	VkDescriptorSetLayout output_layout; assert (vkCreateDescriptorSetLayout (device, &layout_info, NULL, &output_layout) == VK_SUCCESS);
	VkDescriptorPoolSize output_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
	VkDescriptorPoolCreateInfo dp_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &output_size};
	VkDescriptorPool output_pool; assert (vkCreateDescriptorPool (device, &dp_info, NULL, &output_pool) == VK_SUCCESS);
	VkDescriptorSetAllocateInfo ds_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = output_pool, .descriptorSetCount = 1, .pSetLayouts = &output_layout};
	VkDescriptorSet output_set; assert (vkAllocateDescriptorSets (device, &ds_info, &output_set) == VK_SUCCESS);
	VkDescriptorBufferInfo output_buffer = {readback.buffer, 0, 2048};
	VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = output_set, .dstBinding = 0,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output_buffer};
	vkUpdateDescriptorSets (device, 1, &write, 0, NULL);
	VkDescriptorSetLayout layouts[] = {output_layout, hardware ? gpu_scene.hardware_layout : gpu_scene.software_layout, inputs.layout};
	VkPipelineLayoutCreateInfo pl_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 3, .pSetLayouts = layouts};
	VkPipelineLayout pipeline_layout; assert (vkCreatePipelineLayout (device, &pl_info, NULL, &pipeline_layout) == VK_SUCCESS);
	FILE *file = fopen (argv[1], "rb"); assert (file); assert (fseek (file, 0, SEEK_END) == 0);
	long shader_size = ftell (file); assert (shader_size > 0 && shader_size % 4 == 0); rewind (file);
	uint32_t *shader_bytes = malloc ((size_t)shader_size); assert (shader_bytes);
	assert (fread (shader_bytes, 1, (size_t)shader_size, file) == (size_t)shader_size); fclose (file);
	VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = (size_t)shader_size, .pCode = shader_bytes};
	VkShaderModule module; assert (vkCreateShaderModule (device, &module_info, NULL, &module) == VK_SUCCESS); free (shader_bytes);
	VkComputePipelineCreateInfo cp_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module, .pName = "main"}, .layout = pipeline_layout};
	VkPipeline pipeline; assert (vkCreateComputePipelines (device, VK_NULL_HANDLE, 1, &cp_info, NULL, &pipeline) == VK_SUCCESS);
	VkQueryPool query_pool = VK_NULL_HANDLE;
	if (benchmark)
	{
		VkQueryPoolCreateInfo query_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
		assert (vkCreateQueryPool (device, &query_info, NULL, &query_pool) == VK_SUCCESS);
	}
	const float origins[5][3] = {{.5f,.5f,10},{1.5f,.25f,10},{3,3,10},{1.5f,.25f,10},{.5f,.5f,3.5f}};
	const float tmins[5] = {0,0,0,2.5f,0}, direction[3] = {0,0,-1};
#ifdef RC_TEST_HARDWARE
	/* Abandon a recorded build, reset it, and retry before exposing its TLAS. */
	VkCommandBufferBeginInfo abandoned = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 4, 1, command));
	assert (rt.frame[0].static_vertices.allocation_size && rt.frame[0].static_memory.allocation_size &&
		rt.frame[0].vertices.allocation_size && rt.frame[0].blas_memory.allocation_size &&
		rt.frame[0].tlas_memory.allocation_size && rt.frame[0].scratch.allocation_size && rt.frame[0].instance.allocation_size);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	RCSceneRT_Discard (&rt, 0);
#endif
	radiance_light_t mixed_lights[3] = {0};
	const uint32_t mixed_offsets[] = {0, 3}, mixed_indices[] = {0, 1, 2};
	for (unsigned i = 0; i < 3; ++i)
	{
		mixed_lights[i].position_radius[0] = mixed_lights[i].position_radius[1] = 100;
		mixed_lights[i].position_radius[2] = 200; mixed_lights[i].position_radius[3] = 400;
		mixed_lights[i].color_intensity[0] = mixed_lights[i].color_intensity[1] = mixed_lights[i].color_intensity[2] = 1;
		mixed_lights[i].metadata[2] = RADIANCE_LIGHT_SOURCE_MAP;
	}
	mixed_lights[0].color_intensity[3] = 1;
	mixed_lights[1].color_intensity[3] = -.5f;
	mixed_lights[2].color_intensity[3] = .6f; mixed_lights[2].metadata[3] = 4;
	for (unsigned pass = 0; pass < (benchmark ? 3u : 6u); ++pass)
	{
		if (!benchmark)
		{
			light.color_intensity[3] = pass == 1 ? 0.0f : (float)(350.0 * 3.14159265358979323846 / 128.0);
			/* Test the two gain controls independently: disabling point lights
			 * must not also suppress emissive surface hits, and vice versa. */
			snapshot.parameters.counts[3] = (pass == 2 || pass == 3) ? 5u : 1u;
			snapshot.parameters.grid_origin[3] = pass == 3 ? 1.0f : 0.0f;
			snapshot.parameters.grid_inv_cell_size[3] = pass == 3 ? 0.0f : 1.0f;
			snapshot.parameters.bounce_parameters[1] = pass == 3 ? 1.0f : 0.0f;
			if (pass >= 4)
			{
				if (pass == 5) { radiance_light_t tmp = mixed_lights[0]; mixed_lights[0] = mixed_lights[2]; mixed_lights[2] = tmp; }
				snapshot.lights = mixed_lights; snapshot.light_count = 3; snapshot.parameters.counts[0] = 3;
				snapshot.grid_offsets = mixed_offsets; snapshot.grid_indices = mixed_indices;
				snapshot.grid_index_count = snapshot.parameters.grid_dimensions[3] = 3;
			}
			assert (RadianceInputs_Update (&inputs, 0, &snapshot));
		}
		if (!benchmark && pass == 1) {
			for (int i = 0; i < 3; ++i) scene.triangles[0].position[i][2] = 7;
			assert (RCScene_Refit (&scene));
			assert (RCSceneVk_UpdateSplit (&gpu_scene, 1, &scene));
		} else if (!benchmark && pass == 2) {
			/* Frame 0 must retain its original scene after frame 1 changes. */
			for (int i = 0; i < 3; ++i) scene.triangles[0].position[i][2] = 8;
			assert (RCScene_Refit (&scene));
		}
		assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
		VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
		assert (RadianceInputs_RecordUpload (&inputs, 0, command));
#ifdef RC_TEST_HARDWARE
		/* The third dispatch reuses the original frame-0 structure. */
		if (pass < 2 && (!benchmark || pass == 0)) {
			assert (RCSceneRT_BuildSplit (&rt, pass, &scene, 4, 1, command));
			assert (rt.frame[pass].static_builds == 1 && rt.frame[pass].dynamic_builds == 1);
			assert (RCSceneVk_SetAcceleration (&gpu_scene, pass, RCSceneRT_Acceleration (&rt, pass)));
		}
#endif
		VkDescriptorSet sets[] = {output_set, RCSceneVk_Descriptor (&gpu_scene, pass == 1 ? 1 : 0, hardware), RadianceInputs_Descriptor (&inputs, 0)};
		if (benchmark) sets[1] = RCSceneVk_Descriptor (&gpu_scene, 0, hardware);
		vkCmdBindPipeline (command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets (command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 3, sets, 0, NULL);
		if (benchmark)
		{
			vkCmdResetQueryPool (command, query_pool, 0, 2);
			vkCmdWriteTimestamp (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, query_pool, 0);
		}
		vkCmdDispatch (command, 1, 1, 1);
		if (benchmark) vkCmdWriteTimestamp (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, query_pool, 1);
		VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT};
		vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
		assert (vkEndCommandBuffer (command) == VK_SUCCESS);
		VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
		assert (vkQueueSubmit (queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
		#ifdef RC_TEST_HARDWARE
		if (pass < 2 && (!benchmark || pass == 0)) RCSceneRT_Submitted (&rt, pass);
		#endif
		RadianceInputs_MarkSubmitted (&inputs, 0);
		assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
		if (benchmark)
		{
			uint64_t timestamps[2];
			assert (vkGetQueryPoolResults (device, query_pool, 0, 2, sizeof (timestamps), timestamps, sizeof (uint64_t),
				VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
			uint64_t mask = timestamp_bits >= 64 ? UINT64_MAX : (UINT64_C (1) << timestamp_bits) - 1;
			const float (*values)[4] = readback.mapped;
			double checksum = 0;
			for (int i = 0; i < 64; ++i) checksum += values[i][0];
			printf ("shadow benchmark sample %.3f ms checksum %.0f\n", (double)((timestamps[1] - timestamps[0]) & mask) * properties.limits.timestampPeriod / 1000000.0, checksum);
			continue;
		}
		const float (*observed)[4] = readback.mapped;
		assert (observed[97][0] == 0 && fabsf (observed[97][1] - 1.0f) < 1e-5f);
		const float gain = pass == 2 ? 0.0f : 1.0f;
		assert (fabsf (observed[98][0] - .75f) < 1e-5f);
		assert (fabsf (observed[98][1] - .375f * gain) < 1e-5f &&
			fabsf (observed[98][2] - .375f * gain) < 1e-5f &&
			fabsf (observed[98][3] - .5625f * gain) < 1e-5f);
		assert (fabsf (observed[21][0] - .75f) < 1e-5f);
		assert (fabsf (observed[21][1] - (pass == 2 ? 0 : -.375f)) < 1e-5f);
		assert (fabsf (observed[21][2] - (pass == 2 ? 0 : .6f)) < 1e-5f);
		assert (fabsf (observed[23][0] - .75f) < 1e-5f && observed[23][1] == 0);
		assert (fabsf (observed[24][0] - .5625f * gain) < 1e-5f);
		assert (fabsf (observed[24][1] - .375f * gain) < 1e-5f && observed[24][2] == 0);
		assert (fabsf (observed[24][3] - .375f * gain) < 1e-5f);
		/* Both sides of floating-point grazing normal noise preserve the
		 * compiler response, while actual backfaces and Lambert lights stay dark. */
		const float near_front_cosine = .0005f / sqrtf (1.0f + .0005f * .0005f);
		assert (fabsf (observed[107][0] - .375f * (1.0f + near_front_cosine) * gain) < 1e-5f &&
			fabsf (observed[107][1] - .375f * gain) < 1e-5f &&
			observed[107][2] == 0 && observed[107][3] == 0);
		assert (fabsf (observed[108][0] - .5f * (1.0f + near_front_cosine)) < 1e-5f &&
			fabsf (observed[108][1] - .5f) < 1e-5f &&
			observed[108][2] == 0 && observed[108][3] == 0);
		for (unsigned channel = 0; channel < 4; ++channel) assert (observed[109][channel] == 0);
		const float soft = .75f * (cosf (20.0f * .01745329251994329577f) - cosf (30.0f * .01745329251994329577f)) /
			(cosf (15.0f * .01745329251994329577f) - cosf (30.0f * .01745329251994329577f));
		assert (fabsf (observed[25][0] - .75f * gain) < 1e-4f && observed[25][1] == 0);
		assert (fabsf (observed[25][2] - soft * gain) < 1e-4f);
		assert (observed[26][1] == 0);
		if (pass != 3) assert (observed[26][0] > 0 && observed[26][2] > 0);
		else assert (observed[26][0] == 0 && observed[26][2] == 0);
		assert ((pass == 3 ? observed[27][0] < 1e-5f : observed[27][0] > 0) && observed[27][1] < 1e-5f);
		assert (observed[100][0] == 1.0f && observed[100][1] == 1.0f && observed[100][2] == 1.0f);
		assert (observed[101][0] == 1.0f && observed[101][1] == 1.0f);
		for (unsigned i = 102; i < 106; ++i)
			assert (observed[i][0] == 1.0f && observed[i][1] == 1.0f && observed[i][2] == 1.0f && observed[i][3] == 0.0f);
		if (pass < 4)
		{
			/* Pass 1 moves the cutout roof from z=8 to z=7. The ray starts
			 * at z=9.95 after normal bias and ends at z=7.55, so it misses
			 * the moved roof and all 64 samples are visible. */
			const float mismatches = pass == 1 ? 24 : 0, visible = pass == 1 ? 64 : 40;
			if (observed[106][0] != 0 || observed[106][1] != 0 || observed[106][2] != mismatches || observed[106][3] != visible)
				fprintf (stderr, "sky visibility grid: %g %g %g %g (pass %u)\n", observed[106][0], observed[106][1], observed[106][2], observed[106][3], pass);
			assert (observed[106][0] == 0 && observed[106][1] == 0 && observed[106][2] == mismatches && observed[106][3] == visible);
		}
		if (pass >= 4)
		{
			const float invpi = .3183098861837907f;
			assert (fabsf (observed[22][0] - .6f * invpi) < 1e-5f);
			assert (fabsf (observed[22][1] - (.375f * invpi + .4f)) < 1e-5f);
			assert (fabsf (observed[22][2] - .6f * invpi) < 1e-5f);
			continue;
		}
		for (int i = 0; i < 5; ++i) {
			rc_scene_hit_t expected = {0};
			bool found = RCScene_TraceReference (&scene, origins[i], direction, tmins[i], 100, Accept, NULL, &expected);
			if (!found) assert (observed[i][0] == -1);
			else assert (fabsf (observed[i][0] - expected.distance) < 1e-4f && observed[i][1] == (float)expected.material &&
				observed[i][2] == (float)expected.flags && observed[i][3] == (float)expected.triangle);
		}
		VerifyMaterialRays (&scene, observed);
		assert (observed[0][0] == 6 && observed[0][1] == 1);
		assert (observed[1][0] == (pass == 1 ? 3 : 2) && observed[1][1] == 0);
		assert (observed[2][0] == -1 && observed[3][0] == (pass == 1 ? 3 : 6));
		assert (observed[4][0] == .5f && (unsigned)observed[4][2] & RC_SCENE_SKY);
		assert (observed[5][0] == 1 && observed[5][1] == 0);
		assert (observed[6][0] == 0 && observed[6][1] == 0 && observed[6][2] == 1 && observed[6][3] == 1);
		assert (observed[7][0] == 0 && observed[7][1] == 1 && observed[7][2] == 1 && observed[7][3] == 0);
		const float expected_partial = pass == 3 ? observed[20][0] : pass == 1 ? .75f : .5f;
		if (observed[11][0] != expected_partial || observed[11][1] != 1 || observed[11][2] != 0)
			fprintf (stderr, "area visibility: %.3f %.3f %.3f (pass %u)\n", observed[11][0], observed[11][1], observed[11][2], pass);
		assert (observed[11][0] == expected_partial && observed[11][1] == 1 && observed[11][2] == 0);
		if (pass != 1 && pass != 3) assert (observed[12][0] == .5f && observed[12][1] == 1);
		assert (observed[12][2] == 1 && observed[12][3] == 1);
		assert ((observed[20][0] == 0 || observed[20][0] == 1) && observed[20][1] == 1 && observed[20][2] == 0);
		if (pass == 0 || pass >= 2)
		{
			/* Closest edge visible while the center is blocked, and the
			 * opposite arrangement; reversing the tangent cannot change it. */
			if (observed[28][0] != 1 || observed[28][1] != 0 || observed[28][2] != 0 || observed[28][3] != 1)
				fprintf (stderr, "area edge visibility: %.3f %.3f %.3f %.3f (pass %u)\n", observed[28][0], observed[28][1], observed[28][2], observed[28][3], pass);
			assert (observed[28][0] == 1 && observed[28][1] == 0 && observed[28][2] == 0 && observed[28][3] == 1);
			assert (observed[96][0] == 1 && observed[96][1] == 0);
		}
		assert (observed[13][0] == (pass == 3 ? 0 : 2) && observed[13][1] == (pass == 3 ? 0 : 4) &&
			observed[13][2] == (pass == 3 ? 0 : 6) && observed[13][3] == 0);
		assert (fabsf (observed[14][0] - .96f) < 1e-5f &&
			fabsf (observed[14][1] - .96f) < 1e-5f &&
			fabsf (observed[14][2] - (1.0f - sqrtf (20.0f) / 100.0f)) < 1e-5f &&
			fabsf (observed[14][3] - .96f) < 1e-5f);
		assert (fabsf (observed[15][0] - 250.0f / 350.0f) < 1e-5f &&
			fabsf (observed[15][1] - 50.0f / 350.0f) < 1e-5f && observed[15][2] > 0 && observed[15][3] == 0);
		if (observed[16][0] != 0 || observed[16][1] != 0 || observed[16][2] != 1 || observed[16][3] != 1)
			fprintf(stderr,"occlusion: %.0f %.0f %.0f %.0f (pass %u)\n",observed[16][0],observed[16][1],observed[16][2],observed[16][3],pass);
		assert (observed[16][0] == 0 && observed[16][1] == 0 && observed[16][2] == 1 && observed[16][3] == 1);
		for (int channel = 0; channel < 3; ++channel)
			assert (fabsf (observed[17][channel] - ((pass == 0 || pass == 3) ? 250.0f / 128.0f : 0)) < 1e-4f);
		assert (observed[18][0] == 0);
		const float facing_expected = pass == 2 ? 0.0f : .5625f;
		if (fabsf (observed[19][0] - facing_expected) >= 1e-5f || observed[19][1] != 0 || observed[19][2] != 0)
			fprintf (stderr, "Lambert visibility: %g %g %g (pass %u)\n", observed[19][0], observed[19][1], observed[19][2], pass);
		assert (fabsf (observed[19][0] - facing_expected) < 1e-5f && observed[19][1] == 0 && observed[19][2] == 0);
		assert (fabsf(observed[8][0]-.75f)<1e-5f && fabsf(observed[8][1]-.5f)<1e-5f &&
			fabsf(observed[8][2]-.25f)<1e-5f && observed[8][3]==1);
		assert (fabsf(observed[9][0]-(1.0f/9.0f))<1e-5f && fabsf(observed[9][1]-.25f)<1e-5f && observed[9][2]==1);
		assert (fabsf(observed[9][3]-.75f)<1e-5f &&
			fabsf(observed[10][0]-.5f)<1e-5f && observed[10][1]==0 &&
			observed[10][2]==1 && fabsf(observed[10][3]-.001f)<1e-5f);
		assert (observed[31][0]==0 && observed[31][1]==0 && observed[31][2]==0);
	}
	#ifdef RC_TEST_HARDWARE
	if (!benchmark)
	{
	/* Move an existing dynamic triangle and verify an in-place update with an
	 * actual ray query, followed by a discarded update and a clean rebuild. */
	for (int i = 0; i < 3; ++i) scene.triangles[4].position[i][2] = 9.5f;
	assert (RCScene_Refit (&scene));
	assert (RCSceneVk_UpdateSplit (&gpu_scene, 0, &scene));
	VkAccelerationStructureKHR old_blas = rt.frame[0].blas, old_tlas = rt.frame[0].tlas;
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 4, 1, command));
	assert (rt.frame[0].static_builds == 0 && rt.frame[0].static_upload_bytes == 0);
	assert (rt.frame[0].dynamic_builds == 0 && rt.frame[0].tlas_builds == 0);
	assert (rt.frame[0].dynamic_updates == 1 && rt.frame[0].tlas_updates == 1);
	assert (rt.frame[0].blas == old_blas && rt.frame[0].tlas == old_tlas);
	assert (RCSceneVk_SetAcceleration (&gpu_scene, 0, RCSceneRT_Acceleration (&rt, 0)));
	VkDescriptorSet refit_sets[] = {output_set, RCSceneVk_Descriptor (&gpu_scene, 0, hardware), RadianceInputs_Descriptor (&inputs, 0)};
	vkCmdBindPipeline (command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets (command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 3, refit_sets, 0, NULL);
	vkCmdDispatch (command, 1, 1, 1);
	VkMemoryBarrier refit_barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier (command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&refit_barrier,0,NULL,0,NULL);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkQueueSubmit (queue, 1, &(VkSubmitInfo){.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount=1,.pCommandBuffers=&command}, VK_NULL_HANDLE) == VK_SUCCESS);
	RCSceneRT_Submitted (&rt, 0);
	assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
	const float (*refit_observed)[4] = readback.mapped;
	assert (fabsf (refit_observed[0][0] - .5f) < 1e-4f && refit_observed[0][3] == 4);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 4, 1, command));
	assert (rt.frame[0].dynamic_updates == 1);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	RCSceneRT_Discard (&rt, 0);
	assert (!rt.frame[0].geometry_ready);
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 4, 1, command));
	assert (rt.frame[0].static_builds == 0 && rt.frame[0].dynamic_builds == 1 && rt.frame[0].tlas_builds == 1);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	RCSceneRT_Discard (&rt, 0);
	/* A changed primitive count within the same world is a topology change,
	 * even when all old triangle data is otherwise unchanged. */
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 4, 1, command));
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkQueueSubmit (queue, 1, &(VkSubmitInfo){.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount=1,.pCommandBuffers=&command}, VK_NULL_HANDLE) == VK_SUCCESS);
	RCSceneRT_Submitted (&rt, 0);
	assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
	AppendPlane (&scene, -40, 1, 0);
	assert (RCScene_Build (&scene));
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 4, 1, command));
	assert (rt.frame[0].static_builds == 0 && rt.frame[0].dynamic_builds == 1 && rt.frame[0].dynamic_updates == 0);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	RCSceneRT_Discard (&rt, 0);
	/* Map replacement, static-only and dynamic-only snapshots must not reuse
	 * a BLAS from an earlier world. */
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, scene.triangle_count, 2, command));
	assert (rt.frame[0].static_builds == 1 && rt.frame[0].dynamic_builds == 0);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkQueueSubmit (queue, 1, &(VkSubmitInfo){.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount=1,.pCommandBuffers=&command}, VK_NULL_HANDLE) == VK_SUCCESS);
	RCSceneRT_Submitted (&rt, 0);
	assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	assert (vkBeginCommandBuffer (command, &abandoned) == VK_SUCCESS);
	assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 0, 3, command));
	assert (!rt.frame[0].static_blas && rt.frame[0].static_builds == 0 && rt.frame[0].dynamic_builds == 1);
	assert (vkEndCommandBuffer (command) == VK_SUCCESS);
	assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
	RCSceneRT_Discard (&rt, 0);
	}
	#endif
	if (!benchmark)
	{
		/* Change opacity classification without moving geometry or changing the
		 * world epoch. This simulates animated texture/skin changes and catches a
		 * stale opaque acceleration structure that would bypass alpha handling. */
		for (int v=0; v<3; ++v) scene.triangles[4].position[v][2] = -5;
		const float static_origin[3] = {.5f,.5f,10}, dynamic_origin[3] = {.5f,.5f,-4};
		const uint32_t original_static_flags[4] = {RC_SCENE_CUTOUT,0,RC_SCENE_LIQUID,RC_SCENE_SKY | RC_SCENE_CUTOUT | RC_SCENE_ORIENTED};
		for (unsigned phase=0; phase<10; ++phase)
		{
			for (unsigned i=0; i<4; ++i)
				scene.triangles[i].metadata[1] = phase == 7 ? RC_SCENE_CUTOUT : phase == 8 ? 0 : original_static_flags[i];
			if (phase < 7)
			scene.triangles[1].metadata[1] = phase == 1 ? RC_SCENE_LIQUID :
				phase == 5 ? RC_SCENE_EMISSIVE_PROXY : phase == 6 ? RC_SCENE_SKY | RC_SCENE_CUTOUT : 0;
			scene.triangles[4].metadata[0] = phase == 3 ? 0 : 1;
			scene.triangles[4].metadata[1] = phase == 3 ? RC_SCENE_CUTOUT : 0;
			assert (RCScene_Refit (&scene));
			assert (RCSceneVk_UpdateSplit (&gpu_scene, 0, &scene));
			assert (vkResetCommandPool (device, pool, 0) == VK_SUCCESS);
			VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
			assert (vkBeginCommandBuffer (command, &begin) == VK_SUCCESS);
#ifdef RC_TEST_HARDWARE
			assert (RCSceneRT_BuildSplit (&rt, 0, &scene, 4, 4, command));
			/* Emission and sky metadata change shading but not geometry. */
			if (phase == 5 || phase == 6) assert (rt.frame[0].static_builds == 0);
			assert (RCSceneVk_SetAcceleration (&gpu_scene, 0, RCSceneRT_Acceleration (&rt, 0)));
#endif
			VkDescriptorSet sets[] = {output_set, RCSceneVk_Descriptor (&gpu_scene, 0, hardware), RadianceInputs_Descriptor (&inputs, 0)};
			vkCmdBindPipeline (command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vkCmdBindDescriptorSets (command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 3, sets, 0, NULL);
			vkCmdDispatch (command, 1, 1, 1);
			VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
				.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
			vkCmdPipelineBarrier (command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				0, 1, &barrier, 0, NULL, 0, NULL);
			assert (vkEndCommandBuffer (command) == VK_SUCCESS);
			assert (vkQueueSubmit (queue, 1, &(VkSubmitInfo){.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount=1,.pCommandBuffers=&command}, VK_NULL_HANDLE) == VK_SUCCESS);
#ifdef RC_TEST_HARDWARE
			RCSceneRT_Submitted (&rt, 0);
#endif
			assert (vkQueueWaitIdle (queue) == VK_SUCCESS);
			const float (*observed)[4] = readback.mapped;
			VerifyTraceSlot (&scene, observed[0], static_origin, phase, 0);
			VerifyTraceSlot (&scene, observed[29], dynamic_origin, phase, 29);
			VerifyMaterialRays (&scene, observed);
			assert (observed[30][0] == 0 && observed[30][1] == (phase == 1 || phase == 6 ? 1 : 0));
			assert (observed[0][0] == (phase == 1 ? 7 : phase == 7 || phase == 8 ? 1 : 6));
			assert (observed[29][0] == (phase == 3 ? 6 : 1));
		}
	}
	#ifdef RC_TEST_HARDWARE
	if (!benchmark)
	{
		/* One shared object-space BLAS supplies 32,768 translation instances.
		 * Batch geometry participates in direct shadows, not closest-hit GI. */
		const uint32_t count=32768;
		if (rt.properties.maxInstanceCount >= count+2u)
		{
			float (*origins)[3]=calloc(count,sizeof(*origins)); assert(origins);
			for (uint32_t i=0;i<count;++i) { origins[i][0]=1000.0f+(float)i*4.0f; origins[i][1]=1000; origins[i][2]=5; }
			origins[0][0]=3; origins[0][1]=3;
			rc_scene_triangle_t triangle={0}; triangle.position[1][0]=1; triangle.position[2][1]=1;
			rc_scene_rt_batch_t batch={&triangle,1,(const float (*)[3])origins,count,1};
			VkAccelerationStructureKHR mesh=VK_NULL_HANDLE;
			for (unsigned phase=0;phase<4;++phase)
			{
				if (phase==1) origins[0][0]=30;
				if (phase==2) { origins[0][0]=3; triangle.position[1][0]=2; }
				assert (RCSceneVk_UpdateSplit (&gpu_scene, 0, &scene));
				assert (vkResetCommandPool (device,pool,0)==VK_SUCCESS);
				VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
				assert (vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
				if (phase==3) { triangle.metadata[1]=RC_SCENE_CUTOUT; assert(!RCSceneRT_BuildSplitBatch(&rt,0,&scene,4,6,&batch,command)); }
				assert (RCSceneRT_BuildSplitBatch(&rt,0,&scene,4,phase>=2 ? 6 : 5,phase==3 ? NULL : &batch,command));
				if (!phase) { mesh=rt.frame[0].batch_blas; assert(mesh && rt.frame[0].batch_count==count); }
				if (phase==1) assert(rt.frame[0].batch_blas==mesh && rt.frame[0].tlas_updates==1);
				if (phase==2) assert(rt.frame[0].batch_world_epoch==6 && rt.frame[0].batch_recorded);
				assert(RCSceneVk_SetAcceleration(&gpu_scene,0,RCSceneRT_Acceleration(&rt,0)));
				VkDescriptorSet sets[]={output_set,RCSceneVk_Descriptor(&gpu_scene,0,hardware),RadianceInputs_Descriptor(&inputs,0)};
				vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
				vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,3,sets,0,NULL);
				vkCmdDispatch(command,1,1,1);
				VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
				vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
				assert(vkEndCommandBuffer(command)==VK_SUCCESS);
				assert(vkQueueSubmit(queue,1,&(VkSubmitInfo){.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command},VK_NULL_HANDLE)==VK_SUCCESS);
				RCSceneRT_Submitted(&rt,0); assert(vkQueueWaitIdle(queue)==VK_SUCCESS);
				const float (*observed)[4]=readback.mapped;
				assert(observed[99][0] == (phase==0 || phase==2 ? 1 : 0) && observed[99][1]==0);
			}
			free(origins);
		}
	}
	#endif
	if (query_pool) vkDestroyQueryPool (device, query_pool, NULL);
	vkDestroyPipeline (device, pipeline, NULL); vkDestroyShaderModule (device, module, NULL);
	vkDestroyPipelineLayout (device, pipeline_layout, NULL);
	vkDestroyDescriptorPool (device, output_pool, NULL); vkDestroyDescriptorSetLayout (device, output_layout, NULL);
	RadianceInputs_Destroy (&inputs); RCSceneVk_Destroy (&gpu_scene);
#ifdef RC_TEST_HARDWARE
	RCSceneRT_Destroy (&rt);
#endif
	RCScene_Clear (&scene);
	vkUnmapMemory (device, readback.memory); vkDestroyBuffer (device, readback.buffer, NULL); vkFreeMemory (device, readback.memory, NULL);
	vkDestroyCommandPool (device, pool, NULL); vkDestroyDevice (device, NULL); vkDestroyInstance (instance, NULL);
	puts (benchmark ? "radiance synthetic shadow timing complete" :
		(hardware ? "radiance scene GPU ray-query trace passed" : "radiance scene GPU software trace passed")); return 0;
}
