/* World vertex positions reconstructed from clip coordinates. This also
 * handles direct brush draws whose model transform is folded into the MVP. */
layout (std140, set = 5, binding = 20) uniform RCReceiverCamera
{
	mat4 rcInverseViewProjection;
};

vec3 RC_WorldFromClip (vec4 clip)
{
	vec4 world = rcInverseViewProjection * clip;
	return abs (world.w) > 1e-10 ? world.xyz / world.w : vec3 (0.0);
}
