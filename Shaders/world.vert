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
	layout (offset = 84) float bend[10];
}
push_constants;

layout (location = 0) in vec3 in_position;
layout (location = 1) in vec2 in_texcoord1;
layout (location = 2) in vec2 in_texcoord2;

layout (location = 0) out vec4 out_texcoords;
layout (location = 1) out float out_fog_frag_coord;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main ()
{
	out_texcoords.xy = in_texcoord1.xy;
	out_texcoords.zw = in_texcoord2.xy;
	gl_Position = push_constants.mvp * vec4 (in_position, 1.0f);

	vec3 projected = gl_Position.xyw;
	vec2 relativeXY = vec2 (dot (projected, vec3 (push_constants.bend[0], push_constants.bend[1], push_constants.bend[2])),
		dot (projected, vec3 (push_constants.bend[3], push_constants.bend[4], push_constants.bend[5])));
	gl_Position -= vec4 (push_constants.bend[6], push_constants.bend[7], push_constants.bend[8], push_constants.bend[9]) * planet_sag (length (relativeXY)).x;

	out_fog_frag_coord = gl_Position.w;
}
