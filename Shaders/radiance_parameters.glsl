#ifndef QUIVER_RADIANCE_PARAMETERS_GLSL
#define QUIVER_RADIANCE_PARAMETERS_GLSL
layout (std140, set = RC_LIGHT_SET, binding = RC_LIGHT_BINDING_BASE + 1) uniform RCLightingParameters
{
	uvec4 counts; // light count, material count, texture layer count; flags: grid/fullbright/gains/sky/multisampled scene
	vec4 sky_radiance;
	vec4 ground_radiance;
	vec4 grid_origin;
	vec4 grid_inv_cell_size;
	uvec4 grid_dimensions; // xyz cell dimensions, w number of light indices
	vec4 bounce_parameters; // x: indirect gain; y: single-sample area visibility
	vec4 gi_camera_fade;
	vec4 receiver_filter;
	vec4 shadow_parameters; // receiver only: enable, finite radius, sun angular radius radians, visibility filter strength
	vec4 gi_parameters; // x: lookup offset along the normal, as a fraction of probe spacing; y: liquid shadow filter scale; zw: liquid token range
} rcLighting;
/* Lift a receiver's GI lookup off its surface, so probes do not flip between
 * visible and hidden with every small change in position. Bounce transport
 * keeps its own offset, so this control leaves probe history alone. */
#define RC_GI_LOOKUP_POINT(position, normal) ((position) + (normal) * (rcLighting.gi_parameters.x * rcLevels[0].origin_spacing.w))
#endif
