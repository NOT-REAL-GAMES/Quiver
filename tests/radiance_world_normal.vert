#version 460
layout(location = 0) out vec3 world_position;
void main ()
{
	const vec2 positions[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
	vec2 position = positions[gl_VertexIndex];
	/* Matches a zero-angle Quake camera looking +X: framebuffer right is -Y,
	 * framebuffer down is -Z, and this wall lies ahead at X=10. */
	world_position = vec3(10, -position.x, -position.y);
	gl_Position = vec4(position, 0, 1);
}
