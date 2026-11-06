/* With the renderer's positive-height Vulkan viewport, framebuffer Y points
 * down. The reversed cross product makes the geometric normal face the
 * viewer, as the visible side of a Quake world surface should. */
vec3 RC_WorldSurfaceNormal (vec3 world_position)
{
	vec3 geometric = cross (dFdy (world_position), dFdx (world_position));
	float normal_length = length (geometric);
	return normal_length > 1e-10 ? geometric / normal_length : vec3 (0.0, 0.0, 1.0);
}
