#ifndef QUIVER_RADIANCE_PARAMETERS_GLSL
#define QUIVER_RADIANCE_PARAMETERS_GLSL
layout (std140, set = RC_LIGHT_SET, binding = RC_LIGHT_BINDING_BASE + 1) uniform RCLightingParameters
{
	uvec4 counts; // light count, material count, texture layer count; flags: grid/fullbright/gains
	vec4 sky_radiance;
	vec4 ground_radiance;
	vec4 grid_origin;
	vec4 grid_inv_cell_size;
	uvec4 grid_dimensions; // xyz cell dimensions, w number of light indices
	vec4 bounce_parameters; // x: indirect gain; y: single-sample area visibility
	vec4 gi_camera_fade;
	vec4 receiver_filter;
	vec4 shadow_parameters; // receiver only: enable, finite radius, sun angular radius radians, visibility filter strength
} rcLighting;
#endif
