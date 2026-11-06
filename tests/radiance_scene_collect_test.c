/* Real Vulkan frame/fence orchestration over synthetic Quake map data. */
#include "quakedef.h"
#include "radiance_scene_collect.h"
#include "radiance_scene_alias.h"
#include "radiance_coordinator.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
cvar_t gl_fullbrights = {.name = "gl_fullbrights", .string = "0", .flags = CVAR_NONE};
cvar_t gl_nocolors = {.name = "gl_nocolors", .string = "0", .flags = CVAR_NONE};
client_state_t cl; viddef_t vid; gltexture_t *playertextures[MAX_SCOREBOARD]; unsigned int d_8to24table[256];
static bool advance_animation;
texture_t *R_TextureAnimation(texture_t *base, int frame)
{
    if (frame && base->alternate_anims) base=base->alternate_anims;
    return advance_animation && base->anim_next ? base->anim_next : base;
}
qfilesize_t COM_OpenFile(const char *name, int *handle, unsigned int *path_id) { (void)name; (void)path_id; *handle=-1; return -1; }
qfileofs_t Sys_FilePos(int handle) { (void)handle; return -1; }
int Sys_FileSeek(int handle, qfileofs_t offset) { (void)handle; (void)offset; return -1; }
int Sys_FileRead(int handle, void *data, int size) { (void)handle; (void)data; (void)size; return 0; }
void Sys_FileClose(int handle) { (void)handle; }
byte *Image_LoadImage(const char *name, int *width, int *height, enum srcformat *format, unsigned int path_id)
{ (void)name; (void)width; (void)height; (void)format; (void)path_id; return NULL; }
void Mem_Free(const void *pointer) { free((void *)pointer); }
void IdentityMatrix(float m[16]) { memset(m,0,16*sizeof(*m)); m[0]=m[5]=m[10]=m[15]=1; }
void R_RotateForEntityScale(float m[16], vec3_t origin, vec3_t angles, float scale)
{ (void)angles; (void)scale; m[12]=origin[0]; m[13]=origin[1]; m[14]=origin[2]; }
bool RCScene_AppendAliasLerped(rc_scene_t *s, const entity_t *e, uint32_t i, rc_scene_alias_material_fn fn, void *u)
{ (void)s; (void)e; (void)i; (void)fn; (void)u; return false; }
bool RCScene_AliasSupported(const entity_t *entity) { (void)entity; return false; }
uint64_t RCScene_AliasRevision(const entity_t *entity) { (void)entity; return 0; }
bool RCScene_AliasPersistent(const entity_t *entity) { (void)entity; return false; }
bool RCScene_AppendCompiledWorld(rc_scene_t *scene,const qmodel_t *world,rc_scene_alias_material_fn material,void *user)
{ (void)scene;(void)material;(void)user;assert(!world->static_models);return true; }
int main (void)
{
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
	VkFenceCreateInfo fi = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags=VK_FENCE_CREATE_SIGNALED_BIT};
	VkFence fence; assert(vkCreateFence(device,&fi,NULL,&fence)==VK_SUCCESS);
	VkFenceCreateInfo unsignaled_info = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence unsignaled; assert(vkCreateFence(device,&unsignaled_info,NULL,&unsignaled)==VK_SUCCESS);
	rc_scene_collect_t collect;
	assert(RCCollect_Create(&collect, physical, device, &memory, &properties.limits, 2, false, 4096));
	uint8_t pixels[] = {255, 128, 32, 255};
	gltexture_t image={0}; image.source_width=image.source_height=1; image.source_format=SRC_RGBA;
	image.source_offset=(src_offset_t)(uintptr_t)pixels; image.shirt=image.pants=-1;
	texture_t texture={0}; texture.gltexture=&image; texture.type=TEXTYPE_DEFAULT;
	uint8_t pixels2[]={10, 20, 30, 255, 40, 50, 60, 128};
	gltexture_t image2=image; image2.source_width=2; image2.source_offset=(src_offset_t)(uintptr_t)pixels2;
	texture_t texture2={0}; texture2.gltexture=&image2; texture2.type=TEXTYPE_DEFAULT;
	mtexinfo_t texinfo={0}; texinfo.texture=&texture;
	glpoly_t poly={0}; poly.numverts=3; poly.verts[1][0]=1; poly.verts[2][1]=1;
	msurface_t surface={0}; surface.texinfo=&texinfo; surface.polys=&poly;
	mtexinfo_t texinfo2={0}; texinfo2.texture=&texture2;
	msurface_t surface2={0}; surface2.texinfo=&texinfo2; surface2.polys=&poly;
	qmodel_t world={0}, prop={0}; world.type=prop.type=mod_brush; world.surfaces=&surface; prop.surfaces=&surface2;
	world.numsurfaces=prop.numsurfaces=1; world.nummodelsurfaces=prop.nummodelsurfaces=1;
	entity_t entity={0}; entity.model=&prop; entity.netstate.scale=ENTSCALE_DEFAULT;
	entity.origin[0]=10;
	qmodel_t unsupported_model={0}; unsupported_model.type=mod_alias;
	entity_t unsupported={0}; unsupported.model=&unsupported_model; unsupported.netstate.scale=ENTSCALE_DEFAULT;
	entity_t *candidates[]={&entity,&unsupported}; float camera[3]={0,0,2};
	assert(RCCollect_SetWorld(&collect,&world));
	qmodel_t invalid_world={0}; invalid_world.type=mod_alias;
	assert(!RCCollect_SetWorld(&collect,&invalid_world) && collect.runtime.world==&world);
	assert(!strcmp(RCCollect_LastError(&collect),"world geometry or material extraction failed"));
	/* Exercise the production light-only coordinator with the collector's
	 * separately registered scene materials and a real Vulkan snapshot. */
	radiance_coordinator_t coordinator;
	radiance_coordinator_limits_t limits={.max_map_lights=4,.max_lights=4,.max_grid_cells=8,.max_grid_indices=32};
	const char map[]="{ \"classname\" \"worldspawn\" \"_sunlight2\" \"90\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"300\" \"style\" \"2\" }";
	assert(RadianceCoordinator_InitLights(&coordinator,&limits));
	assert(RadianceCoordinator_LoadMap(&coordinator,map,sizeof(map)-1));
	int styles[]={256,256,128};
	radiance_light_grid_t grid={{-64,-64,-64},{128,128,128},{1,1,1}};
	radiance_input_snapshot_t lighting;
	assert(RadianceCoordinator_PrepareLights(&coordinator,styles,3,NULL,0,0,&grid,&lighting));
	assert(lighting.light_count==1 && !lighting.material_count && !lighting.atlas_rgba);
	VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	rc_scene_collect_result_t result;
	assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(!RCCollect_Record(&collect,0,unsignaled,command,candidates,2,camera,1,&lighting,false,&result));
	assert(!strcmp(RCCollect_LastError(&collect),"frame fence not signaled"));
	assert(RCCollect_Record(&collect,0,fence,command,candidates,2,camera,1,&lighting,false,&result));
	assert(!RCCollect_LastError(&collect) && !RCCollect_LastWarning(&collect));
	assert(result.scene && result.lighting && !result.hardware && result.triangles==2 && result.materials==2 && result.stats.selected==1 && result.stats.omitted_unsupported==1);
	assert(collect.materials.entries[0].brush_frame==0 && collect.materials.entries[1].brush_frame==0 && collect.materials.brush_frame==0);
	const radiance_lighting_parameters_t *parameters=collect.inputs.frame[0].buffers[1].mapped;
	const radiance_light_t *light=collect.inputs.frame[0].buffers[0].mapped;
	assert(parameters->counts[0]==1 && parameters->counts[1]==2 && parameters->counts[2]==1 && parameters->counts[3]==5);
	assert(parameters->sky_radiance[3]==coordinator.map_stats.sky_upper[3]);
	assert(light[0].color_intensity[3]==coordinator.map_lights[0].color_intensity[3]*0.5f);
	const radiance_material_t *uploaded=collect.inputs.frame[0].buffers[2].mapped;
	assert(uploaded[0].albedo_rect[0]==0 && uploaded[1].albedo_rect[0]==1 && uploaded[1].albedo_rect[2]==2);
	const uint8_t *staging=collect.inputs.frame[0].staging.mapped;
	assert(!memcmp(staging,pixels,4) && !memcmp(staging+4,pixels2,8));
	assert(!RCCollect_SetWorld(&collect,&world));
	assert(!strcmp(RCCollect_LastError(&collect),"world change has unresolved recording"));
	assert(vkEndCommandBuffer(command)==VK_SUCCESS);
	assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS); RCCollect_Discard(&collect,0);
	assert(!collect.pending[0] && !collect.ready[0] && collect.slot_generation[0]==0);
	assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_Record(&collect,0,fence,command,candidates,2,camera,1,&lighting,true,&result));
	assert(!RCCollect_LastError(&collect) && !strcmp(RCCollect_LastWarning(&collect),"ray-query backend unavailable; using software tracing"));
	assert(vkEndCommandBuffer(command)==VK_SUCCESS);
	assert(vkResetFences(device,1,&fence)==VK_SUCCESS);
	VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
	assert(vkQueueSubmit(queue,1,&submit,fence)==VK_SUCCESS); RCCollect_Submitted(&collect,0);
	assert(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
	assert(collect.ready[0] && collect.slot_generation[0]==collect.generation);
	entity.origin[0]=20;
	assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS); assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_Record(&collect,0,fence,command,candidates,2,camera,1,&lighting,false,&result));
	assert(result.stats.refit && result.triangles==2);
	assert(vkEndCommandBuffer(command)==VK_SUCCESS); assert(vkResetFences(device,1,&fence)==VK_SUCCESS);
	assert(vkQueueSubmit(queue,1,&submit,fence)==VK_SUCCESS); RCCollect_Submitted(&collect,0);
	assert(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
	/* Alternate animation frames must get distinct material IDs, and the
	 * context must reset so later world and brush extraction stays correct. */
	uint64_t before_texture_animation=collect.geometry_generation;
	entity.frame=5;
	assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS); assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_Record(&collect,0,fence,command,candidates,2,camera,1,&lighting,false,&result));
	assert(result.stats.refit && result.materials==2 && collect.materials.entries[1].brush_frame==1 && !collect.materials.brush_frame);
	assert(!result.stats.geometry_changed && collect.geometry_generation==before_texture_animation);
	assert(vkEndCommandBuffer(command)==VK_SUCCESS); assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS);
	RCCollect_Discard(&collect,0);
	/* Once an entity leaves, its texture may be freed or recycled. Retired
	 * material entries must not be loaded into the next atlas. Reintroducing
	 * the entity acquires a fresh entry, while the retained world stays valid. */
	entity.model=NULL; image2.source_offset=0;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	assert(collect.materials.count==1 && collect.materials.entries[0].brush==&texture);
	assert(collect.runtime.scene.triangle_count==1 && collect.runtime.scene.triangles[0].metadata[0]==0);
	uint64_t stable_generation=collect.generation, stable_atlas=collect.atlas_revision, stable_upload=collect.atlas_upload_generation;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	assert(collect.generation==stable_generation && collect.atlas_revision==stable_atlas && collect.atlas_upload_generation==stable_upload);
	/* Pixel inventory includes inactive brush frames. Switching the logical
	 * frame must update the material table without replacing or uploading the
	 * atlas, even with a real fenced Vulkan input snapshot. */
	uint8_t next_pixels[]={20,40,80,255};gltexture_t next_image=image;
	next_image.source_offset=(src_offset_t)(uintptr_t)next_pixels;
	texture_t next_texture=texture;next_texture.gltexture=&next_image;
	texture.anim_next=&next_texture;next_texture.anim_next=&texture;
	texture.anim_total=next_texture.anim_total=2;texture.anim_max=1;
	next_texture.anim_min=1;next_texture.anim_max=2;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	uint8_t *stable_pixels=collect.atlas.rgba;size_t stable_bytes=collect.atlas.bytes;
	stable_upload=collect.atlas_upload_generation;stable_atlas=collect.atlas_revision;
	assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_RecordPrepared(&collect,0,fence,command,&lighting,false,&result));
	assert(collect.inputs.frame[0].atlas_dirty);
	assert(vkEndCommandBuffer(command)==VK_SUCCESS);assert(vkResetFences(device,1,&fence)==VK_SUCCESS);
	assert(vkQueueSubmit(queue,1,&submit,fence)==VK_SUCCESS);RCCollect_Submitted(&collect,0);
	assert(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
	advance_animation=true;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	assert(collect.atlas.rgba==stable_pixels && collect.atlas.bytes==stable_bytes &&
		collect.atlas_upload_generation==stable_upload && collect.atlas_revision!=stable_atlas);
	const radiance_material_t *active=&collect.atlas.materials[0];
	const uint8_t *active_pixel=collect.atlas.rgba+(((size_t)active->layers_flags[0]*collect.atlas.height+
		active->albedo_rect[1])*collect.atlas.width+active->albedo_rect[0])*4;
	assert(!memcmp(active_pixel,next_pixels,4));
	assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS);assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_RecordPrepared(&collect,0,fence,command,&lighting,false,&result));
	assert(!collect.inputs.frame[0].atlas_dirty && collect.inputs.frame[0].atlas_revision==stable_upload);
	uploaded=collect.inputs.frame[0].buffers[2].mapped;
	assert(!memcmp(uploaded[0].albedo_rect,active->albedo_rect,sizeof(active->albedo_rect)));
	assert(vkEndCommandBuffer(command)==VK_SUCCESS);assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS);
	RCCollect_Discard(&collect,0);
	advance_animation=false;texture.anim_next=NULL;texture.anim_total=texture.anim_min=texture.anim_max=0;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	image2.source_offset=(src_offset_t)(uintptr_t)pixels2; entity.model=&prop;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	assert(collect.materials.count==2 && collect.runtime.scene.triangles[1].metadata[0]==1);
	/* Brush/template identity can change through compaction while active
	 * base/emission image pointers, material count and source hash stay equal. */
	{
		memcpy(texture2.name,"matched",sizeof("matched"));
		radiance_surface_template_t template={0};
		memcpy(template.texture,"matched",sizeof("matched"));
		RCCollect_SetSurfaceTemplates(&collect,&template,1);
		assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
		assert(collect.surface_material_mask && collect.surface_material_mask[1]);
		uint64_t shared_source=collect.material_source_revision;
		texture_t shared=texture2;memcpy(shared.name,"unmatched",sizeof("unmatched"));
		mtexinfo_t shared_info=texinfo2;shared_info.texture=&shared;
		msurface_t shared_surface=surface2;shared_surface.texinfo=&shared_info;
		qmodel_t shared_prop=prop;shared_prop.surfaces=&shared_surface;entity.model=&shared_prop;
		assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
		assert(collect.materials.count==2 && collect.material_source_revision==shared_source);
		assert(collect.surface_material_mask && !collect.surface_material_mask[1]);
		entity.model=&prop;texture2.name[0]=0;RCCollect_SetSurfaceTemplates(&collect,NULL,0);
		assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	}
	/* Replace a model without changing triangle count. Its old texture is
	 * invalid by the time the next atlas is built, but must not be read. */
	uint8_t pixels3[]={80,90,100,255};
	gltexture_t image3=image; image3.source_offset=(src_offset_t)(uintptr_t)pixels3;
	texture_t texture3={0}; texture3.gltexture=&image3; texture3.type=TEXTYPE_DEFAULT;
	mtexinfo_t texinfo3={0}; texinfo3.texture=&texture3;
	msurface_t surface3={0}; surface3.texinfo=&texinfo3; surface3.polys=&poly;
	qmodel_t prop3=prop; prop3.surfaces=&surface3;
	entity.model=&prop3; image2.source_offset=0;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	assert(collect.stats.materials_changed && collect.materials.count==2 &&
		collect.materials.entries[1].brush==&texture3 && collect.runtime.scene.triangles[1].metadata[0]==1);
	image2.source_offset=(src_offset_t)(uintptr_t)pixels2; entity.model=&prop;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	/* An extraction can add a new material before a later entity fails.
	 * A retry with the original, unchanged scene must prune that orphan. */
	entity_t extra={0}, broken={0}; extra.model=&prop3; extra.netstate.scale=ENTSCALE_DEFAULT;
	msurface_t broken_surface=surface3; broken_surface.polys=NULL;
	qmodel_t broken_prop=prop3; broken_prop.surfaces=&broken_surface;
	broken.model=&broken_prop; broken.netstate.scale=ENTSCALE_DEFAULT;
	entity_t *failed_candidates[]={&entity,&extra,&broken};
	assert(!RCCollect_PrepareScene(&collect,failed_candidates,3,camera,1,NULL,0,NULL));
	assert(collect.materials_need_compaction && collect.materials.count==3);
	image3.source_offset=0;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,1,NULL,0,NULL));
	assert(!collect.materials_need_compaction && collect.materials.count==2);
	/* Missing runtime material is a hard failure; old scene is never returned. */
	image.source_offset=0;
	assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS); assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(!RCCollect_Record(&collect,0,fence,command,candidates,2,camera,1,&lighting,false,&result));
	assert(!strcmp(RCCollect_LastError(&collect),"material atlas build failed"));
	assert(!result.scene && !result.lighting);
	assert(vkEndCommandBuffer(command)==VK_SUCCESS); assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS);
	image.source_offset=(src_offset_t)(uintptr_t)pixels;
	assert(RCCollect_SetWorld(&collect,&world));
	assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_Record(&collect,0,fence,command,candidates,2,camera,2,&lighting,false,&result));
	assert(result.stats.rebuilt && result.triangles==2);
	assert(vkEndCommandBuffer(command)==VK_SUCCESS); assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS);
	RCCollect_Discard(&collect,0);
	/* The production two-phase path derives a real emissive source from the
	 * same atlas/scene IDs that are uploaded to Vulkan. */
	uint8_t emission_pixel[]={255,192,64,255};
	gltexture_t emission=image; emission.source_offset=(src_offset_t)(uintptr_t)emission_pixel;
	texture.fullbright=&emission; gl_fullbrights.value=1;
	assert(RCCollect_SetWorld(&collect,&world));
	radiance_light_t emitters[8]; radiance_emissive_stats_t emission_stats;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,3,emitters,8,&emission_stats));
	assert(emission_stats.emitted_lights==1 && !emission_stats.omitted_lights &&
		(collect.runtime.scene.triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	/* Changing which emissive surfaces have proxies changes shader metadata,
	 * not map geometry; hardware AS must remain reusable. */
	uint64_t geometry_generation=collect.geometry_generation, scene_generation=collect.generation;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,3,NULL,0,&emission_stats));
	assert(collect.geometry_generation==geometry_generation && collect.generation>scene_generation);
	assert(!(collect.runtime.scene.triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,3,emitters,8,&emission_stats));
	assert(collect.geometry_generation==geometry_generation && emission_stats.emitted_lights==1);
	assert(RadianceCoordinator_PrepareLightsWithEmitters(&coordinator,styles,3,NULL,0,0,&grid,
		emitters,emission_stats.emitted_lights,&lighting));
	assert(lighting.light_count==2 && lighting.lights[1].metadata[0]==RADIANCE_LIGHT_EMISSIVE);
	assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_RecordPrepared(&collect,0,fence,command,&lighting,false,&result));
	const radiance_light_t *uploaded_lights=collect.inputs.frame[0].buffers[0].mapped;
	assert(uploaded_lights[1].metadata[0]==RADIANCE_LIGHT_EMISSIVE && uploaded_lights[1].area_axis[3]>0 && uploaded_lights[1].direction_cone[3]>0);
	assert(vkEndCommandBuffer(command)==VK_SUCCESS); assert(vkResetCommandPool(device,pool,0)==VK_SUCCESS);
	RCCollect_Discard(&collect,0);
	/* A wrapped atlas identity cannot match either slot's old GPU data. */
	collect.atlas_upload_generation=UINT64_MAX; collect.atlas_valid=false;
	for(uint32_t i=0;i<collect.frames;++i) collect.inputs.frame[i].atlas_revision=1;
	assert(RCCollect_PrepareScene(&collect,candidates,2,camera,3,emitters,8,&emission_stats));
	assert(collect.atlas_upload_generation==1);
	for(uint32_t i=0;i<collect.frames;++i) assert(collect.inputs.frame[i].atlas_revision==0);
	/* A world whose only material is sky has no source texels; it still needs
	 * a real GPU atlas image and must not fall back to classic lighting. */
	texture_t sky_texture={0}; sky_texture.type=TEXTYPE_SKY;
	mtexinfo_t sky_info={0}; sky_info.texture=&sky_texture;
	msurface_t sky_surface={0}; sky_surface.texinfo=&sky_info; sky_surface.polys=&poly; sky_surface.flags=SURF_DRAWSKY;
	qmodel_t sky_world={0}; sky_world.type=mod_brush; sky_world.surfaces=&sky_surface;
	sky_world.numsurfaces=sky_world.nummodelsurfaces=1;
	assert(RCCollect_SetWorld(&collect,&sky_world));
	assert(RadianceCoordinator_PrepareLights(&coordinator,styles,3,NULL,0,0,&grid,&lighting));
	assert(RCCollect_PrepareScene(&collect,NULL,0,camera,4,NULL,0,&emission_stats));
	assert(collect.atlas.width==1 && collect.atlas.height==1 && collect.atlas.layers==1 && collect.atlas.bytes==4);
	assert((collect.runtime.scene.triangles[0].metadata[1]&RC_SCENE_SKY)!=0);
	assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
	assert(RCCollect_RecordPrepared(&collect,0,fence,command,&lighting,false,&result));
	assert(result.scene && result.lighting && result.materials==1 && result.triangles==1);
	assert(vkEndCommandBuffer(command)==VK_SUCCESS); assert(vkResetFences(device,1,&fence)==VK_SUCCESS);
	assert(vkQueueSubmit(queue,1,&submit,fence)==VK_SUCCESS); RCCollect_Submitted(&collect,0);
	assert(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
	RCCollect_Destroy(&collect); RadianceCoordinator_Clear(&coordinator);
	vkDestroyFence(device,unsignaled,NULL); vkDestroyFence(device,fence,NULL);
	vkDestroyCommandPool(device,pool,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
	puts("radiance scene collection passed"); return 0;
}
