#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (push_constant) uniform PushConsts
{
	mat4 mvp;
}
push_constants;

layout (location = 0) out vec3 out_view_dir;
layout (location = 1) flat out vec2 out_depth_terms;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main ()
{
	const vec2 positions[3] = vec2[3] (vec2 (-1.0, -1.0), vec2 (3.0, -1.0), vec2 (-1.0, 3.0));
	vec2 ndc = positions[gl_VertexIndex];
	gl_Position = vec4 (ndc, 0.0, 1.0);

	// Reconstruct in the local render frame, independently of the periodic
	// cloud-noise eye. Inversion happens only at the three triangle vertices.
	mat4 inverse_mvp = inverse (push_constants.mvp);
	vec4 eye = inverse_mvp[2];
	vec4 far_point = inverse_mvp * vec4 (ndc, 0.0, 1.0);
	out_view_dir = far_point.xyz - eye.xyz * (far_point.w / eye.w);
	// Reverse-Z opaque distance is length(view_dir) / (x + depth * y).
	out_depth_terms = vec2 (far_point.w, inverse_mvp[2].w);
}
