/* Opt-in host-side source collection for radiance inputs. Include quakedef.h
 * first. This module does not hook the renderer or own the Vulkan device. */
#ifndef QUIVER_RADIANCE_COORDINATOR_H
#define QUIVER_RADIANCE_COORDINATOR_H
#include "radiance_gpu_inputs.h"
#include "radiance_materials_quake.h"
#include "radiance_scene.h"

typedef struct
{
	size_t max_map_lights, max_lights, max_grid_cells, max_grid_indices, max_atlas_bytes;
	uint32_t max_atlas_dimension, max_atlas_layers;
} radiance_coordinator_limits_t;

typedef struct
{
	radiance_gpu_inputs_t *gpu; /* externally owned, frame fences required */
	radiance_coordinator_limits_t limits;
	radiance_quake_materials_t materials;
	radiance_material_atlas_t atlas;
	radiance_light_parse_stats_t map_stats;
	radiance_light_t *map_lights, *frame_lights;
	radiance_surface_template_t *surface_templates;
	radiance_light_t *surface_static_lights, *surface_dynamic_lights;
	size_t surface_template_count, surface_static_count, surface_dynamic_count;
	size_t surface_unmatched_count;
	uint8_t *surface_static_matches, *surface_dynamic_matches;
	uint64_t surface_geometry_generation;
	bool surface_static_valid, surface_dynamic_valid;
	uint32_t *grid_offsets, *grid_indices;
	void *grid_keys;
	radiance_light_grid_t cached_grid;
	size_t map_count, frame_count, index_capacity, offset_capacity;
	size_t cached_key_count, cached_index_count;
	uint64_t grid_builds;
	uint64_t grid_revision;
	uint64_t atlas_revision;
	float point_gain, emissive_gain, spot_softness;
	bool map_loaded, atlas_valid, grid_valid, cooperative, view_origin_valid;
	float view_origin[3];
} radiance_coordinator_t;

/* Init/Clear and all calls are serial with texture/map destruction. Clearing
 * CPU state does not wait on GPU; wait for device/frame fences separately. */
bool RadianceCoordinator_Init (radiance_coordinator_t *coordinator, radiance_gpu_inputs_t *gpu,
	const radiance_coordinator_limits_t *limits);
/* CPU-only mode for RCCollect: the collector owns the material registry,
 * texture atlas and GPU input snapshots. Atlas limits are not needed here. */
bool RadianceCoordinator_InitLights (radiance_coordinator_t *coordinator,
	const radiance_coordinator_limits_t *limits);
/* Independent category gains. Zero disables that category; positive finite
 * values preserve the incoming source color and attenuation. */
bool RadianceCoordinator_SetGains (radiance_coordinator_t *coordinator, float point_gain, float emissive_gain);
/* 0 preserves compiler hard-cone behavior; 1 uses the widest soft ramp. */
bool RadianceCoordinator_SetSpotSoftness (radiance_coordinator_t *coordinator, float softness);
/* KEX dynamiclight spawnflags depend on the current client game mode. */
void RadianceCoordinator_SetCooperative (radiance_coordinator_t *coordinator, bool cooperative);
/* A missing/invalid view preserves map lights at their authored strength. */
void RadianceCoordinator_SetViewOrigin (radiance_coordinator_t *coordinator, const float origin[3]);
void RadianceCoordinator_Clear (radiance_coordinator_t *coordinator);
/* Invalid map, allocation failure, or truncation leaves this map unloaded.
 * Use the active worldmodel->entities, including loaded external .ent files. */
bool RadianceCoordinator_LoadMap (radiance_coordinator_t *coordinator, const char *entities, size_t length);
/* Rebuild explicit compiler surface-light instances from brush geometry.
 * static_count separates stable world triangles from transformed entities. */
bool RadianceCoordinator_PrepareSurfaceLights (radiance_coordinator_t *coordinator,
	const rc_scene_t *scene, uint32_t static_count, const radiance_quake_materials_t *materials,
	uint64_t geometry_generation);
/* Supply &coordinator->materials as the user data for the scene's material
 * callbacks. Register all scene materials before Prepare, including current
 * alias animation skins and brush frame. Material IDs live until next LoadMap. */

/* Call only after waiting for the GPU fence for frame. Lightstyles are 8.8;
 * dynamic light slots are scanned without touching gameplay state. The grid
 * covers camera/cascade influence; shaders fall back to full evaluation outside
 * it. False requires a classic-renderer fallback. Prepare copies into that
 * frame's GPU buffers; then call RadianceInputs_RecordUpload and resolve via
 * MarkSubmitted/DiscardRecorded, with normal queue synchronization. */
bool RadianceCoordinator_Prepare (radiance_coordinator_t *coordinator, uint32_t frame,
	const int *styles, size_t style_count, const dlight_t *dynamic_lights, size_t dynamic_count,
	double time, const radiance_light_grid_t *grid);

/* Build the source/light-grid portion of a collector snapshot. Its arrays
 * belong to the coordinator and are valid only until the next Prepare,
 * PrepareLights, LoadMap or Clear call. It contains no materials or atlas:
 * RCCollect_Record fills them from the same registry used to build its scene.
 * False leaves the returned snapshot zeroed and must trigger fallback. */
bool RadianceCoordinator_PrepareLights (radiance_coordinator_t *coordinator,
	const int *styles, size_t style_count, const dlight_t *dynamic_lights, size_t dynamic_count,
	double time, const radiance_light_grid_t *grid, radiance_input_snapshot_t *snapshot);
/* As above, but adds collector-generated emissive surface lights before
 * building the conservative grid. Emitters are copied during this call. */
bool RadianceCoordinator_PrepareLightsWithEmitters (radiance_coordinator_t *coordinator,
	const int *styles, size_t style_count, const dlight_t *dynamic_lights, size_t dynamic_count,
	double time, const radiance_light_grid_t *grid, const radiance_light_t *emitters, size_t emitter_count,
	radiance_input_snapshot_t *snapshot);
#endif
