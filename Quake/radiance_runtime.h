/* User-facing mode and truthful renderer readiness for radiance cascades. */
#ifndef QUIVER_RADIANCE_RUNTIME_H
#define QUIVER_RADIANCE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "radiance_receiver_diagnostics.h"

/* Include quakedef.h before this header for cvar_t. */
extern cvar_t r_radiance;
extern cvar_t r_radiance_backend;
extern cvar_t r_radiance_quality;
extern cvar_t r_radiance_point_gain;
extern cvar_t r_radiance_emissive_gain;
extern cvar_t r_radiance_bounce_gain;
extern cvar_t r_radiance_sky_fallback;
extern cvar_t r_radiance_probe_gaussian;
extern cvar_t r_radiance_probe_spacing;
extern cvar_t r_radiance_gi_distance;
extern cvar_t r_radiance_spot_softness;
extern cvar_t r_radiance_soft_shadows;
extern cvar_t r_radiance_shadow_radius;
extern cvar_t r_radiance_shadow_sun_angle;
extern cvar_t r_radiance_shadow_filter;
extern cvar_t r_radiance_grid;
extern cvar_t r_radiance_highlight;
extern cvar_t r_radiance_profile;
extern cvar_t r_radiance_batch_shadows;
extern cvar_t r_radiance_resolution_scale;
extern cvar_t r_radiance_half_resolution;
extern cvar_t r_radiance_checkerboard;
extern cvar_t r_radiance_receiver_filter;
extern cvar_t r_radiance_receiver_diagnostics;
extern cvar_t r_radiance_deferred;

typedef enum
{
	RADIANCE_BACKEND_NONE,
	RADIANCE_BACKEND_SOFTWARE,
	RADIANCE_BACKEND_HARDWARE
} radiance_backend_t;

typedef struct radiance_runtime_stats_s
{
	bool active, timing_available, sky_fallback, sky_authored, ecs_batch_shadow_active;
	radiance_backend_t backend;
	uint64_t allocation_bytes, probes, intervals_updated;
	uint64_t input_gpu_bytes, scene_gpu_bytes, cascade_gpu_bytes, acceleration_gpu_bytes;
	uint64_t scene_upload_bytes, static_blas_upload_bytes, dynamic_blas_upload_bytes, light_grid_builds;
	uint32_t static_blas_builds, dynamic_blas_builds, tlas_builds, dynamic_blas_updates, tlas_updates;
	uint32_t interval_budget, updated_level;
	uint32_t updated_first, refresh_slot;
	bool refresh_work;
	uint32_t grid_cells, grid_indices, grid_camera_candidates;
	float grid_cell_size, probe_spacing;
	float sky_radiance[4];
	bool grid_fine;
	uint32_t source_count, emissive_count, omitted_emitters, dynamic_occluders, sparse_overflow;
	uint64_t surface_templates, surface_static, surface_dynamic, surface_unmatched;
	uint32_t occluders_considered, occluders_limit, occluders_unsupported, occluders_transparent, occluders_ecs_batch;
	uint32_t ecs_gi_selected, ecs_gi_omitted, ecs_local_shadow_selected, ecs_local_shadow_omitted;
	uint64_t omitted_occluders;
	uint32_t dynamic_cached, dynamic_updated, dynamic_deferred, alias_update_budget;
	double gpu_ms;
	bool cpu_profile_available;
	double cpu_prepare_ms, cpu_pipeline_ms, cpu_scene_ms, cpu_lights_ms, cpu_upload_ms, cpu_sparse_ms, cpu_dispatch_ms;
} radiance_runtime_stats_t;

void R_RadianceInit (void);
void R_RadianceFrameNewMap (void);
/* Renderer must publish active only after every required pipeline, scene,
 * descriptor and synchronization check has succeeded for the frame. */
void R_RadianceSetStats (const radiance_runtime_stats_t *stats, const char *reason);
/* Reciprocal per-axis scale: 1, 1.5 (two thirds), 2, 3, or 4. */
float R_RadianceResolutionDivisor (void);
/* Sanitized receiver-only shadow controls: enabled, finite-source radius,
 * directional angular radius in radians, spatial filter strength [0,2]. */
void R_RadianceShadowParameters (float output[4]);
/* Update publication without counting a frame (for lifecycle resets). */
void R_RadianceSetReceiverUse (float divisor, bool checkerboard, bool texture_filter);
/* Update the last recorded optional-pass state without counting a frame. */
void R_RadianceSetReceiverSoftUse (bool active);
/* Record actual optional post work, including unsupported/skipped frames. */
void R_RadianceRecordReceiverSoftUse (bool active);
void R_RadianceSetReceiverShadowFilterUse (float strength);
void R_RadianceRecordReceiverShadowFilterUse (float strength);
float R_RadianceActiveShadowFilter (void);
/* Record one frame's selected world path, including full-resolution fallback. */
void R_RadianceRecordReceiverUse (float divisor, bool checkerboard, bool texture_filter);
void R_RadianceRecordModelReceiverUse (uint32_t draws, uint32_t triangles, bool reused);
void R_RadianceSetReceiverDiagnostics (const rc_receiver_diagnostic_stats_t *stats);
float R_RadianceActiveProbeSpacing (void);
void R_RadianceReset (const char *reason);
bool R_RadianceActive (void);
/* Actual prepared scene counts: total, immutable world, persistent base,
 * compiled placements. Returns false (and zeros) when no active scene exists. */
bool R_RadianceGeometryCounts (uint32_t counts[4]);
const char *R_RadianceMenuStatus (void);

#endif
