#version 460
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 world_position;
layout(std430, set = 0, binding = 0) buffer Results { vec4 normal_and_light; } output_data;
#include "radiance_world_normal.glsl"
void main ()
{
	vec3 normal = RC_WorldSurfaceNormal(world_position);
	output_data.normal_and_light = vec4(normal, max(dot(normal, vec3(-1, 0, 0)), 0.0));
}
