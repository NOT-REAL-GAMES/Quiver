#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : require

#include "tonemap_agx.glsl"
#include "sky_atmosphere_common.glsl"

layout (set = 0, binding = 0) uniform sampler2D atmosphere_lut;

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	vec3  eye_pos;
	float display_scale;
	vec3  sun_dir;
	float sun_intensity;
	float use_lut;
}
push_constants;

layout (location = 0) in vec3 in_view_dir;

layout (location = 0) out vec4 out_frag_color;

void main ()
{
	// Quake uses Z-up coordinate system, atmosphere uses Y-up
	// Convert: Quake (X, Y, Z) -> Atmosphere (X, Z, Y)
	vec3 quake_view_dir = normalize (in_view_dir);
	vec3 atmos_view_dir = vec3 (quake_view_dir.x, quake_view_dir.z, -quake_view_dir.y);
	// The sky also fills distant gaps below the local horizon when flying
	// above the curved cloud deck. Continue horizon haze into those directions
	// instead of shading an unlit, imaginary Earth surface behind the world.
	// A smooth maximum keeps the horizon free of a sharp lighting boundary.
	float horizon_blend = max (0.02 - abs (atmos_view_dir.y), 0.0) / 0.02;
	float sky_elevation = max (atmos_view_dir.y, 0.0) + 0.005 * horizon_blend * horizon_blend;
	vec2 horizontal_dir = atmos_view_dir.xz;
	float horizontal_length = length (horizontal_dir);
	horizontal_dir = horizontal_length > 0.00001 ? horizontal_dir / horizontal_length : vec2 (1.0, 0.0);
	horizontal_dir *= sqrt (max (1.0 - sky_elevation * sky_elevation, 0.0));
	atmos_view_dir = vec3 (horizontal_dir.x, sky_elevation, horizontal_dir.y);

	vec3 quake_sun_dir = normalize (push_constants.sun_dir);
	vec3 atmos_sun_dir = vec3 (quake_sun_dir.x, quake_sun_dir.z, -quake_sun_dir.y);

	// Square-root elevation gives the thin horizon scattering more LUT samples.
	vec2 lut_uv = vec2 (atan (atmos_view_dir.z, atmos_view_dir.x) / (2.0 * 3.14159265),
		sqrt (asin (clamp (atmos_view_dir.y, 0.0, 1.0)) / (0.5 * 3.14159265)));
	vec3 sky_color = push_constants.use_lut > 0.5 ? textureLod (atmosphere_lut, lut_uv, 0.0).rgb
		: compute_atmosphere (atmos_view_dir, atmos_sun_dir);
	sky_color *= push_constants.sun_intensity;

	sky_color = AgX_ForScene (sky_color, push_constants.display_scale);

	out_frag_color = vec4 (sky_color, 1.0);

	// Apply fog
	if (push_constants.fog_density > 0.0)
		out_frag_color.rgb = mix (out_frag_color.rgb, push_constants.fog_color, push_constants.fog_density);
}
