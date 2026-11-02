/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tonemap_agx.glsl"
#include "planet_curvature.glsl"

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	uint  fog_color_and_scale;
	float fog_density;
	float cloud_speed;
	float cloud_scale;
	vec3  eye_pos;
	float time;
	vec3  sun_dir;
	float sun_intensity;
	float cloud_height;	   // base height of cloud layer
	float cloud_thickness; // thickness of cloud layer
	float cloud_density;   // overall opacity
	float cloud_coverage;  // how much sky is covered
}
push_constants;

layout (set = 0, binding = 0) uniform sampler3D noise_tex;
#if defined(CLOUDS_REDUCED) || defined(CLOUDS_COMPOSITE)
#include "sky_clouds_frame.glsl"
#ifdef CLOUDS_MSAA
layout (set = 1, binding = 0) uniform sampler2DMS scene_depth;
layout (constant_id = 0) const int DEPTH_SAMPLES = 1;
#else
layout (set = 1, binding = 0) uniform sampler2D scene_depth;
#endif
#ifdef CLOUDS_COMPOSITE
layout (set = 3, binding = 0) uniform sampler2D cloud_radiance;
layout (set = 3, binding = 1) uniform sampler2D cloud_far_radiance;
#endif
#else
#ifdef CLOUDS_MSAA
layout (input_attachment_index = 0, set = 1, binding = 0) uniform subpassInputMS scene_depth;
layout (constant_id = 0) const int DEPTH_SAMPLES = 1;
#else
layout (input_attachment_index = 0, set = 1, binding = 0) uniform subpassInput scene_depth;
#endif
#endif

layout (location = 0) in vec3 in_view_dir;
layout (location = 1) flat in vec2 in_depth_terms;

layout (location = 0) out vec4 out_frag_color;

// The pass reads opaque depth without changing it. Clip the cloud interval to
// that surface before marching, including when the camera is above the layer.
layout (early_fragment_tests) in;

// Hash function for dithering (interleaved gradient noise)
float interleaved_gradient_noise (vec2 screen_pos)
{
	vec3 magic = vec3 (0.06711056, 0.00583715, 52.9829189);
	return fract (magic.z * fract (dot (screen_pos, magic.xy)));
}

// A local spherical shell curves the cloud deck down toward the horizon. Its
// horizontal center follows the camera; noise still uses canonical world XY.
// Keep the center below even deep negative-height maps, without folding Z.
bool planet_cloud_curvature ()
{
	return (floatBitsToUint (push_constants.cloud_coverage) & 0x80000000u) != 0u;
}

float cloud_planet_radius ()
{
	if (planet_cloud_curvature ())
		return 203872000.0;
	return max (262144.0, -2.0 * min (push_constants.eye_pos.z, push_constants.cloud_height));
}

float cloud_altitude (vec3 pos)
{
	if (planet_cloud_curvature ())
		return pos.z + planet_sag (length (pos.xy - push_constants.eye_pos.xy)).x;
	float radius = cloud_planet_radius ();
	vec2 relative_xy = pos.xy - push_constants.eye_pos.xy;
	// Rationalized length(p)-radius preserves precision near the layer base.
	float numerator = dot (relative_xy, relative_xy) + pos.z * (2.0 * radius + pos.z);
	return numerator / (length (vec3 (relative_xy, radius + pos.z)) + radius);
}

vec2 ray_altitude_sphere (float eye_height, vec3 ray_dir, float altitude)
{
	float radius = cloud_planet_radius ();
	float b = (radius + eye_height) * ray_dir.z;
	float c = (eye_height - altitude) * (2.0 * radius + eye_height + altitude);
	if (planet_cloud_curvature ())
	{
		// A flat altitude plane bent by the shared sag is a sphere of radius R
		// whose center is translated vertically by altitude (not radius R+h).
		b = (radius + eye_height - altitude) * ray_dir.z;
		c = (eye_height - altitude) * (2.0 * radius + eye_height - altitude);
	}
	float discriminant = b * b - c;
	if (discriminant < 0.0)
		return vec2 (-1.0);
	// The second root uses the product c to avoid cancellation when the eye
	// is near a layer boundary. A tangent has an empty, zero-length interval.
	float root = sqrt (discriminant);
	float q = -b - (b >= 0.0 ? root : -root);
	if (abs (q) < 0.000001)
		return vec2 (0.0);
	return vec2 (min (q, c / q), max (q, c / q));
}

// Reconstruct the same clip planes used by rasterized world geometry. With
// reverse Z, depth 0 (cleared sky) is the far plane, not an unlimited ray.
// Using the unnormalized ray accounts for FOV and off-axis screen pixels.
vec2 cloud_view_interval (vec3 ray)
{
	float ray_length = length (ray);
	vec3 ray_dir = ray / ray_length;
	vec3 ray_origin = push_constants.eye_pos;
	float layer_bottom = push_constants.cloud_height;
	float layer_top = layer_bottom + push_constants.cloud_thickness;
	// The flat-near blend differs from the sphere by less than 0.17 units.
	// Enclose that discrepancy; density sampling rejects the extra interval.
	float margin = planet_cloud_curvature () ? 0.25 : 0.0;
	vec2 outer = ray_altitude_sphere (ray_origin.z, ray_dir, layer_top + margin);
	vec2 inner = ray_altitude_sphere (ray_origin.z, ray_dir, layer_bottom - margin);
	vec2 range = vec2 (max (outer.x, 0.0), outer.y);
	// Use the first forward segment of the shell, rather than marching the
	// empty interior or clouds on the opposite side of the virtual planet.
	if (inner.y > inner.x && inner.y > range.x)
	{
		if (inner.x > range.x)
			range.y = min (range.y, inner.x);
		else
			range.x = inner.y;
	}
	float ground_height = min (0.0, min (ray_origin.z, layer_bottom));
	vec2 ground = ray_altitude_sphere (ray_origin.z, ray_dir, ground_height);
	if (ground.x >= 0.0 && ground.y > ground.x)
		range.y = min (range.y, ground.x);
	range.x = max (range.x, ray_length / max (in_depth_terms.x + in_depth_terms.y, 1e-20));
	range.y = min (range.y, ray_length / max (in_depth_terms.x, 1e-20));
	if (range.y <= range.x)
		return vec2 (-1.0);
	return range;
}

float opaque_distance_at (ivec2 pixel, vec3 ray)
{
#ifdef CLOUDS_MSAA
	float depth = 0.0;
	for (int sample_index = 0; sample_index < DEPTH_SAMPLES; ++sample_index)
#if defined(CLOUDS_REDUCED) || defined(CLOUDS_COMPOSITE)
		depth = max (depth, texelFetch (scene_depth, pixel, sample_index).r);
#else
		depth = max (depth, subpassLoad (scene_depth, sample_index).r);
#endif
#else
#if defined(CLOUDS_REDUCED) || defined(CLOUDS_COMPOSITE)
	float depth = texelFetch (scene_depth, pixel, 0).r;
#else
	float depth = subpassLoad (scene_depth).r;
#endif
#endif
	// Reverse Z: every MSAA sample participates in the conservative nearest hit.
	if (depth <= 0.0)
		return 1e30;
	float reciprocal_w = in_depth_terms.x + depth * in_depth_terms.y;
	return length (ray) / max (reciprocal_w, 1e-20);
}

// The source texture contains four broad noise cells per axis plus detail.
// Fade unresolved octaves toward their mean instead of sampling rapidly
// changing high frequencies from a texture without a mip chain.
// Mean of Noise_CloudDensity3D over the generated four-cell, R8-quantized tile.
const float CLOUD_NOISE_MEAN = 0.5562505;
// The generated periodic R8 tile peaks at 197/255. Keep headroom, and apply
// this bound only to ordinary octaves: stochastic variance correction may
// exceed it. Nearest, linear, and unresolved-mean filtering preserve the bound.
const float CLOUD_NOISE_MAX = 0.78;

float noise_resolution (float uv_footprint)
{
	return 1.0 - smoothstep (0.25, 1.0, uv_footprint * 8.0);
}

float filtered_noise (vec3 uv, float uv_footprint)
{
	float resolution = noise_resolution (uv_footprint);
	if (resolution <= 0.0)
		return CLOUD_NOISE_MEAN;
	return mix (CLOUD_NOISE_MEAN, textureLod (noise_tex, uv, 0.0).r, resolution);
}

// Offset banks are stable in the world/wind frame. The analytic selector below
// repeats every ten base UV units, matching CloudCoordinates' canonical-phase
// reduction (whole periods also fit each wrapped planet). No screen, frame, or camera value enters the hash.
vec3 stochastic_translation (uint bank)
{
	// The selector lies in [7,25], with one neighboring bank for blending.
	// Cache the exact low 24 bits of the original integer hash rather than
	// repeating its multiply/xor chain in every view and lighting sample.
	const uint translations[20] = uint[20](
		0xbdbb81u, 0x5be59cu, 0xf85a61u, 0x4dba09u, 0xe3af6du, 0x6017b2u, 0x051b55u, 0x4cfb9fu, 0xc6f2d3u, 0x1de1c8u, 0xd55077u, 0xf80c6bu, 0xddca7du,
		0x022e09u, 0xf3da3au, 0xc455aeu, 0xa4729au, 0xcbbb06u, 0x588259u, 0xf15b88u);
	uint hash = translations[bank - 7u];
	return vec3 (hash & 255u, (hash >> 8) & 255u, (hash >> 16) & 255u) * (1.0 / 256.0);
}

float stochastic_base_noise (vec3 uv)
{
	// Smooth control avoids magnifying the nearest-filtered broad field's R8
	// voxel steps into whole-cloud discontinuities. Only the selector is smooth;
	// every cloud-noise lookup still uses the requested nearest/linear sampler.
	vec2  phase = uv.xy * (6.28318530718 / 10.0);
	float selector = 16.0 + 3.5 * sin (phase.x) + 3.5 * sin (phase.y) + 2.0 * sin (phase.x + phase.y);
	uint  bank = uint (floor (selector));
	float fraction = fract (selector);
	// Keep one source across most of each region and crossfade only through
	// its middle. The smooth regions are spatially coherent, so these branches
	// avoid the second hash/fetch in most view and lighting density evaluations.
	// Smooth endpoints retain continuity both here and between offset banks.
	uint  first_bank = bank + uint (fraction >= 0.65);
	float a = textureLod (noise_tex, uv + stochastic_translation (first_bank), 0.0).r;
	if (fraction <= 0.35 || fraction >= 0.65)
		return a;
	float weight = smoothstep (0.35, 0.65, fraction);
	float b = textureLod (noise_tex, uv + stochastic_translation (bank + 1u), 0.0).r;
	// Adjacent banks share an endpoint. The value and weight derivative agree
	// at a bank boundary, so the selector introduces no discontinuous tile edges.
	// Preserve source contrast instead of flattening coverage in blended areas.
	float variance_scale = inversesqrt (weight * weight + (1.0 - weight) * (1.0 - weight));
	return clamp (CLOUD_NOISE_MEAN + (mix (a, b, weight) - CLOUD_NOISE_MEAN) * variance_scale, 0.0, 1.0);
}

// FBM (Fractal Brownian Motion) - multiple octaves of noise for natural variation
float fbm_noise (vec3 uv, float time, float modulation, float uv_footprint)
{
	// Sum of the original five octave amplitudes: .5 + .125 + .03125 +
	// .0078125 + .001953125. Keep every octave that can affect coverage.
	const float TOTAL_AMPLITUDE = 0.666015625;
	float		resolution = noise_resolution (uv_footprint);
	if (resolution <= 0.0)
		return CLOUD_NOISE_MEAN;

	vec2  wind = time * push_constants.cloud_speed * vec2 (1.0, 0.3);
	vec3  base_uv = uv + vec3 (wind, 0.0);
	float noise = mix (CLOUD_NOISE_MEAN, stochastic_base_noise (base_uv), resolution) * 0.5;
	float remaining_amplitude = TOTAL_AMPLITUDE - 0.5;
	float upper_bound = max ((noise + CLOUD_NOISE_MAX * remaining_amplitude) / TOTAL_AMPLITUDE + 0.000001, 0.01);
	if (upper_bound * modulation <= 1.0 - abs (push_constants.cloud_coverage))
		return -1.0;

	// Peel the stochastic octave out of this loop. Hashes, offset samples, and
	// selector temporaries are dead before the ordinary octave march starts,
	// avoiding their branch/register cost in each nested view/light evaluation.
	float amplitude = 0.125;
	float frequency = 8.0;
	for (int i = 1; i < 5; i++)
	{
		resolution = noise_resolution (uv_footprint * frequency);
		if (resolution <= 0.0)
		{
			noise += CLOUD_NOISE_MEAN * remaining_amplitude;
			break;
		}
		vec3 sample_uv = uv * frequency;
		sample_uv.xy += wind * (1.0 + float (i) * 0.2);
		noise += mix (CLOUD_NOISE_MEAN, textureLod (noise_tex, sample_uv, 0.0).r, resolution) * amplitude;
		remaining_amplitude -= amplitude;

		// Reject only when even the largest remaining ordinary octaves cannot
		// produce density; the stochastic first octave is already evaluated.
		if (i < 4)
		{
			upper_bound = max ((noise + CLOUD_NOISE_MAX * remaining_amplitude) / TOTAL_AMPLITUDE + 0.000001, 0.01);
			if (upper_bound * modulation <= 1.0 - abs (push_constants.cloud_coverage))
				return -1.0;
		}
		amplitude *= 0.25;
		frequency *= 8.0;
	}
	return noise / TOTAL_AMPLITUDE;
}

// Domain warping - distorts the sampling position for more organic shapes
vec3 domain_warp (vec3 uv, float time, float uv_footprint)
{
	float wind_speed = time * push_constants.cloud_speed * 0.5;
	vec2  wind = vec2 (wind_speed, wind_speed * 0.7);

	// Sample noise to get warp offsets
	float warp_x = filtered_noise (uv * 0.7 + vec3 (wind, 0.0), uv_footprint * 0.7) - 0.5;
	float warp_y = filtered_noise (uv * 0.7 + vec3 (0.0, wind.y, wind.x), uv_footprint * 0.7) - 0.5;
	float warp_z = filtered_noise (uv * 0.7 + vec3 (wind.y, 0.0, wind.x), uv_footprint * 0.7) - 0.5;

	// Apply warp (scaled down to be subtle)
	return uv + vec3 (warp_x, warp_y, warp_z) * 0.15;
}

// Sample cloud density from 3D noise texture with FBM
float sample_cloud_density (vec3 pos, float time, float footprint, out float height_fraction)
{
	height_fraction = (cloud_altitude (pos) - push_constants.cloud_height) / push_constants.cloud_thickness;
	if (height_fraction <= 0.0 || height_fraction >= 1.0)
		return 0.0;

	// Scale position to noise texture coordinates (controlled by r_clouds_scale cvar)
	// March and depth clipping operate in bent rendering space. The inverse
	// transform keeps the density texture anchored in the flat generated world.
	vec3 texture_pos = pos;
	if (planet_cloud_curvature ())
		texture_pos.z = cloud_altitude (pos);
	vec3  uv = texture_pos * push_constants.cloud_scale;
	float uv_footprint = footprint * abs (push_constants.cloud_scale);

	float large_scale = filtered_noise (uv * 0.1 + vec3 (time * push_constants.cloud_speed * 0.1, 0.0, 0.0), uv_footprint * 0.1);
	float modulation = 0.7 + 0.6 * max (large_scale, 0.01);

	// Once the base octave is unresolved, FBM is exactly its mean regardless
	// of position. Avoid the three warp samples whose result would be unused.
	float noise = CLOUD_NOISE_MEAN;
	float resolution = noise_resolution (uv_footprint);
	if (resolution > 0.0)
	{
		// Blending two values with variance correction cannot exceed mean +
		// sqrt(2) * (CLOUD_NOISE_MAX - mean). Later octaves have no greater
		// resolution than octave one. Bound the complete filtered FBM before
		// any warp/stochastic fetch; rejection produces exactly zero density.
		const float STOCHASTIC_MAX = 0.8727;
		const float TAIL_AMPLITUDE = 0.166015625;
		float		max_base = mix (CLOUD_NOISE_MEAN, STOCHASTIC_MAX, resolution);
		float		max_tail = mix (CLOUD_NOISE_MEAN, CLOUD_NOISE_MAX, noise_resolution (uv_footprint * 8.0));
		float		upper_bound = (0.5 * max_base + TAIL_AMPLITUDE * max_tail) / (0.5 + TAIL_AMPLITUDE) + 0.000001;
		if (upper_bound * modulation <= 1.0 - abs (push_constants.cloud_coverage))
			return 0.0;

		vec3 warped_uv = domain_warp (uv, time, uv_footprint);
		noise = fbm_noise (warped_uv, time, modulation, uv_footprint);
		if (noise < 0.0)
			return 0.0;
	}

	// Ensure noise is never exactly zero (avoid black spots from texture edge cases)
	noise = max (noise, 0.01);

	noise *= modulation;

	// Apply coverage threshold with smooth falloff
	float coverage = abs (push_constants.cloud_coverage);
	noise = smoothstep (1.0 - coverage, 1.0 - coverage * 0.3, noise);

	// Smoother falloff at top edge to avoid artifacts
	float height_falloff = 4.0 * height_fraction * (1.0 - height_fraction); // Parabola
	// Extra fade at top 10% of layer
	if (height_fraction > 0.9)
		height_falloff *= 1.0 - smoothstep (0.9, 1.0, height_fraction);

	return noise * height_falloff * push_constants.cloud_density;
}

// Beer's law light attenuation
float beer_law (float optical_depth)
{
	return exp (-optical_depth);
}

// Ray march through cloud layer
vec4 raymarch_clouds_banded (
	vec3 ray_origin, vec3 ray_dir, vec2 t_range, vec3 sun_dir, float time, float dither, float ray_footprint, int band, float split_distance)
{
	const int	MAX_STEPS = 80;
	const int	LIGHT_STEPS = 4;
	// Beyond this depth the omitted direct light is at most 1e-4 * sun_color.
	const float LIGHT_DEPTH_CUTOFF = 9.2103404; // -log(1e-4)
	// Additional omission is bounded per premultiplied HDR channel, before
	// tone mapping: at most 80 * 1e-5 = 8e-4 over a complete view ray.
	const float LIGHT_CONTRIBUTION_EPSILON = 1e-5;

	float ray_length = t_range.y - t_range.x;
	float target_step = push_constants.cloud_thickness / 32.0;
	int	  step_count = int (clamp (ceil (ray_length / target_step), 24.0, float (MAX_STEPS)));
	// Short rays remain uniform. Long grazing rays spend their budget near the
	// front of the volume, before widening the distant, filtered segments.
	float near_weight = clamp (1.0 - float (step_count) * target_step / ray_length, 0.0, 0.9);
	// Opacity describes the layer rather than depending on world units or the
	// number of samples. Dense near clouds still extinguish the distant layer.
	float extinction = 160.0 / push_constants.cloud_thickness;
	float light_extinction = extinction;

	vec3  accumulated_color = vec3 (0.0);
	float accumulated_alpha = 0.0;

	// Sun color (warm for daytime)
	vec3  sun_color = vec3 (1.0, 0.95, 0.8) * push_constants.sun_intensity;
	float max_sun_radiance = max (max (abs (sun_color.r), abs (sun_color.g)), abs (sun_color.b));

	// Static, centered jitter breaks up bands without jumping over an entire
	// near-cloud segment every frame. Each segment's full length is integrated.
	float sample_fraction = mix (0.35, 0.65, dither);
	float light_step = push_constants.cloud_thickness * 0.5 / float (LIGHT_STEPS);
	vec3  light_step_vector = sun_dir * light_step;
	float segment_start = t_range.x;
	int	  segment_index = 0;
#ifdef CLOUDS_REDUCED
	// Gradually pair far intervals once a ray exceeds 64 original steps.
	// The untouched prefix never falls below 32; the longest rays use 56
	// evaluations without an abrupt quality switch at the 80-step limit.
	int paired_intervals = min ((3 * max (step_count - 64, 0)) & ~1, max (step_count - 32, 0));
	int unmerged_prefix = step_count - paired_intervals;
#endif

	for (int i = 0; i < MAX_STEPS; i++)
	{
		if (segment_index >= step_count || accumulated_alpha > 0.995)
			break;

		int end_index = segment_index + 1;
#ifdef CLOUDS_REDUCED
		// Cover each coalesced interval completely, using its corresponding
		// integration step size and noise-filter footprint.
		// Full-resolution and depth-edge fallback rays retain all 80 intervals.
		if (segment_index >= unmerged_prefix)
			++end_index;
#endif
		float fraction = float (end_index) / float (step_count);
		segment_index = end_index;
		float segment_end = t_range.x + ray_length * mix (fraction, fraction * fraction, near_weight);
		float step_size = segment_end - segment_start;
		float t = mix (segment_start, segment_end, sample_fraction);
		float contribution_step = step_size;
		// Keep the whole-ray schedule, sample positions and filter footprint.
		// A straddling sample contributes the appropriate extinction length to
		// each band, so splitting a constant sample preserves Beer-Lambert law.
		if (band == 1)
		{
			if (segment_start >= split_distance)
				break;
			if (segment_end > split_distance)
				contribution_step = split_distance - segment_start;
		}
		else if (band == 2)
		{
			if (segment_end <= split_distance)
			{
				segment_start = segment_end;
				continue;
			}
			if (segment_start < split_distance)
				contribution_step = segment_end - split_distance;
		}
		segment_start = segment_end;

		vec3  sample_pos = ray_origin + ray_dir * t;
		float footprint = max (t * ray_footprint, step_size * 0.5);
		float height_frac;
		float density = sample_cloud_density (sample_pos, time, footprint, height_frac);
		float sample_alpha = 1.0 - exp (-density * contribution_step * extinction);

		if (sample_alpha > 0.0)
		{
			float view_weight = sample_alpha * (1.0 - accumulated_alpha);
			float max_direct_radiance = view_weight * max_sun_radiance;
			float light_transmittance = 0.0;
			if (max_direct_radiance > LIGHT_CONTRIBUTION_EPSILON)
			{
				// Optical depth only increases. Once the remaining direct sunlight
				// cannot change this view sample by epsilon, further light samples
				// are unnecessary. Ambient light and exact opacity remain untouched.
				float light_cutoff = min (LIGHT_DEPTH_CUTOFF, log (max_direct_radiance / LIGHT_CONTRIBUTION_EPSILON));
				float light_optical_depth = 0.0;
				vec3  light_pos = sample_pos + 0.5 * light_step_vector;
				for (int j = 0; j < LIGHT_STEPS; j++)
				{
					float light_height_fraction;
					float light_density = sample_cloud_density (light_pos, time, max (footprint, light_step * 0.5), light_height_fraction);
					if (light_height_fraction >= 1.0 || light_height_fraction <= 0.0)
						break;
					light_optical_depth += light_density * light_step * light_extinction;
					if (light_optical_depth >= light_cutoff)
						break;
					light_pos += light_step_vector;
				}
				if (light_optical_depth < light_cutoff)
					light_transmittance = beer_law (light_optical_depth);
			}

			// Ambient sky light from above (Z is up)
			// Clamp height fraction to valid range and ensure minimum ambient
			float ambient = 0.4 + 0.2 * clamp (height_frac, 0.0, 1.0);

			// Cloud color: sunlit + ambient (ensure minimum brightness to avoid black spots)
			vec3 cloud_color = sun_color * light_transmittance + vec3 (0.6, 0.7, 0.9) * ambient;
			cloud_color = max (cloud_color, vec3 (0.15, 0.18, 0.25)); // Minimum cloud brightness

			// Accumulate with front-to-back compositing
			accumulated_color += cloud_color * view_weight;
			accumulated_alpha += view_weight;
		}
	}

	return vec4 (accumulated_color, accumulated_alpha);
}

vec4 raymarch_clouds (vec3 ray_origin, vec3 ray_dir, vec2 t_range, vec3 sun_dir, float time, float dither, float ray_footprint)
{
	return raymarch_clouds_banded (ray_origin, ray_dir, t_range, sun_dir, time, dither, ray_footprint, 0, 0.0);
}

vec2 cloud_band_interval (vec2 interval, int band, float split_distance)
{
	if (band == 1)
		interval.y = min (interval.y, split_distance);
	else if (band == 2)
		interval.x = max (interval.x, split_distance);
	return interval;
}

vec4 cloud_display (vec4 cloud)
{
	if (cloud.a <= 0.0)
		return vec4 (0.0);
	vec4 fog = unpackUnorm4x8 (push_constants.fog_color_and_scale);
	cloud.rgb = AgX_ForScene (cloud.rgb / cloud.a, fog.a) * cloud.a;
	cloud.rgb = mix (cloud.rgb, fog.rgb * cloud.a, max (push_constants.fog_density, 0.0));
	return cloud;
}

#if !defined(CLOUDS_REDUCED) && !defined(CLOUDS_COMPOSITE)

void main ()
{
	vec3 view_dir = normalize (in_view_dir);
	vec3 sun_dir = normalize (push_constants.sun_dir);
	// Derivatives must be evaluated before divergent ray rejection.
	float ray_footprint = max (length (dFdx (view_dir)), length (dFdy (view_dir)));

	vec3 ray_origin = push_constants.eye_pos;
	vec2 t_range = cloud_view_interval (in_view_dir);
	t_range.y = min (t_range.y, opaque_distance_at (ivec2 (gl_FragCoord.xy), in_view_dir));

	if (t_range.x < 0.0 || t_range.y <= t_range.x)
		discard;

	float dither = interleaved_gradient_noise (gl_FragCoord.xy);

	// Ray march clouds
	vec4 cloud_color = raymarch_clouds (ray_origin, view_dir, t_range, sun_dir, push_constants.time, dither, ray_footprint);
	if (cloud_color.a == 0.0)
		discard;

	// Display transform is applied once, after HDR integration.
	out_frag_color = cloud_display (cloud_color);
}
#endif
