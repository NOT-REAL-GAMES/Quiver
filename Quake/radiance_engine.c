/* Frame ownership for the optional world-space radiance renderer. */
#include "quakedef.h"
#include "radiance_runtime.h"
#include "radiance_receiver_capture.h"
#include "radiance_coordinator.h"
#include "radiance_scene_collect.h"
#include "radiance_scene_alias.h"
#include "radiance_receiver_vulkan.h"
#include "radiance_graphics.h"
#include "radiance_receiver_experimental.h"

#include <math.h>
#include <string.h>

typedef struct
{
	rc_scene_collect_t scene;
	rc_scene_ecs_batch_t ecs_batch;
	radiance_coordinator_t lights;
	rc_sparse_t *sparse;
	rc_vulkan_t cascades;
	rc_receiver_vk_t receiver;
	rc_graphics_t graphics[2];
	bool graphics_attempted[2];
	rc_scheduler_t scheduler;
	rc_budget_t budget;
	rc_sparse_refresh_work_t refresh[DOUBLE_BUFFERED];
	bool refresh_reserved[DOUBLE_BUFFERED];
	radiance_light_t *previous_lights;
	float *previous_dynamic_radius;
	entity_t **occluder_candidates;
	uint32_t occluder_candidate_capacity;
	size_t previous_light_count;
	float previous_sky[8], previous_gains[4];
	uint64_t previous_atlas_revision, frame_serial;
	bool light_history, fine_grid_disabled, sky_portal, previous_sky_portals_only;
	VkQueryPool queries;
	uint64_t timestamp_mask;
	bool timestamps_written[DOUBLE_BUFFERED];
	bool pending[DOUBLE_BUFFERED], attempted[DOUBLE_BUFFERED], initialized, failed, map_dirty;
	VkCommandBuffer commands[DOUBLE_BUFFERED];
	const qmodel_t *world;
	double gpu_ms;
	bool timing_available;
	float spacing_attempted;
} radiance_engine_t;

static radiance_engine_t rc_engine;

bool R_RadianceGeometryCounts (uint32_t counts[4])
{
	if (!counts) return false;
	memset (counts, 0, 4 * sizeof (*counts));
	if (!R_RadianceActive () || !rc_engine.initialized || !rc_engine.scene.scene_prepared ||
		rc_engine.world != cl.worldmodel) return false;
	counts[0] = rc_engine.scene.runtime.scene.triangle_count;
	counts[1] = rc_engine.scene.runtime.static_scene.triangle_count;
	counts[2] = rc_engine.scene.runtime.base_scene.triangle_count;
	counts[3] = rc_engine.world && rc_engine.world->static_models ? rc_engine.world->static_models->triangle_count : 0;
	return true;
}

bool R_RadianceExperimentalBindings(uint32_t slot, rc_experimental_bindings_t *out)
{
	const radiance_engine_t *e=&rc_engine;
	const rc_graphics_t *graphics=RCGraphics_Frame();
	if (!out || !e->initialized || slot>=DOUBLE_BUFFERED || !e->pending[slot] || !graphics) return false;
	VkDescriptorSetLayout layout=graphics->hardware ? e->receiver.hardware_layout : e->receiver.software_layout;
	rc_experimental_bindings_t result={.graphics=layout,.cascade=e->cascades.cascade_layout,.lighting=e->scene.inputs.layout,
		.cascade_set=RCGraphics_FrameDescriptor(),.lighting_set=RadianceInputs_Descriptor(&e->scene.inputs,slot),
		.hardware=graphics->hardware};
	if (!result.graphics || !result.cascade || !result.lighting || !result.cascade_set || !result.lighting_set) return false;
	*out=result; return true;
}

static void RCFrame_ClearPublication (const char *reason)
{
	RCGraphics_SetFrame (NULL, VK_NULL_HANDLE);
	R_RadianceReset (reason);
}

bool R_RadianceFrameApplySpacing (void)
{
	/* Called on the main thread before this frame's recording tasks exist. */
	assert (!Tasks_IsWorker ());
	radiance_engine_t *e=&rc_engine;
	if (r_radiance.value<=0 || !e->initialized || !e->sparse)
	{ e->spacing_attempted=0; return true; }
	const float requested=RC_ReceiverSpacing (r_radiance_probe_spacing.value);
	const rc_layout_t *current=RC_SparseLayout (e->sparse);
	if (current && current->levels[0].origin_spacing[3]==requested)
	{ e->spacing_attempted=0; return true; }
	/* Retrying an allocation failure every frame would stall ordinary play.
	 * Selecting the active spacing, or disabling Radiance, permits a retry. */
	if (e->spacing_attempted==requested) return false;
	e->spacing_attempted=requested;
	const uint32_t capacities[RC_CASCADE_COUNT]={512,64,8,8,8};
	rc_sparse_t *pending=RC_SparseCreate (32,2,requested,requested*2.0f,capacities);
	const rc_layout_t *layout=RC_SparseLayout (pending);
	uint32_t table_count=0;
	const uint32_t *table=RC_SparseTable (pending,&table_count);
	if (!layout || !table || layout->record_count!=e->cascades.record_count || table_count!=e->cascades.table_capacity)
	{
		RC_SparseDestroy (pending);
		Con_Printf ("Radiance: probe spacing change to %g unavailable; keeping existing probes\n",requested);
		return false;
	}
	/* Join the previous submission task and drain both GPU slots before any
	 * probe identity changes. Scene/atlas/pipelines/descriptors stay intact. */
	GL_WaitForDeviceIdle ();
	bool busy=e->cascades.recorded_commands!=VK_NULL_HANDLE;
	for (uint32_t i=0;i<DOUBLE_BUFFERED;++i) busy|=e->pending[i] || e->refresh_reserved[i];
	if (busy)
	{
		RC_SparseDestroy (pending);
		Con_Printf ("Radiance: probe spacing change deferred by unfinished recording\n");
		return false;
	}
	RCFrame_ClearPublication ("probe spacing changed");
	rc_sparse_t *previous=e->sparse;
	e->sparse=pending;
	e->cascades.needs_clear=true; /* Clear both raw and merged validity. */
	e->cascades.submitted_revision=0;
	e->cascades.submitted_dirty_mask=e->cascades.recorded_dirty_mask=0;
	e->cascades.recorded_merge_intervals=0;
	memset (e->cascades.snapshot_ready,0,sizeof(e->cascades.snapshot_ready));
	memset (e->cascades.snapshot_revision,0,sizeof(e->cascades.snapshot_revision));
	memset (e->cascades.clear_count,0,sizeof(e->cascades.clear_count));
	memset (e->cascades.work,0,sizeof(e->cascades.work));
	memset (e->refresh,0,sizeof(e->refresh));
	memset (e->timestamps_written,0,sizeof(e->timestamps_written));
	RC_SchedulerReset (&e->scheduler);
	e->light_history=false;
	e->timing_available=false;
	e->spacing_attempted=0;
	RC_SparseDestroy (previous);
	return true;
}

void R_RadianceFrameShutdown (void)
{
	radiance_engine_t *e = &rc_engine;
	RCFrame_ClearPublication ("renderer resources unavailable");
	/* Caller has waited for both frame slots and the device. Graphics must go
	 * first because its pipeline layouts reference the receiver layout. */
	for (unsigned i = 0; i < 2; ++i) RCGraphics_Destroy (&e->graphics[i]);
	RCReceiverVk_Destroy (&e->receiver);
	RCVk_Destroy (&e->cascades);
	RCCollect_Destroy (&e->scene);
	RCScene_ECSBatchClear (&e->ecs_batch);
	RC_SparseDestroy (e->sparse);
	RadianceCoordinator_Clear (&e->lights);
	free (e->previous_lights);
	free (e->previous_dynamic_radius);
	free (e->occluder_candidates);
	if (e->queries) vkDestroyQueryPool (vulkan_globals.device, e->queries, NULL);
	memset (e, 0, sizeof (*e));
}

void R_RadianceFrameNewMap (void)
{
	rc_engine.map_dirty = true;
	RCFrame_ClearPublication ("map resources not ready");
}

static void RCFrame_CreateTiming (radiance_engine_t *e, VkPhysicalDevice physical)
{
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties (physical, &count, NULL);
	if (!count || vulkan_globals.gfx_queue_family_index >= count ||
		!vulkan_globals.device_properties.limits.timestampComputeAndGraphics ||
		vulkan_globals.device_properties.limits.timestampPeriod <= 0) return;
	VkQueueFamilyProperties *families = malloc ((size_t)count * sizeof (*families));
	if (!families) return;
	vkGetPhysicalDeviceQueueFamilyProperties (physical, &count, families);
	const uint32_t bits = families[vulkan_globals.gfx_queue_family_index].timestampValidBits;
	free (families);
	if (!bits) return;
	e->timestamp_mask = bits >= 64 ? UINT64_MAX : (UINT64_C (1) << bits) - 1;
	VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * DOUBLE_BUFFERED};
	if (vkCreateQueryPool (vulkan_globals.device, &info, NULL, &e->queries) != VK_SUCCESS)
		e->queries = VK_NULL_HANDLE;
}

static bool RCFrame_Create (radiance_engine_t *e)
{
	const VkPhysicalDevice physical = GL_RadiancePhysicalDevice ();
	const VkPhysicalDeviceLimits *limits = &vulkan_globals.device_properties.limits;
	const uint32_t capacities[RC_CASCADE_COUNT] = {512, 64, 8, 8, 8};
	/* Scale trace intervals with probe spacing. */
	const float probe_spacing = RC_ReceiverSpacing (r_radiance_probe_spacing.value);
	const radiance_coordinator_limits_t source_limits = {
		.max_map_lights = 16384, .max_lights = 16704,
		.max_grid_cells = 32768, .max_grid_indices = 4194304};
	if (!physical || !(e->previous_lights = malloc (source_limits.max_lights * sizeof (*e->previous_lights))) ||
		!(e->previous_dynamic_radius = calloc (source_limits.max_lights, sizeof (*e->previous_dynamic_radius))) ||
		!RadianceCoordinator_InitLights (&e->lights, &source_limits) ||
		!(e->sparse = RC_SparseCreate (32, 2, probe_spacing, probe_spacing * 2.0f, capacities)) ||
		!RCCollect_Create (&e->scene, physical, vulkan_globals.device, &vulkan_globals.memory_properties,
			/* Bound decoded sources and exact-texture atlas separately. This is
			 * a ceiling; ordinary maps still allocate only their packed texels. */
			limits, DOUBLE_BUFFERED, vulkan_globals.ray_query, 512u * 1024u * 1024u)) goto fail;
	rc_vk_shader_t sw = {(const uint32_t *)radiance_trace_sw_comp_spv, (size_t)radiance_trace_sw_comp_spv_size};
	rc_vk_shader_t rt = {(const uint32_t *)radiance_trace_rt_comp_spv, (size_t)radiance_trace_rt_comp_spv_size};
	rc_vk_shader_t merge = {(const uint32_t *)radiance_merge_comp_spv, (size_t)radiance_merge_comp_spv_size};
	if (!RCVk_Create (&e->cascades, vulkan_globals.device, &vulkan_globals.memory_properties, limits, e->sparse,
		DOUBLE_BUFFERED, e->scene.scene_gpu.software_layout, e->scene.scene_gpu.hardware_layout, e->scene.inputs.layout, sw, rt, merge, R_PipelineCache ()) ||
		!RCReceiverVk_Create (&e->receiver, vulkan_globals.device, &vulkan_globals.memory_properties, limits,
			DOUBLE_BUFFERED, e->scene.rt_available) ||
		!RC_BudgetInit (&e->budget, 256, 16384, 2048, 2.0)) goto fail;
	RC_SchedulerReset (&e->scheduler);
	RCFrame_CreateTiming (e, physical);
	e->map_dirty = true;
	e->initialized = true;
	return true;
fail:
	R_RadianceFrameShutdown ();
	e->failed = true;
	return false;
}

/* Compiling the other backend's full family of graphics pipelines can take
 * seconds. Create it only if it is selected or needed for fallback. */
static bool RCFrame_EnsureGraphics (radiance_engine_t *e, bool hardware)
{
	const unsigned index = hardware ? 1u : 0u;
	if (e->graphics[index].ready) return RCGraphics_EnsureCurrentVariant (&e->graphics[index]);
	if (e->graphics_attempted[index]) return false;
	e->graphics_attempted[index] = true;
	VkDescriptorSetLayout layout = hardware ? e->receiver.hardware_layout : e->receiver.software_layout;
	return layout && (!hardware || e->cascades.trace_hardware) && RCGraphics_Create (&e->graphics[index], layout, hardware);
}

static void RCFrame_ReadTiming (radiance_engine_t *e, uint32_t slot)
{
	if (!e->queries || !e->timestamps_written[slot]) return;
	struct { uint64_t value, available; } samples[2] = {{0}};
	VkResult result = vkGetQueryPoolResults (vulkan_globals.device, e->queries, slot * 2, 2,
		sizeof (samples), samples, sizeof (samples[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	if (result == VK_SUCCESS && samples[0].available && samples[1].available)
	{
		e->gpu_ms = (double)((samples[1].value - samples[0].value) & e->timestamp_mask) *
			vulkan_globals.device_properties.limits.timestampPeriod / 1000000.0;
		e->timing_available = isfinite (e->gpu_ms) && e->gpu_ms > 0;
		if (e->timing_available) RC_BudgetUpdate (&e->budget, e->gpu_ms);
	}
	e->timestamps_written[slot] = false;
}

/* The renderer's visible list includes every received network entity but only
 * PVS-visible static entities. Include the remaining client-known statics so
 * they can cast into visible geometry from outside the current PVS. */
static bool RCFrame_AssembleOccluders (radiance_engine_t *e, uint32_t *count)
{
	entity_t *batch = NULL;
	uint32_t batch_count = CL_ECSVisualBatch (&batch);
	if (cl.num_statics < 0 || cl_numvisedicts < 0 ||
		(batch_count && !batch) || (uint64_t)cl.num_statics + (uint64_t)cl_numvisedicts + batch_count > UINT32_MAX) return false;
	uint32_t needed = (uint32_t)cl.num_statics + (uint32_t)cl_numvisedicts + batch_count;
	if (needed > e->occluder_candidate_capacity)
	{
		uint32_t capacity = e->occluder_candidate_capacity ? e->occluder_candidate_capacity : 256;
		while (capacity < needed)
		{
			if (capacity > UINT32_MAX / 2) { capacity = needed; break; }
			capacity *= 2;
		}
		if ((uint64_t)capacity * sizeof (*e->occluder_candidates) > SIZE_MAX) return false;
		entity_t **storage = realloc (e->occluder_candidates, (size_t)capacity * sizeof (*storage));
		if (!storage) return false;
		e->occluder_candidates = storage;
		e->occluder_candidate_capacity = capacity;
	}
	return RCSceneRuntime_AssembleCandidates (e->occluder_candidates, e->occluder_candidate_capacity,
		cl.static_entities, (uint32_t)cl.num_statics, cl_visedicts, (uint32_t)cl_numvisedicts, count) &&
		RCSceneRuntime_AppendBatch (e->occluder_candidates, e->occluder_candidate_capacity, count, batch, batch_count);
}

static bool RCFrame_MarkReceivers (radiance_engine_t *e)
{
	if (!RC_SparseBeginFrame (e->sparse, r_origin) ||
		!RC_SparseMarkAABB (e->sparse, r_origin, r_origin, 0) ||
		!RC_SparseMarkAABB (e->sparse, cl.worldmodel->mins, cl.worldmodel->maxs, 2)) return false;
	for (int i = 0; i < cl_numvisedicts; ++i)
	{
		const entity_t *entity = cl_visedicts[i];
		if (!entity || !entity->model) continue;
		if (entity->compiled_bounds_valid)
		{
			if (!RC_SparseMarkAABB (e->sparse, entity->compiled_mins, entity->compiled_maxs, 1)) return false;
			continue;
		}
		float radius = 0, mins[3], maxs[3];
		for (int axis = 0; axis < 3; ++axis)
			radius = q_max (radius, q_max (fabsf (entity->model->rmins[axis]), fabsf (entity->model->rmaxs[axis])));
		radius *= entity->netstate.scale || entity->modelscale_override > 0 ? R_EntityModelScale (entity) : 1;
		for (int axis = 0; axis < 3; ++axis) { mins[axis] = entity->origin[axis] - radius; maxs[axis] = entity->origin[axis] + radius; }
		if (!RC_SparseMarkAABB (e->sparse, mins, maxs, 1)) return false;
	}
	entity_t *batch = NULL;
	uint32_t batch_count = CL_ECSVisualBatch (&batch);
	if (batch_count && batch)
	{
		float mins[3], maxs[3];
		for (int axis = 0; axis < 3; ++axis) mins[axis] = maxs[axis] = batch[0].origin[axis];
		for (uint32_t i = 1; i < batch_count; ++i)
			for (int axis = 0; axis < 3; ++axis)
			{ mins[axis] = q_min (mins[axis], batch[i].origin[axis]); maxs[axis] = q_max (maxs[axis], batch[i].origin[axis]); }
		/* Expand for the instanced model rather than marking only origins. */
		float radius = 0;
		if (batch[0].model)
			for (int axis = 0; axis < 3; ++axis)
				radius = q_max (radius, q_max (fabsf (batch[0].model->rmins[axis]), fabsf (batch[0].model->rmaxs[axis])));
		for (int axis = 0; axis < 3; ++axis) { mins[axis] -= radius; maxs[axis] += radius; }
		if (!RC_SparseMarkAABB (e->sparse, mins, maxs, 1)) return false;
	}
	return RC_SparseResolve (e->sparse);
}

static bool RCFrame_RequestLightRefresh (radiance_engine_t *e, const radiance_light_t *light)
{
	const uint32_t attenuation = light->metadata[3] & 7u;
	if (light->metadata[0] == RADIANCE_LIGHT_DIRECTIONAL ||
		((light->metadata[2] & RADIANCE_LIGHT_SOURCE_MAP) && (attenuation == 3u || attenuation == 4u)))
		return RC_SparseRequestRefresh (e->sparse, cl.worldmodel->mins, cl.worldmodel->maxs);
	float mins[3], maxs[3], radius = light->position_radius[3];
	if (light->metadata[0] == RADIANCE_LIGHT_EMISSIVE)
		radius += hypotf (light->area_axis[3], light->direction_cone[3]);
	if (!isfinite (radius) || radius < 0) return false;
	for (int axis = 0; axis < 3; ++axis)
	{ mins[axis] = light->position_radius[axis] - radius; maxs[axis] = light->position_radius[axis] + radius; }
	return RC_SparseRequestRefresh (e->sparse, mins, maxs);
}

static bool RCFrame_RefreshChanges (radiance_engine_t *e, const radiance_input_snapshot_t *sources,
	const rc_scene_collect_result_t *scene, const float old_mins[3], const float old_maxs[3], bool old_bounds, uint64_t old_hash)
{
	if (sources->light_count > e->lights.limits.max_lights) return false;
	if (e->light_history)
	{
		const size_t count = q_max (e->previous_light_count, sources->light_count);
		for (size_t i = 0; i < count; ++i)
		{
			const radiance_light_t *before = i < e->previous_light_count ? &e->previous_lights[i] : NULL;
			const radiance_light_t *after = i < sources->light_count ? &sources->lights[i] : NULL;
			if (before && after && !memcmp (before, after, sizeof (*before))) continue;
			if ((before && !RCFrame_RequestLightRefresh (e, before)) || (after && !RCFrame_RequestLightRefresh (e, after))) return false;
			/* Clear expedited transient radiance promptly when a source is removed
			 * or relocated; otherwise its last sampled bounce can persist for a
			 * full round-robin sweep after the light has gone. */
			const bool same_dynamic = before && after && (before->metadata[2] & RADIANCE_LIGHT_SOURCE_DYNAMIC) &&
				(after->metadata[2] & RADIANCE_LIGHT_SOURCE_DYNAMIC) &&
				!memcmp (before->position_radius, after->position_radius, 3 * sizeof (float));
			if (before && (before->metadata[2] & RADIANCE_LIGHT_SOURCE_DYNAMIC) && !same_dynamic)
			{
				/* Decay shrinks the last radius below the volume that was lit at
				 * its peak. Invalidate that whole former volume on removal. */
				radiance_light_t old = *before;
				old.position_radius[3] = q_max (old.position_radius[3], e->previous_dynamic_radius[i]);
				if (!RCFrame_RequestLightRefresh (e, &old) ||
					!RC_SparsePrioritizeExpiredInfluence (e->sparse, old.position_radius, old.position_radius[3] + 64.0f)) return false;
			}
			if (after && (after->metadata[2] & RADIANCE_LIGHT_SOURCE_DYNAMIC))
				e->previous_dynamic_radius[i] = same_dynamic ? q_max (e->previous_dynamic_radius[i], after->position_radius[3]) : after->position_radius[3];
			else e->previous_dynamic_radius[i] = 0;
			/* Short-lived effects can expire before a round-robin pass reaches
			 * visible receivers. Expedite a nearby affected fine brick, while
			 * keeping a fair share for the rest of the refresh queue. */
			if (after && (after->metadata[2] & RADIANCE_LIGHT_SOURCE_DYNAMIC) &&
				!RC_SparsePrioritizeInfluence (e->sparse, after->position_radius, after->position_radius[3] + 64.0f, r_origin)) return false;
		}
		/* Sky changes affect the whole transport field. Keeping the old valid
		 * samples during a budgeted refresh leaves disabled or recolored sky in
		 * both receiver lighting and recursive bounce, potentially for several
		 * sweeps. Start from runtime direct light instead and rebuild the field. */
		const bool sky_changed = Radiance_SkyLightingChanged (e->previous_sky, e->previous_sky_portals_only, &sources->parameters);
		if (sky_changed)
		{
			e->cascades.needs_clear = true;
			RC_SchedulerReset (&e->scheduler);
		}
		if (sky_changed || e->previous_gains[0] != sources->parameters.grid_origin[3] ||
			e->previous_gains[1] != sources->parameters.grid_inv_cell_size[3] ||
			e->previous_gains[2] != sources->parameters.bounce_parameters[0] ||
			e->previous_gains[3] != sources->parameters.bounce_parameters[1] ||
			e->previous_atlas_revision != e->scene.atlas_revision)
			if (!RC_SparseRequestRefresh (e->sparse, cl.worldmodel->mins, cl.worldmodel->maxs)) return false;
		if (scene->stats.rebuilt || scene->stats.refit)
		{
			float mins[3], maxs[3];
			uint64_t hash;
			bool bounds = RCSceneRuntime_DynamicBounds (&e->scene.runtime, mins, maxs, &hash);
			if (old_hash != hash || old_bounds != bounds)
			{
				if (old_bounds && !RC_SparseRequestRefresh (e->sparse, old_mins, old_maxs)) return false;
				if (bounds && !RC_SparseRequestRefresh (e->sparse, mins, maxs)) return false;
			}
		}
	}
	if (!e->light_history)
		for (size_t i = 0; i < sources->light_count; ++i)
			e->previous_dynamic_radius[i] = (sources->lights[i].metadata[2] & RADIANCE_LIGHT_SOURCE_DYNAMIC) ? sources->lights[i].position_radius[3] : 0;
	if (sources->light_count) memcpy (e->previous_lights, sources->lights, sources->light_count * sizeof (*sources->lights));
	e->previous_light_count = sources->light_count;
	memcpy (e->previous_sky, sources->parameters.sky_radiance, 4 * sizeof (float));
	memcpy (e->previous_sky + 4, sources->parameters.ground_radiance, 4 * sizeof (float));
	e->previous_sky_portals_only = (sources->parameters.counts[3] & 16u) != 0;
	e->previous_gains[0] = sources->parameters.grid_origin[3];
	e->previous_gains[1] = sources->parameters.grid_inv_cell_size[3];
	e->previous_gains[2] = sources->parameters.bounce_parameters[0];
	e->previous_gains[3] = sources->parameters.bounce_parameters[1];
	e->previous_atlas_revision = e->scene.atlas_revision;
	e->light_history = true;
	return true;
}

bool R_RadianceFramePrepare (uint32_t slot, VkFence fence, VkCommandBuffer commands)
{
	radiance_engine_t *e = &rc_engine;
	RCFrame_ClearPublication (r_radiance.value > 0 ? "frame resources not ready" : "disabled");
	if (r_radiance.value <= 0 || !cl.worldmodel || !commands || slot >= DOUBLE_BUFFERED || e->pending[slot]) return false;
	e->attempted[slot] = false;
	if (e->failed || (!e->initialized && !RCFrame_Create (e)))
	{
		RCFrame_ClearPublication ("unsupported device or radiance resource creation failed");
		return false;
	}
	RCFrame_ReadTiming (e, slot);
	if (e->map_dirty || e->world != cl.worldmodel)
	{
		const char *entities = cl.worldmodel->entities ? cl.worldmodel->entities : "";
		RCScene_ECSBatchClear (&e->ecs_batch);
		if (!RCCollect_SetWorld (&e->scene, cl.worldmodel) || !RadianceCoordinator_LoadMap (&e->lights, entities, strlen (entities)))
		{
			RCFrame_ClearPublication ("map scene or runtime light sources unavailable");
			return false;
		}
		RCCollect_SetSurfaceTemplates (&e->scene, e->lights.surface_templates, e->lights.surface_template_count);
		e->sky_portal = false;
        for (uint32_t i = 0; i < e->scene.runtime.static_scene.triangle_count; ++i)
            if (e->scene.runtime.static_scene.triangles[i].metadata[1] & RC_SCENE_SKY)
            { e->sky_portal = true; break; }
        e->world = cl.worldmodel; e->map_dirty = false;
		e->fine_grid_disabled = false;
		e->light_history = false;
		e->cascades.needs_clear = true;
		RC_SchedulerReset (&e->scheduler);
	}
	e->attempted[slot] = true;
	if (e->queries)
	{
		vkCmdResetQueryPool (commands, e->queries, slot * 2, 2);
		vkCmdWriteTimestamp (commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, e->queries, slot * 2);
	}
	radiance_light_grid_t coarse_grid = {.cell_size = {512, 512, 512}, .dimensions = {16, 16, 16}}, grid;
	for (int axis = 0; axis < 3; ++axis) coarse_grid.origin[axis] = floorf (r_origin[axis] / 512.0f) * 512.0f - 4096.0f;
	bool fine_grid = r_radiance_grid.value > 0 && !e->fine_grid_disabled && Radiance_PlanLightGrid (cl.worldmodel->mins, cl.worldmodel->maxs, &grid);
	if (!fine_grid) grid = coarse_grid;
	radiance_input_snapshot_t sources;
	rc_scene_collect_result_t scene;
	radiance_light_t emitters[256];
	radiance_emissive_stats_t emissive_stats;
	const char *failure = "radiance frame preparation failed";
	const bool profile = r_radiance_profile.value > 0;
	double cpu_time[7] = {0};
	if (profile) cpu_time[0] = Sys_DoubleTime ();
	bool want_hardware = (int)r_radiance_backend.value != 1 && RCFrame_EnsureGraphics (e, true);
	if (!want_hardware && !RCFrame_EnsureGraphics (e, false))
	{ failure = "radiance receiver pipelines unavailable"; goto fail; }
	if (profile) cpu_time[1] = Sys_DoubleTime ();
	float old_mins[3], old_maxs[3];
	uint64_t old_hash;
	bool old_bounds = RCSceneRuntime_DynamicBounds (&e->scene.runtime, old_mins, old_maxs, &old_hash);
	/* Collector tracks each selected alias pose/skin independently. */
	uint32_t occluder_count;
	if (!RCFrame_AssembleOccluders (e, &occluder_count))
	{ failure = "radiance occluder candidate allocation failed"; goto fail; }
	if (!RCCollect_PrepareScene (&e->scene, e->occluder_candidates, occluder_count, r_origin, 0,
		emitters, countof (emitters), &emissive_stats))
	{ failure = RCCollect_LastError (&e->scene); if (!failure) failure = "emissive scene preparation failed"; goto fail; }
    if (e->scene.stats.base_content_changed) e->lights.surface_static_valid=false;
    if (!RadianceCoordinator_PrepareSurfaceLights (&e->lights, &e->scene.runtime.scene,
		RCSceneRuntime_BaseTriangleCount(&e->scene.runtime), &e->scene.materials, e->scene.geometry_generation))
	{ failure = "surface light preparation failed"; goto fail; }
	if (profile) cpu_time[2] = Sys_DoubleTime ();
	if (!RadianceCoordinator_SetGains (&e->lights, r_radiance_point_gain.value, r_radiance_emissive_gain.value))
	{ failure = "invalid radiance light gains"; goto fail; }
	if (!RadianceCoordinator_SetSpotSoftness (&e->lights,
		isfinite (r_radiance_spot_softness.value) ? r_radiance_spot_softness.value : 0.5f))
	{ failure = "invalid radiance spotlight softness"; goto fail; }
	RadianceCoordinator_SetCooperative (&e->lights, cl.maxclients > 1 && cl.gametype == GAME_COOP);
	RadianceCoordinator_SetViewOrigin (&e->lights, r_refdef.vieworg);
	if (!RadianceCoordinator_PrepareLightsWithEmitters (&e->lights, d_lightstylevalue, MAX_LIGHTSTYLES, cl_dlights, MAX_DLIGHTS,
		cl.time, &grid, emitters, emissive_stats.emitted_lights, &sources))
	{
		/* Dense/unbounded-light maps may exceed the index or allocation limit.
		 * Use the prior conservative grid for this map without retrying the
		 * expensive failed build every frame. */
		if (!fine_grid) { failure = "runtime light snapshot failed"; goto fail; }
		e->fine_grid_disabled = true;
		fine_grid = false; grid = coarse_grid;
		if (!RadianceCoordinator_PrepareLightsWithEmitters (&e->lights, d_lightstylevalue, MAX_LIGHTSTYLES, cl_dlights, MAX_DLIGHTS,
			cl.time, &grid, emitters, emissive_stats.emitted_lights, &sources))
		{ failure = "runtime light snapshot failed"; goto fail; }
	}
	float sky_color[3];
	bool fallback_sky = Sky_GetRadianceColor (sky_color) &&
		Radiance_ApplySkyFallback (&e->lights.map_stats, e->sky_portal, sky_color,
			r_radiance_sky_fallback.value, sources.parameters.sky_radiance);
	if (fallback_sky) sources.parameters.counts[3] |= 16u;
	sources.parameters.receiver_filter[1] = r_radiance_checkerboard.value > 0 && R_RadianceResolutionDivisor () > 1 ? 1.0f : 0.0f;
	sources.parameters.receiver_filter[2] = r_radiance_receiver_filter.value > 0 ? 1.0f : 0.0f;
	sources.parameters.receiver_filter[3] = R_RadianceShadowFilterRadius ();
	sources.parameters.receiver_filter[0] = isfinite (r_radiance_probe_gaussian.value) ? CLAMP (0.0f, r_radiance_probe_gaussian.value, 2.0f) : 0.0f;
	/* Receiver controls do not alter source history or probe transport. */
	R_RadianceShadowParameters (sources.parameters.shadow_parameters);
	sources.parameters.counts[3] |= 8u;
	if (vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT) sources.parameters.counts[3] |= 32u;
	if (r_radiance_debug_fallback.value > 0) sources.parameters.counts[3] |= 64u;
	/* Most scenes have no local-minimum lights, so the filters can skip that field. */
	for (size_t i = 0; i < sources.light_count; ++i)
		if ((sources.lights[i].metadata[2] & 17u) == 1u && (sources.lights[i].metadata[3] & 7u) == 4u)
		{ sources.parameters.counts[3] |= 128u; break; }
	const uint32_t quality = isfinite (r_radiance_quality.value) ? (uint32_t)CLAMP (0.0f, r_radiance_quality.value, 2.0f) : 1u;
	if (r_fullbright_cheatsafe || (r_lightmap_cheatsafe && r_fullbright.value)) sources.parameters.counts[3] |= 2u;
	sources.parameters.bounce_parameters[0] = isfinite (r_radiance_bounce_gain.value) ? q_max (0.0f, r_radiance_bounce_gain.value) : 1.0f;
	sources.parameters.bounce_parameters[1] = quality == 0 ? 1.0f : 0.0f;
	/* Receiver highlight shoulder follows the postprocess contrast setting. */
	sources.parameters.bounce_parameters[2] = isfinite (vid_contrast.value) ? CLAMP (1.0f, vid_contrast.value, 2.0f) : 1.0f;
	sources.parameters.bounce_parameters[3] = vulkan_globals.scene_hdr_active ? 2.0f : r_radiance_highlight.value > 0 ? 1.0f : 0.0f;
	for (int axis = 0; axis < 3; ++axis) sources.parameters.gi_camera_fade[axis] = r_refdef.vieworg[axis];
	sources.parameters.gi_camera_fade[3] = isfinite (r_radiance_gi_distance.value) ? q_max (0.0f, r_radiance_gi_distance.value) : 0.0f;
	sources.parameters.gi_parameters[0] = isfinite (r_radiance_gi_normal_bias.value) ? CLAMP (0.0f, r_radiance_gi_normal_bias.value, 1.0f) : 0.0f;
	sources.parameters.gi_parameters[1] = isfinite (r_radiance_water_shadow_scale.value) ? CLAMP (1.0f, r_radiance_water_shadow_scale.value, 16.0f) : 1.0f;
	/* Receiver tokens identify liquid texels; floats hold these values exactly. */
	sources.parameters.gi_parameters[2] = (float)(1 + (NUM_WORLD_CBX + NUM_ENTITIES_CBX) * RC_EXPERIMENTAL_CAPTURE_CAPACITY);
	sources.parameters.gi_parameters[3] = sources.parameters.gi_parameters[2] + (float)(RC_EXPERIMENTAL_CAPTURE_CAPACITY - 1);
	if (profile) cpu_time[3] = Sys_DoubleTime ();
	entity_t *batch_entities = NULL;
	uint32_t batch_count = CL_ECSVisualBatch (&batch_entities);
	rc_scene_rt_batch_t batch_snapshot = {0};
	const rc_scene_rt_batch_t *shadow_batch = NULL;
	if (r_radiance_batch_shadows.value > 0 && want_hardware && e->scene.rt_available && batch_count &&
		RCScene_ECSBatchPrepare (&e->ecs_batch, batch_entities, batch_count, &batch_snapshot))
		shadow_batch = &batch_snapshot;
	if (!RCCollect_RecordPreparedBatch (&e->scene, slot, fence, commands, &sources, want_hardware, shadow_batch, &scene))
	{ failure = RCCollect_LastError (&e->scene); if (!failure) failure = "scene collection or upload failed"; goto fail; }
	if (!scene.hardware && !RCFrame_EnsureGraphics (e, false))
	{ failure = "software radiance receiver pipelines unavailable"; goto fail; }
	if (profile) cpu_time[4] = Sys_DoubleTime ();
	if (!RCFrame_MarkReceivers (e)) { failure = "sparse receiver selection failed"; goto fail; }
	if (!RCFrame_RefreshChanges (e, &sources, &scene, old_mins, old_maxs, old_bounds, old_hash))
	{ failure = "radiance change tracking failed"; goto fail; }
	if (!RCVk_UpdateSparse (&e->cascades, slot, e->sparse)) { failure = "sparse GPU snapshot failed"; goto fail; }
	static const uint32_t quality_maximum[] = {4096, 16384, 32768};
	e->budget.maximum = quality_maximum[quality];
	if (e->budget.current > e->budget.maximum) e->budget.current = e->budget.maximum;
	uint32_t level, first, count;
	e->refresh_reserved[slot] = false;
	if ((e->frame_serial & 1u) && RC_SparseRefreshNext (e->sparse, e->budget.current, &e->refresh[slot]))
	{
		e->refresh_reserved[slot] = true;
		level = e->refresh[slot].level; first = e->refresh[slot].first; count = e->refresh[slot].count;
	}
	else if (!RC_SparseSchedulerNext (&e->scheduler, e->sparse, e->budget.current, &level, &first, &count))
	{ failure = "sparse cascade scheduler found no work"; goto fail; }
	if (profile) cpu_time[5] = Sys_DoubleTime ();
	if (!RCVk_Record (&e->cascades, commands, slot, scene.scene, scene.lighting, scene.hardware, level, first, count))
	{ failure = "radiance dispatch recording failed"; goto fail; }
	if (!RCReceiverVk_Update (&e->receiver, slot, &e->cascades, &e->scene.scene_gpu, &e->scene.inputs,
		vulkan_globals.view_projection_matrix)) { failure = "receiver descriptors or camera matrix failed"; goto fail; }
	VkDescriptorSet descriptor = RCReceiverVk_Descriptor (&e->receiver, slot, scene.hardware);
	if (!descriptor || !RCGraphics_SetFrame (&e->graphics[scene.hardware ? 1 : 0], descriptor))
	{ failure = "receiver pipeline publication failed"; goto fail; }
	if (profile) cpu_time[6] = Sys_DoubleTime ();
	if (e->queries) vkCmdWriteTimestamp (commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, e->queries, slot * 2 + 1);
	e->pending[slot] = true; e->commands[slot] = commands;
	++e->frame_serial;
	radiance_runtime_stats_t stats = {.active = true, .timing_available = e->timing_available,
		.atlas_revision = e->scene.atlas_revision, .atlas_repacks = e->scene.atlas_repacks, .atlas_repack_ms = e->scene.atlas_repack_ms,
		.atlas_layers = e->scene.atlas.layers, .atlas_width = e->scene.atlas.width, .atlas_height = e->scene.atlas.height, .atlas_bytes = e->scene.atlas.bytes,
		.sky_fallback = fallback_sky, .sky_authored = e->lights.map_stats.sky_authored,
		.backend = scene.hardware ? RADIANCE_BACKEND_HARDWARE : RADIANCE_BACKEND_SOFTWARE,
		.probes = RC_SparseLayout (e->sparse)->probe_count, .intervals_updated = count,
		.probe_spacing = RC_SparseLayout (e->sparse)->levels[0].origin_spacing[3],
		.source_count = (uint32_t)sources.light_count, .emissive_count = (uint32_t)emissive_stats.emitted_lights,
		.omitted_emitters = (uint32_t)emissive_stats.omitted_lights, .dynamic_occluders = scene.stats.selected,
		.occluders_considered = scene.stats.considered, .occluders_limit = scene.stats.omitted_limit,
		.occluders_unsupported = scene.stats.omitted_unsupported, .occluders_transparent = scene.stats.omitted_transparent,
		.occluders_ecs_batch = CL_ECSVisualBatch (NULL),
		.gpu_ms = e->gpu_ms, .input_gpu_bytes = RadianceInputs_AllocationBytes (&e->scene.inputs)};
	entity_t *ecs_entities = NULL;
	uint32_t ecs_count = CL_ECSVisualBatch (&ecs_entities);
	stats.ecs_gi_selected = RCSceneRuntime_CountBatchSelected (&e->scene.runtime, ecs_entities, ecs_count);
	stats.ecs_gi_omitted = ecs_count - stats.ecs_gi_selected;
	stats.ecs_batch_shadow_active = scene.ecs_shadow_count > 0;
	stats.ecs_local_shadow_selected = scene.ecs_shadow_count ? scene.ecs_shadow_count : stats.ecs_gi_selected;
	stats.ecs_local_shadow_omitted = ecs_count - stats.ecs_local_shadow_selected;
	memcpy (stats.sky_radiance, sources.parameters.sky_radiance, sizeof (stats.sky_radiance));
	stats.omitted_occluders = (uint64_t)stats.occluders_limit + stats.occluders_unsupported + stats.occluders_transparent;
	stats.surface_templates = e->lights.surface_template_count;
	stats.surface_static = e->lights.surface_static_count;
	stats.surface_dynamic = e->lights.surface_dynamic_count;
	stats.surface_unmatched = e->lights.surface_unmatched_count;
	stats.scene_upload_bytes = e->scene.scene_gpu.last_upload_bytes[slot];
	if (profile)
	{
		stats.cpu_profile_available = true;
		stats.cpu_prepare_ms = (cpu_time[6] - cpu_time[0]) * 1000.0;
		stats.cpu_pipeline_ms = (cpu_time[1] - cpu_time[0]) * 1000.0;
		stats.cpu_scene_ms = (cpu_time[2] - cpu_time[1]) * 1000.0;
		stats.cpu_lights_ms = (cpu_time[3] - cpu_time[2]) * 1000.0;
		stats.cpu_upload_ms = (cpu_time[4] - cpu_time[3]) * 1000.0;
		stats.cpu_sparse_ms = (cpu_time[5] - cpu_time[4]) * 1000.0;
		stats.cpu_dispatch_ms = (cpu_time[6] - cpu_time[5]) * 1000.0;
	}
	stats.interval_budget = e->budget.current;
	stats.updated_first = first;
	stats.refresh_work = e->refresh_reserved[slot];
	stats.refresh_slot = e->refresh_reserved[slot] ? e->refresh[slot].slot : 0;
	stats.dynamic_cached = scene.stats.cached;
	stats.dynamic_updated = scene.stats.updated;
	stats.dynamic_deferred = scene.stats.deferred;
	stats.alias_update_budget = e->scene.runtime.alias_budget;
	stats.updated_level = level;
	stats.grid_fine = fine_grid;
	stats.grid_cell_size = grid.cell_size[0];
	stats.grid_cells = (uint32_t)(sources.grid_offset_count - 1);
	stats.grid_indices = (uint32_t)sources.grid_index_count;
	uint32_t camera_cell[3];
	bool camera_in_grid = true;
	for (int axis = 0; axis < 3; ++axis)
	{
		double coord = floor (((double)r_origin[axis] - grid.origin[axis]) / grid.cell_size[axis]);
		if (!isfinite (coord) || coord < 0 || coord >= grid.dimensions[axis]) { camera_in_grid = false; break; }
		camera_cell[axis] = (uint32_t)coord;
	}
	if (camera_in_grid)
	{
		const size_t cell = ((size_t)camera_cell[2] * grid.dimensions[1] + camera_cell[1]) * grid.dimensions[0] + camera_cell[0];
		stats.grid_camera_candidates = sources.grid_offsets[cell + 1] - sources.grid_offsets[cell];
	}
	stats.static_blas_upload_bytes = e->scene.rt.frame[slot].static_upload_bytes;
	stats.dynamic_blas_upload_bytes = e->scene.rt.frame[slot].dynamic_upload_bytes;
	stats.static_blas_builds = e->scene.rt.frame[slot].static_builds;
	stats.dynamic_blas_builds = e->scene.rt.frame[slot].dynamic_builds;
	stats.tlas_builds = e->scene.rt.frame[slot].tlas_builds;
	stats.dynamic_blas_updates = e->scene.rt.frame[slot].dynamic_updates;
	stats.tlas_updates = e->scene.rt.frame[slot].tlas_updates;
	stats.light_grid_builds = e->lights.grid_builds;
	for (unsigned i = 0; i < 6; ++i) stats.cascade_gpu_bytes += e->cascades.records[i].allocation_size;
	for (unsigned i = 0; i < e->cascades.frames; ++i)
	{
		stats.cascade_gpu_bytes += e->cascades.metadata[i].allocation_size + e->cascades.brick_table[i].allocation_size +
			e->cascades.active_bricks[i].allocation_size + e->receiver.inverse_view_projection[i].allocation_size;
	}
	for (unsigned i = 0; i < e->scene.scene_gpu.frames; ++i)
		for (unsigned j = 0; j < 4; ++j) stats.scene_gpu_bytes += e->scene.scene_gpu.buffers[i][j].allocation_size;
	for (unsigned i = 0; i < e->scene.rt.frames; ++i)
	{
		const rc_scene_rt_frame_t *f = &e->scene.rt.frame[i];
		stats.acceleration_gpu_bytes += f->vertices.allocation_size + f->instance.allocation_size + f->blas_memory.allocation_size +
			f->tlas_memory.allocation_size + f->scratch.allocation_size + f->static_vertices.allocation_size + f->static_memory.allocation_size +
			f->batch_vertices.allocation_size + f->batch_memory.allocation_size;
	}
	stats.allocation_bytes = stats.input_gpu_bytes + stats.scene_gpu_bytes + stats.cascade_gpu_bytes + stats.acceleration_gpu_bytes;
	for (unsigned i = 0; i < RC_CASCADE_COUNT; ++i)
	{
		uint32_t base, active, per, overflow;
		if (RC_SparseLevelWork (e->sparse, i, &base, &active, &per, &overflow)) stats.sparse_overflow += overflow;
	}
	R_RadianceSetStats (&stats, NULL);
	return true;
fail:
	RCFrame_ClearPublication (failure);
	return false;
}

void R_RadianceFrameSubmitted (uint32_t slot, VkCommandBuffer commands)
{
	radiance_engine_t *e = &rc_engine;
	if (slot >= DOUBLE_BUFFERED || !e->pending[slot] || e->commands[slot] != commands) return;
	RCCollect_Submitted (&e->scene, slot);
	RCVk_MarkSubmitted (&e->cascades, commands);
	if (e->refresh_reserved[slot]) RC_SparseRefreshCommit (e->sparse, &e->refresh[slot], true);
	e->refresh_reserved[slot] = false;
	e->timestamps_written[slot] = e->queries != VK_NULL_HANDLE;
	e->pending[slot] = e->attempted[slot] = false; e->commands[slot] = VK_NULL_HANDLE;
}

void R_RadianceFrameDiscard (uint32_t slot, VkCommandBuffer commands)
{
	radiance_engine_t *e = &rc_engine;
	if (slot >= DOUBLE_BUFFERED) return;
	RCCollect_Discard (&e->scene, slot);
	RCVk_DiscardRecorded (&e->cascades, commands);
	if (e->refresh_reserved[slot]) RC_SparseRefreshCommit (e->sparse, &e->refresh[slot], false);
	e->refresh_reserved[slot] = false;
	/* Sparse slots may have changed even if recording failed before dispatch.
	 * A full queued clear prevents reused slots retaining discarded history. */
	if (e->initialized && e->attempted[slot]) { e->cascades.needs_clear = true; RC_SchedulerReset (&e->scheduler); }
	e->pending[slot] = e->attempted[slot] = false; e->commands[slot] = VK_NULL_HANDLE;
	RCGraphics_SetFrame (NULL, VK_NULL_HANDLE);
}
