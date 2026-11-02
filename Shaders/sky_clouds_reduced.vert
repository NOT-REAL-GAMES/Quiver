#version 460
#extension GL_GOOGLE_include_directive : require
#include "sky_clouds_frame.glsl"
layout (push_constant) uniform PushConsts { mat4 mvp; } push_constants;
layout (location = 0) out vec3 out_view_dir;
layout (location = 1) flat out vec2 out_depth_terms;
layout (location = 2) flat out vec3 out_ray_base;
layout (location = 3) flat out vec3 out_ray_dx;
layout (location = 4) flat out vec3 out_ray_dy;
void main ()
{
	const vec2 positions[3] = vec2[3] (vec2 (-1.0, -1.0), vec2 (3.0, -1.0), vec2 (-1.0, 3.0));
	gl_Position = vec4 (positions[gl_VertexIndex], 0.0, 1.0);
	mat4 m = inverse (push_constants.mvp);
	vec3 eye = m[2].xyz / m[2].w;
	vec4 far_point = m * vec4 (-1.0, -1.0, 0.0, 1.0);
	out_ray_base = far_point.xyz - eye * far_point.w;
	out_ray_dx = (m[0].xyz - eye * m[0].w) * (2.0 / cloud_frame.viewport.z);
	out_ray_dy = (m[1].xyz - eye * m[1].w) * (2.0 / cloud_frame.viewport.w);
	out_depth_terms = vec2 (far_point.w, m[2].w);
	out_view_dir = out_ray_base;
}
