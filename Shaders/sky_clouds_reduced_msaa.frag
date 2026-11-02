#version 460
#extension GL_GOOGLE_include_directive : require
#define CLOUDS_REDUCED
#define CLOUDS_MSAA
#include "sky_clouds_common.glsl"
layout (location = 2) flat in vec3 in_ray_base;
layout (location = 3) flat in vec3 in_ray_dx;
layout (location = 4) flat in vec3 in_ray_dy;
vec3 pixel_ray (vec2 pixel)
{
	return in_ray_base + in_ray_dx * pixel.x + in_ray_dy * pixel.y;
}
void main ()
{
	int	  band = int (cloud_frame.params.w);
	vec4  target = (band == 2 && cloud_frame.params.y > 0.0) ? cloud_frame.far_target : cloud_frame.target;
	int	  divisor = int (target.z);
	ivec2 block = ivec2 (gl_FragCoord.xy) * divisor;
	ivec2 extent = ivec2 (cloud_frame.viewport.zw);
	ivec2 origin = ivec2 (cloud_frame.viewport.xy);
	bool  all_empty = true;
	bool  all_clear = true;
	// The shell center follows camera XY, so this is the exact shortest
	// distance to the cloud band, independent of ray direction.
	float nearest_layer = max (
		0.0,
		max (
			push_constants.cloud_height - push_constants.eye_pos.z, push_constants.eye_pos.z - (push_constants.cloud_height + push_constants.cloud_thickness)));
	if (band == 2)
		nearest_layer = max (nearest_layer, cloud_frame.params.x);
	for (int y = 0; y < divisor; ++y)
		for (int x = 0; x < divisor; ++x)
		{
			ivec2 p = block + ivec2 (x, y);
			if (any (greaterThanEqual (p, extent)))
				continue;
			vec3  ray = pixel_ray (vec2 (p) + 0.5);
			float stop = opaque_distance_at (origin + p, ray);
			if (stop <= nearest_layer)
			{
				all_clear = false;
				continue;
			}
			vec2 interval = cloud_view_interval (ray);
			interval = cloud_band_interval (interval, band, cloud_frame.params.x);
			bool has_cloud = interval.x >= 0.0 && interval.y > interval.x;
			all_empty = all_empty && (!has_cloud || stop <= interval.x);
			all_clear = all_clear && has_cloud && stop >= interval.y;
		}
	if (all_empty)
	{
		// Keep occluded/absent intervals distinct from clear rays with no cloud
		// density, so linear upscaling cannot attenuate across terrain edges.
		out_frag_color = vec4 (0.0, 0.0, 0.0, -2.0);
		return;
	}
	if (!all_clear)
	{
		// Negative opacity is an exact-ray marker, never a displayed opacity.
		out_frag_color = vec4 (0.0, 0.0, 0.0, -1.0);
		return;
	}
	vec2 center = min (vec2 (block) + float (divisor) * 0.5, vec2 (extent) - 0.5);
	vec3 center_ray = pixel_ray (center);
	vec3 view_dir = normalize (center_ray);
	vec2 interval = cloud_view_interval (center_ray);
	if (interval.y <= interval.x)
	{
		// Individual pixels pass the shell/clip tests but their shared sample
		// misses at a grazing boundary. Resolve those rays in the composite.
		out_frag_color = vec4 (0.0, 0.0, 0.0, -1.0);
		return;
	}
	float footprint =
		max (length (normalize (pixel_ray (center + vec2 (divisor, 0))) - view_dir), length (normalize (pixel_ray (center + vec2 (0, divisor))) - view_dir));
	if (band == 0)
		out_frag_color = raymarch_clouds (
			push_constants.eye_pos, view_dir, interval, normalize (push_constants.sun_dir), push_constants.time,
			interleaved_gradient_noise (vec2 (origin) + center), footprint);
	else
		out_frag_color = raymarch_clouds_banded (
			push_constants.eye_pos, view_dir, interval, normalize (push_constants.sun_dir), push_constants.time,
			interleaved_gradient_noise (vec2 (origin) + center), footprint, band, cloud_frame.params.x);
}
