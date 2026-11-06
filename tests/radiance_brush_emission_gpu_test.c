/* End-to-end BSP face extraction, cached emissive proxy and Vulkan receiver. */
#include "quakedef.h"
#include "radiance_scene_bsp.h"
#include "radiance_emissive.h"
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
static const texture_t *emitting_texture;
static uint32_t material_id (const texture_t *texture, void *user)
{ (void)user; return texture == emitting_texture ? 0 : 1; }
static void quad (glpoly_t *poly, const float points[4][3])
{
    memset(poly, 0, sizeof(*poly)); poly->numverts = 4;
    for(int i=0; i<4; ++i) {
        for(int c=0; c<3; ++c) poly->verts[i][c] = points[i][c];
        poly->verts[i][3] = (i==2 || i==3) ? 1.0f : 0.0f;
        poly->verts[i][4] = (i==1 || i==2) ? 1.0f : 0.0f;
    }
}
int main (int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[2], "--gated")) {
        const char *run = getenv("QUIVER_TEST_VULKAN");
        if (!run || strcmp(run, "1")) { puts("Vulkan GPU test disabled: set QUIVER_TEST_VULKAN=1"); return 77; }
    } else assert(argc == 2);
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
	free (families);
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
	VkBufferCreateInfo binfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1024,
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

    rc_scene_t scene; RCScene_Init(&scene);
    texture_t textures[2] = {0}; emitting_texture = &textures[0];
    textures[0].fullbright = (gltexture_t *)&textures[0];
    mtexinfo_t texinfo[2] = {0}; texinfo[0].texture = &textures[0]; texinfo[1].texture = &textures[1];
    mplane_t planes[2] = {0}; planes[0].normal[2] = planes[1].normal[2] = 1;
    glpoly_t polys[2];
    /* Emitter deliberately winds -Z: brush extraction must orient to +Z.
     * An offset blocker shades one front receiver without covering the other. */
    const float emitter[4][3]={{-5,-1,0},{-5,1,0},{-3,1,0},{-3,-1,0}};
    const float blocker[4][3]={{-3.75f,-2,2},{-2.5f,-2,2},{-2.5f,2,2},{-3.75f,2,2}};
    quad(&polys[0], emitter); quad(&polys[1], blocker);
    msurface_t surfaces[2] = {0};
    for(int i=0; i<2; ++i) { surfaces[i].texinfo=&texinfo[i]; surfaces[i].plane=&planes[i]; surfaces[i].polys=&polys[i]; }
    qmodel_t model = {0}; model.type=mod_brush; model.surfaces=surfaces; model.numsurfaces=model.nummodelsurfaces=2;
    assert(RCScene_AppendBrush(&scene, &model, NULL, 0, false, material_id, NULL));
    assert(scene.triangle_count == 4 && (scene.triangles[0].metadata[1] & RC_SCENE_ORIENTED));
    radiance_material_t materials[2] = {0};
    for(int i=0; i<2; ++i) for(int c=0; c<4; ++c) materials[i].albedo_alpha[c]=1;
    for(int c=0; c<4; ++c) materials[0].emission[c]=1;
    materials[0].layers_flags[2]=RADIANCE_MATERIAL_EMISSION_TEXTURE;
    materials[0].emission_rect[2]=materials[0].emission_rect[3]=1;
    uint8_t pixels[4]={255,255,255,255};
    radiance_material_atlas_t atlas={.materials=materials,.rgba=pixels,.material_count=2,.bytes=4,.width=1,.height=1,.layers=1};
    radiance_emissive_cache_t *cache=Radiance_EmissiveCacheCreate(); assert(cache);
    radiance_light_t light={0}; radiance_emissive_stats_t stats; bool changed;
    const float camera[3]={-4,0,4};
    assert(Radiance_BuildEmissiveLightsCached(cache,&scene,4,&atlas,1,true,true,camera,&light,1,&stats,&changed));
    assert(stats.emitted_lights==1 && changed && (light.metadata[2] & RADIANCE_LIGHT_SOURCE_ONE_SIDED));
    assert(light.direction_cone[2] > .99f);
    assert(Radiance_BuildEmissiveLightsCached(cache,&scene,4,&atlas,1,false,false,camera,&light,1,&stats,&changed) && !changed);
    assert(RCScene_Build(&scene));
    rc_scene_vk_t gpu_scene;
#ifdef RC_TEST_HARDWARE
    const bool hardware = true;
    rc_scene_rt_t rt; assert(RCSceneRT_Create(&rt,physical,device,2));
#else
    const bool hardware = false;
#endif
    assert(RCSceneVk_Create(&gpu_scene,device,&memory,&properties.limits,2,hardware));
    assert(RCSceneVk_UpdateSplit(&gpu_scene,0,&scene));
    radiance_gpu_inputs_t inputs;
    assert(RadianceInputs_Create(&inputs,physical,device,&memory,&properties.limits,2));
    const uint32_t offsets[]={0,1}, indices[]={0};
    radiance_input_snapshot_t snapshot={.lights=&light,.light_count=1,.materials=materials,.material_count=2,
       .grid_offsets=offsets,.grid_offset_count=2,.grid_indices=indices,.grid_index_count=1,
       .atlas_rgba=pixels,.atlas_width=1,.atlas_height=1,.atlas_layers=1};
    snapshot.parameters.counts[0]=1; snapshot.parameters.counts[1]=2; snapshot.parameters.counts[2]=1;
    snapshot.parameters.bounce_parameters[1]=1; /* deterministic center visibility ray */
    const char *emissive_gain_string = getenv("QUIVER_TEST_EMISSIVE_GAIN");
    float emissive_gain = emissive_gain_string ? strtof(emissive_gain_string, NULL) : 1.0f;
    assert(isfinite(emissive_gain) && emissive_gain >= 0);
    if (emissive_gain_string) {
        snapshot.parameters.counts[3] |= 4u;
        snapshot.parameters.grid_origin[3] = 1.0f;
        snapshot.parameters.grid_inv_cell_size[3] = emissive_gain;
    }
    assert(RadianceInputs_Update(&inputs,0,&snapshot));
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
	VkDescriptorBufferInfo output_buffer = {readback.buffer, 0, 1024};
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

    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
    assert(RadianceInputs_RecordUpload(&inputs,0,command));
#ifdef RC_TEST_HARDWARE
    assert(RCSceneRT_BuildSplit(&rt,0,&scene,scene.triangle_count,1,command));
    assert(RCSceneVk_SetAcceleration(&gpu_scene,0,RCSceneRT_Acceleration(&rt,0)));
#endif
    VkDescriptorSet sets[]={output_set,RCSceneVk_Descriptor(&gpu_scene,0,hardware),RadianceInputs_Descriptor(&inputs,0)};
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,3,sets,0,NULL);
    vkCmdDispatch(command,1,1,1);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    assert(vkEndCommandBuffer(command)==VK_SUCCESS);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    assert(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);
#ifdef RC_TEST_HARDWARE
    RCSceneRT_Submitted(&rt,0);
#endif
    RadianceInputs_MarkSubmitted(&inputs,0);
    assert(vkQueueWaitIdle(queue)==VK_SUCCESS);
    const float (*observed)[4]=readback.mapped;
    printf("receiver clear=%g blocked=%g back=%g two-sided-back=%g\n",observed[0][0],observed[0][1],observed[0][2],observed[0][3]);
    printf("hit front=%g back=%g front-normal=%g back-normal=%g\n",observed[1][0],observed[1][1],observed[1][2],observed[1][3]);
    if (emissive_gain > 0) assert(observed[0][0] > .01f && observed[0][3] > .01f);
    else assert(observed[0][0] < 1e-6f && observed[0][3] < 1e-6f);
    assert(observed[0][1] < 1e-6f && observed[0][2] < 1e-6f);
    assert(fabsf(observed[1][0] - emissive_gain * sqrtf(3.0f)) < 1e-5f &&
        observed[1][1] < 1e-6f && observed[1][2] > .99f && observed[1][3] > .99f);
    assert(observed[2][0] < 1e-6f && fabsf(observed[2][1] - .1f) < 1e-6f &&
        fabsf(observed[2][2] - 1.f) < 1e-6f && fabsf(observed[2][3]) < 1e-6f);
    vkDestroyPipeline(device,pipeline,NULL); vkDestroyShaderModule(device,module,NULL);
    vkDestroyPipelineLayout(device,pipeline_layout,NULL);
    vkDestroyDescriptorPool(device,output_pool,NULL); vkDestroyDescriptorSetLayout(device,output_layout,NULL);
    RadianceInputs_Destroy(&inputs); RCSceneVk_Destroy(&gpu_scene);
#ifdef RC_TEST_HARDWARE
    RCSceneRT_Destroy(&rt);
#endif
    Radiance_EmissiveCacheDestroy(cache); RCScene_Clear(&scene);
    vkUnmapMemory(device,readback.memory); vkDestroyBuffer(device,readback.buffer,NULL); vkFreeMemory(device,readback.memory,NULL);
    vkDestroyCommandPool(device,pool,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    puts(hardware ? "PASS: BSP cached emissive proxy through ray-query" : "PASS: BSP cached emissive proxy through software BVH");
    return 0;
}
