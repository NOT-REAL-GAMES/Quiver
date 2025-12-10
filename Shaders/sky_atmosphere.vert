#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	vec3  eye_pos;
	float time;
	vec3  sun_dir;
	float sun_intensity;
}
push_constants;

layout (location = 0) in vec3 in_position;

layout (location = 0) out vec3 out_view_dir;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main ()
{
	gl_Position = push_constants.mvp * vec4 (in_position, 1.0f);

	// View direction is from eye to vertex position (in world space)
	out_view_dir = in_position - push_constants.eye_pos;
}
