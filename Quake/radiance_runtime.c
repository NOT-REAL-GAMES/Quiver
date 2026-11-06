#include "quakedef.h"
#include "radiance_runtime.h"
#include "radiance_cascades.h"
#include <math.h>

/* Keep classic lighting as the default until the renderer and performance
 * acceptance checks have passed. */
cvar_t r_radiance = {"r_radiance", "0", CVAR_ARCHIVE};
/* 0: automatic, 1: software BVH, 2: hardware ray query. */
cvar_t r_radiance_backend = {"r_radiance_backend", "0", CVAR_ARCHIVE};
cvar_t r_radiance_quality = {"r_radiance_quality", "0", CVAR_ARCHIVE};
cvar_t r_radiance_point_gain = {"r_radiance_point_gain", "1", CVAR_ARCHIVE};
cvar_t r_radiance_emissive_gain = {"r_radiance_emissive_gain", "1", CVAR_ARCHIVE};
cvar_t r_radiance_bounce_gain = {"r_radiance_bounce_gain", "1", CVAR_ARCHIVE};
cvar_t r_radiance_sky_fallback = {"r_radiance_sky_fallback", "0.15", CVAR_ARCHIVE};
cvar_t r_radiance_probe_gaussian = {"r_radiance_probe_gaussian", "0", CVAR_ARCHIVE};
/* Applied at a synchronized frame boundary; no video restart required. */
cvar_t r_radiance_probe_spacing = {"r_radiance_probe_spacing", "32", CVAR_ARCHIVE};
cvar_t r_radiance_gi_distance = {"r_radiance_gi_distance", "0", CVAR_ARCHIVE};
cvar_t r_radiance_spot_softness = {"r_radiance_spot_softness", "0.5", CVAR_ARCHIVE};
cvar_t r_radiance_soft_shadows = {"r_radiance_soft_shadows", "0", CVAR_ARCHIVE};
cvar_t r_radiance_shadow_radius = {"r_radiance_shadow_radius", "2", CVAR_ARCHIVE};
/* Angular radius in degrees, rather than angular diameter. */
cvar_t r_radiance_shadow_sun_angle = {"r_radiance_shadow_sun_angle", "0.5", CVAR_ARCHIVE};
/* Spatial visibility smoothing; zero keeps the unfiltered soft-shadow result. */
cvar_t r_radiance_shadow_filter = {"r_radiance_shadow_filter", "1", CVAR_ARCHIVE};
/* Non-persistent override: 0 camera grid, 1 map grid. */
cvar_t r_radiance_grid = {"r_radiance_grid", "1"};
cvar_t r_radiance_highlight = {"r_radiance_highlight", "1", CVAR_ARCHIVE};
cvar_t r_radiance_profile = {"r_radiance_profile", "0"};
/* Experimental hardware-only local shadows for eligible ECS instance batches. */
cvar_t r_radiance_batch_shadows = {"r_radiance_batch_shadows", "0"};
/* Reduced-resolution receiver lighting is opt-in while being validated. */
cvar_t r_radiance_resolution_scale = {"r_radiance_resolution_scale", "1", CVAR_ARCHIVE};
/* Compatibility alias for older configs; the scale is the persisted setting. */
cvar_t r_radiance_half_resolution = {"r_radiance_half_resolution", "0"};
cvar_t r_radiance_checkerboard = {"r_radiance_checkerboard", "0", CVAR_ARCHIVE};
/* 0: reference probe filtering; 1: depth-guided indirect texture filtering. */
cvar_t r_radiance_receiver_filter = {"r_radiance_receiver_filter", "1", CVAR_ARCHIVE};
cvar_t r_radiance_receiver_diagnostics = {"r_radiance_receiver_diagnostics", "0"};
/* Shared opaque model/world lighting, including the full-resolution grid.
 * Zero retains the original reduced-world/forward-model reference path. */
cvar_t r_radiance_deferred = {"r_radiance_deferred", "0", CVAR_ARCHIVE};

void R_RadianceShadowParameters (float output[4])
{
	output[0] = isfinite (r_radiance_soft_shadows.value) && r_radiance_soft_shadows.value > 0 ? 1.0f : 0.0f;
	output[1] = isfinite (r_radiance_shadow_radius.value) ? fminf (fmaxf (r_radiance_shadow_radius.value, 0.0f), 64.0f) : 0.0f;
	const float degrees = isfinite (r_radiance_shadow_sun_angle.value) ? fminf (fmaxf (r_radiance_shadow_sun_angle.value, 0.0f), 15.0f) : 0.0f;
	output[2] = degrees * 0.01745329251994329577f;
	output[3] = isfinite (r_radiance_shadow_filter.value) ? fminf (fmaxf (r_radiance_shadow_filter.value, 0.0f), 2.0f) : 0.0f;
}

/* Quantize to the nearest supported per-axis scale. Invalid values use full
 * resolution; smaller positive requests clamp to quarter resolution. */
float R_RadianceResolutionDivisor (void)
{
	const float scale = r_radiance_resolution_scale.value;
	if (!isfinite (scale) || scale <= 0 || scale >= (1.0f + 2.0f / 3.0f) * 0.5f) return 1;
	if (scale >= (2.0f / 3.0f + 0.5f) * 0.5f) return 1.5f;
	if (scale >= (0.5f + 1.0f / 3.0f) * 0.5f) return 2;
	return scale >= (1.0f / 3.0f + 0.25f) * 0.5f ? 3 : 4;
}

static bool radiance_resolution_sync;
static void R_RadianceProbeSpacingChanged (cvar_t *var)
{
	const float spacing = RC_ReceiverSpacing (var->value);
	if (!isfinite (var->value) || spacing != var->value)
		Con_Printf ("r_radiance_probe_spacing: using %.6g world units (range 8 to 128; invalid/nonpositive values use 32)\n", spacing);
}

static void R_RadianceResolutionChanged (cvar_t *var)
{
	if (radiance_resolution_sync) return;
	radiance_resolution_sync = true;
	if (var == &r_radiance_half_resolution)
	{
		const bool half = var->value > 0;
		Cvar_SetQuick (&r_radiance_resolution_scale, half ? "0.5" : "1");
		Cvar_SetQuick (&r_radiance_half_resolution, half ? "1" : "0");
	}
	else
	{
		const float divisor = R_RadianceResolutionDivisor ();
		const char *scale = divisor == 4 ? "0.25" : divisor == 3 ? "0.333333333" : divisor == 2 ? "0.5" : divisor == 1.5f ? "0.666666667" : "1";
		if (var->value != 1.0f / divisor)
			Con_Printf ("r_radiance_resolution_scale: using %s (supported: 0.25, 0.333333333, 0.5, 0.666666667, 1)\n", scale);
		Cvar_SetQuick (&r_radiance_resolution_scale, scale);
		/* Distinct mirror values ensure legacy writes of 0/1 still invoke
		 * the callback after another reduced-scale request. Cvar_SetQuick
		 * deliberately skips callbacks when the string has not changed. */
		Cvar_SetQuick (&r_radiance_half_resolution, divisor == 4 ? "3" : divisor == 3 ? "2" : divisor == 2 ? "1" : divisor == 1.5f ? "4" : "0");
	}
	radiance_resolution_sync = false;
}

static radiance_runtime_stats_t radiance_stats;
/* Receiver passes may be recorded on a render worker. */
static atomic_uint32_t radiance_receiver_use;
static atomic_uint32_t radiance_receiver_soft_use;
/* Monotonic world-path selections let timing tools detect even a single
 * full-resolution fallback between their state snapshots. Unsigned wrap is
 * intentional; no diagnostic GPU pass or per-frame console output is needed. */
static atomic_uint32_t radiance_receiver_reduced_frames;
static atomic_uint32_t radiance_receiver_full_frames;
static atomic_uint32_t radiance_soft_active_frames;
static atomic_uint32_t radiance_soft_inactive_frames;
static atomic_uint32_t radiance_shadow_filter_use;
static atomic_uint32_t radiance_shadow_filter_active_frames, radiance_shadow_filter_inactive_frames;
static atomic_uint32_t radiance_model_draws, radiance_model_triangles, radiance_model_reused;
static atomic_uint32_t radiance_model_active_frames, radiance_model_fallback_frames;
static rc_receiver_diagnostic_stats_t radiance_receiver_diagnostics;
static bool radiance_receiver_diagnostics_valid;
static char radiance_reason[128] = "renderer resources unavailable";

void R_RadianceSetStats (const radiance_runtime_stats_t *stats, const char *reason)
{
	if (stats) radiance_stats = *stats;
	else memset (&radiance_stats, 0, sizeof (radiance_stats));
	q_strlcpy (radiance_reason, reason ? reason : "", sizeof (radiance_reason));
}

/* Private atomic encoding keeps the flags separate from the fractional
 * divisor. Code 5 means an actual divisor of 1.5, never a fifth-size grid. */
static uint32_t R_RadianceReceiverUseCode (float divisor)
{
	return divisor == 1.5f ? 5u : divisor == 2 ? 2u : divisor == 3 ? 3u : divisor == 4 ? 4u : 1u;
}

void R_RadianceSetReceiverUse (float divisor, bool checkerboard, bool texture_filter)
{
	const uint32_t code = R_RadianceReceiverUseCode (divisor);
	texture_filter &= divisor==1 || divisor==1.5f || divisor==2 || divisor==3 || divisor==4;
	Atomic_StoreUInt32 (&radiance_receiver_use, code | (code != 1 && checkerboard ? 8u : 0u) |
		(texture_filter ? 16u : 0u));
	if (!texture_filter) {
		R_RadianceSetReceiverDiagnostics (NULL);
		R_RadianceSetReceiverSoftUse (false);
	}
}

void R_RadianceSetReceiverSoftUse (bool active)
{
	Atomic_StoreUInt32 (&radiance_receiver_soft_use, active ? 1u : 0u);
	if (!active) R_RadianceSetReceiverShadowFilterUse (0);
}

void R_RadianceSetReceiverShadowFilterUse (float strength)
{
	strength = isfinite (strength) && strength > 0 ? fminf (strength, 2.0f) : 0;
	uint32_t bits;
	memcpy (&bits, &strength, sizeof (bits));
	Atomic_StoreUInt32 (&radiance_shadow_filter_use, bits);
}

float R_RadianceActiveShadowFilter (void)
{
	const uint32_t bits = Atomic_LoadUInt32 (&radiance_shadow_filter_use);
	float strength;
	memcpy (&strength, &bits, sizeof (strength));
	return strength;
}

void R_RadianceRecordReceiverShadowFilterUse (float strength)
{
	const bool active = isfinite (strength) && strength > 0;
	Atomic_IncrementUInt32 (active ? &radiance_shadow_filter_active_frames : &radiance_shadow_filter_inactive_frames);
	R_RadianceSetReceiverShadowFilterUse (strength);
}

void R_RadianceRecordReceiverSoftUse (bool active)
{
	Atomic_IncrementUInt32 (active ? &radiance_soft_active_frames : &radiance_soft_inactive_frames);
	R_RadianceSetReceiverSoftUse (active);
}

void R_RadianceRecordReceiverUse (float divisor, bool checkerboard, bool texture_filter)
{
	Atomic_IncrementUInt32 (R_RadianceReceiverUseCode (divisor) != 1 ? &radiance_receiver_reduced_frames : &radiance_receiver_full_frames);
	R_RadianceSetReceiverUse (divisor, checkerboard, texture_filter);
}

void R_RadianceRecordModelReceiverUse (uint32_t draws, uint32_t triangles, bool reused)
{
	Atomic_StoreUInt32 (&radiance_model_draws, draws);
	Atomic_StoreUInt32 (&radiance_model_triangles, triangles);
	Atomic_StoreUInt32 (&radiance_model_reused, reused ? 1u : 0u);
	Atomic_IncrementUInt32 (reused ? &radiance_model_active_frames : &radiance_model_fallback_frames);
}

void R_RadianceSetReceiverDiagnostics (const rc_receiver_diagnostic_stats_t *stats)
{
	if (stats) radiance_receiver_diagnostics = *stats;
	radiance_receiver_diagnostics_valid = stats != NULL;
}

float R_RadianceActiveProbeSpacing (void)
{
	return radiance_stats.probe_spacing;
}

void R_RadianceReset (const char *reason)
{
	R_RadianceSetStats (NULL, reason ? reason : "renderer resources unavailable");
	/* Frame preparation temporarily clears publication every frame. Keep the
	 * last recorded receiver/soft-pass selection and fence-completed coverage
	 * until actual world-path selection changes. Clearing here made settled
	 * soft-shadow frames intermittently report inactive between recordings. */
}

bool R_RadianceActive (void)
{
	return r_radiance.value > 0 && radiance_stats.active;
}

const char *R_RadianceMenuStatus (void)
{
	if (r_radiance.value <= 0) return "Off";
	return R_RadianceActive () ? "On" : "Unavailable";
}

static void R_RadianceStats_f (void)
{
	const uint32_t receiver_use = Atomic_LoadUInt32 (&radiance_receiver_use);
	const uint32_t receiver_code = receiver_use & 7u;
	const float receiver_divisor = receiver_code == 5 ? 1.5f : receiver_code >= 2 && receiver_code <= 4 ? (float)receiver_code : 1.0f;
	const char *backend = radiance_stats.backend == RADIANCE_BACKEND_SOFTWARE ? "software" :
		radiance_stats.backend == RADIANCE_BACKEND_HARDWARE ? "ray-query" : "none";
	Con_Printf ("Radiance: requested=%u active=%u backend=%s quality=%d\n",
		(unsigned)(r_radiance.value > 0), (unsigned)R_RadianceActive (), backend, (int)r_radiance_quality.value);
	Con_Printf ("Radiance: gains point=%.3g emissive=%.3g bounce=%.3g\n",
		r_radiance_point_gain.value, r_radiance_emissive_gain.value, r_radiance_bounce_gain.value);
	float shadows[4]; R_RadianceShadowParameters (shadows);
	Con_Printf ("Radiance: soft shadows requested=%u active=%u radius=%.3g sun_radius_deg=%.3g max_extra_rays=3\n",
		(unsigned)(r_radiance_soft_shadows.value > 0),
		(unsigned)(R_RadianceActive () && Atomic_LoadUInt32 (&radiance_receiver_soft_use) != 0),
		shadows[1], shadows[2] * 57.29577951308232088f);
	Con_Printf ("Radiance: soft shadow frames active=%u inactive=%u\n",
		Atomic_LoadUInt32 (&radiance_soft_active_frames), Atomic_LoadUInt32 (&radiance_soft_inactive_frames));
	Con_Printf ("Radiance: shadow filter requested=%.6g active=%.6g\n", shadows[3],
		R_RadianceActive () ? R_RadianceActiveShadowFilter () : 0);
	Con_Printf ("Radiance: shadow filter frames active=%u inactive=%u\n",
		Atomic_LoadUInt32 (&radiance_shadow_filter_active_frames), Atomic_LoadUInt32 (&radiance_shadow_filter_inactive_frames));
	const float effective_spacing = RC_ReceiverSpacing (r_radiance_probe_spacing.value);
	Con_Printf ("Radiance: probe spacing requested=%.6g active=%.6g effective=%.6g%s\n",
		r_radiance_probe_spacing.value, radiance_stats.probe_spacing, effective_spacing,
		radiance_stats.probe_spacing > 0 && radiance_stats.probe_spacing != effective_spacing ? " (not applied yet)" : "");
	Con_Printf ("Radiance: GI distance=%.3g (0 disables fade)\n", r_radiance_gi_distance.value);
	Con_Printf ("Radiance: sky=%s fallback_gain=%.3g upper_rgb=(%.3g %.3g %.3g) intensity=%.3g\n",
		radiance_stats.sky_fallback ? "sky texture" : radiance_stats.sky_authored ? "authored" : "none", r_radiance_sky_fallback.value,
		radiance_stats.sky_radiance[0], radiance_stats.sky_radiance[1], radiance_stats.sky_radiance[2], radiance_stats.sky_radiance[3]);
	Con_Printf ("Radiance: scene_target=%s hdr_requested=%u hdr_supported=%u highlight=%u spotlight_softness=%.3g\n",
		vulkan_globals.scene_hdr_active ? "RGBA16F" : "classic UNORM",
		(unsigned)(Cvar_VariableValue ("r_radiance_hdr") > 0), (unsigned)vulkan_globals.scene_hdr_supported,
		(unsigned)(r_radiance_highlight.value > 0), r_radiance_spot_softness.value);
	if (!isfinite (r_radiance_bounce_gain.value))
		Con_Printf ("Radiance: effective bounce gain=1 (invalid value)\n");
	else if (r_radiance_bounce_gain.value < 0)
		Con_Printf ("Radiance: effective bounce gain=0 (negative value)\n");
	if (!R_RadianceActive ())
		Con_Printf ("Radiance: classic lighting in use%s%s\n", radiance_reason[0] ? ": " : "", radiance_reason);
	Con_Printf ("Radiance: tracked_memory=%llu probe_capacity=%llu intervals_updated=%llu lights=%u emissive=%u emissive_omitted=%u dynamic=%u omitted=%llu overflow=%u\n",
		(unsigned long long)radiance_stats.allocation_bytes, (unsigned long long)radiance_stats.probes,
		(unsigned long long)radiance_stats.intervals_updated, radiance_stats.source_count, radiance_stats.emissive_count, radiance_stats.omitted_emitters,
		radiance_stats.dynamic_occluders, (unsigned long long)radiance_stats.omitted_occluders, radiance_stats.sparse_overflow);
	Con_Printf ("Radiance: surface templates=%llu generated static=%llu dynamic=%llu unmatched=%llu\n",
		(unsigned long long)radiance_stats.surface_templates, (unsigned long long)radiance_stats.surface_static,
		(unsigned long long)radiance_stats.surface_dynamic, (unsigned long long)radiance_stats.surface_unmatched);
	Con_Printf ("Radiance: occluder candidates=%u ECS_batch=%u omitted limit=%u unsupported=%u transparent=%u\n",
		radiance_stats.occluders_considered, radiance_stats.occluders_ecs_batch, radiance_stats.occluders_limit,
		radiance_stats.occluders_unsupported, radiance_stats.occluders_transparent);
	Con_Printf ("Radiance: ECS local-shadow selected=%u omitted=%u; GI/sun selected=%u omitted=%u\n",
		radiance_stats.ecs_local_shadow_selected, radiance_stats.ecs_local_shadow_omitted,
		radiance_stats.ecs_gi_selected, radiance_stats.ecs_gi_omitted);
	Con_Printf ("Radiance: experimental ECS batch shadows requested=%u active=%u (local shadows only)\n",
		(unsigned)(r_radiance_batch_shadows.value > 0), (unsigned)radiance_stats.ecs_batch_shadow_active);
	Con_Printf ("Radiance: receiver scale requested=%.6g active=%.6g divisor=%g\n",
		r_radiance_resolution_scale.value, 1.0 / receiver_divisor, receiver_divisor);
	Con_Printf ("Radiance: receiver filter requested=%s active=%s\n",
		r_radiance_receiver_filter.value > 0 ? "texture" : "probe",
		(receiver_use & 16u) ? "texture" : receiver_divisor <= 1 ? "full" : "probe");
	Con_Printf ("Radiance: deferred opaque requested=%u models_active=%u draws=%u triangles=%u frames_active=%u frames_fallback=%u\n",
		(unsigned)(r_radiance_deferred.value > 0), Atomic_LoadUInt32 (&radiance_model_reused),
		Atomic_LoadUInt32 (&radiance_model_draws), Atomic_LoadUInt32 (&radiance_model_triangles),
		Atomic_LoadUInt32 (&radiance_model_active_frames), Atomic_LoadUInt32 (&radiance_model_fallback_frames));
	Con_Printf ("Radiance: half-resolution receiver requested=%u active=%u checkerboard requested=%u active=%u\n",
		(unsigned)(r_radiance_half_resolution.value > 0), (unsigned)(receiver_divisor > 1),
		(unsigned)(r_radiance_checkerboard.value > 0), (unsigned)((receiver_use & 8u) != 0));
	Con_Printf ("Radiance: receiver frames reduced=%u full=%u\n",
		Atomic_LoadUInt32 (&radiance_receiver_reduced_frames), Atomic_LoadUInt32 (&radiance_receiver_full_frames));
	if (r_radiance_receiver_diagnostics.value > 0 && radiance_receiver_diagnostics_valid)
	{
		const rc_receiver_diagnostic_stats_t *d = &radiance_receiver_diagnostics;
		Con_Printf ("Radiance: estimated receiver coverage frame=%llu viewport=%ux%u divisor=%g output=%s\n",
			(unsigned long long)d->frame_serial, d->width, d->height, d->divisor, d->use_resolved ? "filtered" : "raw");
		Con_Printf ("Radiance: receiver blocks total=%u empty=%u reusable=%u mixed=%u raw_GI_valid=%u selected_GI_valid=%u\n",
			d->count[RC_DIAG_TOTAL_BLOCKS], d->count[RC_DIAG_EMPTY_BLOCKS], d->count[RC_DIAG_REUSABLE_BLOCKS],
			d->count[RC_DIAG_MIXED_BLOCKS], d->count[RC_DIAG_RAW_GI_VALID], d->count[RC_DIAG_SELECTED_GI_VALID]);
		Con_Printf ("Radiance: receiver pixels covered=%u GI_reuse=%u GI_fallback=%u direct_reuse=%u direct_fallback=%u invalid_geometry=%u\n",
			d->count[RC_DIAG_COVERED_PIXELS], d->count[RC_DIAG_GI_REUSE], d->count[RC_DIAG_GI_FALLBACK],
			d->count[RC_DIAG_DIRECT_REUSE], d->count[RC_DIAG_DIRECT_FALLBACK], d->count[RC_DIAG_INVALID_GEOMETRY]);
	}
	Con_Printf ("Radiance: GPU allocations inputs=%llu scene=%llu cascades=%llu acceleration=%llu B (driver overhead excluded)\n",
		(unsigned long long)radiance_stats.input_gpu_bytes, (unsigned long long)radiance_stats.scene_gpu_bytes,
		(unsigned long long)radiance_stats.cascade_gpu_bytes, (unsigned long long)radiance_stats.acceleration_gpu_bytes);
	Con_Printf ("Radiance: interval_budget=%u updated_level=%u\n", radiance_stats.interval_budget, radiance_stats.updated_level);
	if (radiance_stats.refresh_work)
		Con_Printf ("Radiance: refresh_slot=%u first=%u\n", radiance_stats.refresh_slot, radiance_stats.updated_first);
	Con_Printf ("Radiance: dynamic cached=%u updated=%u deferred=%u alias_interpolation_budget=%u\n",
		radiance_stats.dynamic_cached, radiance_stats.dynamic_updated, radiance_stats.dynamic_deferred, radiance_stats.alias_update_budget);
	Con_Printf ("Radiance: light_grid=%s cell=%.0f cells=%u indices=%u camera_candidates=%u\n",
		radiance_stats.grid_fine ? "map" : "camera", radiance_stats.grid_cell_size,
		radiance_stats.grid_cells, radiance_stats.grid_indices, radiance_stats.grid_camera_candidates);
	Con_Printf ("Radiance: last frame scene_upload=%llu B BLAS_upload static=%llu B dynamic=%llu B; builds static=%u dynamic=%u TLAS=%u; updates dynamic=%u TLAS=%u; light_grid_builds=%llu\n",
		(unsigned long long)radiance_stats.scene_upload_bytes, (unsigned long long)radiance_stats.static_blas_upload_bytes,
		(unsigned long long)radiance_stats.dynamic_blas_upload_bytes, radiance_stats.static_blas_builds,
		radiance_stats.dynamic_blas_builds, radiance_stats.tlas_builds, radiance_stats.dynamic_blas_updates,
		radiance_stats.tlas_updates, (unsigned long long)radiance_stats.light_grid_builds);
	if (radiance_stats.timing_available)
		Con_Printf ("Radiance: completed GPU prepass=%.3f ms (receiver shading excluded)\n", radiance_stats.gpu_ms);
	else
		Con_Printf ("Radiance: GPU timing unavailable\n");
	if (radiance_stats.cpu_profile_available)
		Con_Printf ("Radiance: CPU prepare=%.3f ms pipelines=%.3f scene=%.3f lights=%.3f upload=%.3f sparse=%.3f dispatch=%.3f\n",
			radiance_stats.cpu_prepare_ms, radiance_stats.cpu_pipeline_ms, radiance_stats.cpu_scene_ms,
			radiance_stats.cpu_lights_ms, radiance_stats.cpu_upload_ms, radiance_stats.cpu_sparse_ms, radiance_stats.cpu_dispatch_ms);
}

void R_RadianceInit (void)
{
	Cvar_RegisterVariable (&r_radiance);
	Cvar_RegisterVariable (&r_radiance_backend);
	Cvar_RegisterVariable (&r_radiance_quality);
	Cvar_RegisterVariable (&r_radiance_point_gain);
	Cvar_RegisterVariable (&r_radiance_emissive_gain);
	Cvar_RegisterVariable (&r_radiance_bounce_gain);
	Cvar_RegisterVariable (&r_radiance_sky_fallback);
	Cvar_RegisterVariable (&r_radiance_probe_gaussian);
	Cvar_RegisterVariable (&r_radiance_probe_spacing);
	Cvar_SetCallback (&r_radiance_probe_spacing, R_RadianceProbeSpacingChanged);
	Cvar_RegisterVariable (&r_radiance_gi_distance);
	Cvar_RegisterVariable (&r_radiance_spot_softness);
	Cvar_RegisterVariable (&r_radiance_soft_shadows);
	Cvar_RegisterVariable (&r_radiance_shadow_radius);
	Cvar_RegisterVariable (&r_radiance_shadow_sun_angle);
	Cvar_RegisterVariable (&r_radiance_shadow_filter);
	Cvar_RegisterVariable (&r_radiance_grid);
	Cvar_RegisterVariable (&r_radiance_highlight);
	Cvar_RegisterVariable (&r_radiance_profile);
	Cvar_RegisterVariable (&r_radiance_batch_shadows);
	Cvar_RegisterVariable (&r_radiance_resolution_scale);
	Cvar_RegisterVariable (&r_radiance_half_resolution);
	Cvar_SetCallback (&r_radiance_resolution_scale, R_RadianceResolutionChanged);
	Cvar_SetCallback (&r_radiance_half_resolution, R_RadianceResolutionChanged);
	Cvar_RegisterVariable (&r_radiance_checkerboard);
	Cvar_RegisterVariable (&r_radiance_deferred);
	Cvar_RegisterVariable (&r_radiance_receiver_filter);
	Cvar_RegisterVariable (&r_radiance_receiver_diagnostics);
	Cmd_AddCommand ("radiance_stats", R_RadianceStats_f);
	R_RadianceReset (NULL);
}
