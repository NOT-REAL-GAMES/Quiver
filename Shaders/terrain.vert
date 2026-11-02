#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable
#include "planet_curvature.glsl"
#extension GL_ARB_shading_language_420pack : enable

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	vec3  sun_dir;
	float sun_intensity;
	vec4  world_phase;
	vec4 curvature;
}
push_constants;

layout (location = 0) in vec3 in_position;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec4 in_material_weights;
layout (location = 3) in vec4 in_material_tint;

layout (location = 0) out vec4 out_material_weights;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec3 out_position;
layout (location = 3) out float out_fog_frag_coord;
layout (location = 4) out vec4 out_material_tint;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main ()
{
	gl_Position = push_constants.mvp * vec4 (in_position, 1.0);
	out_material_weights = in_material_weights;
	out_material_tint = in_material_tint;
	out_normal = in_normal;
	if (push_constants.curvature.z > 0.0)
	{
		vec2 relativeXY = in_position.xy + push_constants.curvature.xy;
		float distance = length (relativeXY);
		vec2 sag = planet_sag (distance);
		gl_Position -= push_constants.mvp[2] * sag.x;
		out_normal.xy += relativeXY * (sag.y * in_normal.z / max (distance, 1.0));
		out_normal = normalize (out_normal);
	}
	out_position = in_position + push_constants.world_phase.xyz;
	out_fog_frag_coord = gl_Position.w;
}
