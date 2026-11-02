#version 460
#extension GL_GOOGLE_include_directive : require
#define CLOUDS_COMPOSITE
#define CLOUDS_MSAA
#include "sky_clouds_common.glsl"
vec4 sample_cloud_target (sampler2D image, vec4 target, vec2 local_pixel)
{
	ivec2 extent = ivec2 (target.xy);
	ivec2 block = clamp (ivec2 (local_pixel / target.z), ivec2 (0), extent - 1);
	vec4  cloud = texelFetch (image, block, 0);
	if (cloud.a >= 0.0 && target.w > 0.0)
	{
		// Filter only valid, unobstructed blocks within this band. Never blend
		// through mixed/occluded markers or across a terrain boundary.
		vec2  texel = local_pixel / target.z - 0.5;
		ivec2 base = ivec2 (floor (texel));
		vec4  a = texelFetch (image, clamp (base, ivec2 (0), extent - 1), 0);
		vec4  b = texelFetch (image, clamp (base + ivec2 (1, 0), ivec2 (0), extent - 1), 0);
		vec4  c = texelFetch (image, clamp (base + ivec2 (0, 1), ivec2 (0), extent - 1), 0);
		vec4  d = texelFetch (image, clamp (base + ivec2 (1, 1), ivec2 (0), extent - 1), 0);
		if (min (min (a.a, b.a), min (c.a, d.a)) >= 0.0)
			cloud = mix (mix (a, b, fract (texel.x)), mix (c, d, fract (texel.x)), fract (texel.y));
	}
	return cloud;
}

vec4 trace_cloud_pixel (vec3 view_dir, float footprint, int band)
{
	vec2 interval = cloud_view_interval (in_view_dir);
	interval.y = min (interval.y, opaque_distance_at (ivec2 (gl_FragCoord.xy), in_view_dir));
	if (interval.x < 0.0 || interval.y <= interval.x)
		return vec4 (0.0);
	if (band == 0)
		return raymarch_clouds (
			push_constants.eye_pos, view_dir, interval, normalize (push_constants.sun_dir), push_constants.time, interleaved_gradient_noise (gl_FragCoord.xy),
			footprint);
	return raymarch_clouds_banded (
		push_constants.eye_pos, view_dir, interval, normalize (push_constants.sun_dir), push_constants.time, interleaved_gradient_noise (gl_FragCoord.xy),
		footprint, band, cloud_frame.params.x);
}

void main ()
{
	vec3  view_dir = normalize (in_view_dir);
	float footprint = max (length (dFdx (view_dir)), length (dFdy (view_dir)));
	vec2  local_pixel = gl_FragCoord.xy - cloud_frame.viewport.xy;
	int	  diagnostic_band = int (cloud_frame.params.z);
	vec4  cloud;
	if (cloud_frame.params.y <= 0.0)
	{
		// The disabled path retains the original single-target composition and
		// whole-ray schedule. Diagnostics select only integration contributions.
		cloud = sample_cloud_target (cloud_radiance, cloud_frame.target, local_pixel);
		if (cloud.a < -1.5)
			discard;
		if (cloud.a < 0.0)
			cloud = trace_cloud_pixel (view_dir, footprint, diagnostic_band);
	}
	else
	{
		vec4 near_cloud = vec4 (0.0);
		vec4 far_cloud = vec4 (0.0);
		if (diagnostic_band != 2)
			near_cloud = sample_cloud_target (cloud_radiance, cloud_frame.target, local_pixel);
		if (diagnostic_band != 1)
			far_cloud = sample_cloud_target (cloud_far_radiance, cloud_frame.far_target, local_pixel);
		if (near_cloud.a == -1.0 || far_cloud.a == -1.0)
		{
			// A mixed band makes the pixel use one exact, depth-clipped ray for
			// all requested bands; never combine incompatible edge approximations.
			cloud = trace_cloud_pixel (view_dir, footprint, diagnostic_band);
		}
		else
		{
			if (near_cloud.a < 0.0)
				near_cloud = vec4 (0.0);
			if (far_cloud.a < 0.0)
				far_cloud = vec4 (0.0);
			// Premultiplied radiance and opacity compose front-to-back in HDR.
			// Tone mapping and fog are applied once to the combined result.
			cloud = near_cloud + (1.0 - near_cloud.a) * far_cloud;
		}
	}
	if (cloud.a <= 0.0)
		discard;
	out_frag_color = cloud_display (cloud);
}
