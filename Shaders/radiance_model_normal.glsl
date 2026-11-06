#ifndef QUIVER_RADIANCE_MODEL_NORMAL_GLSL
#define QUIVER_RADIANCE_MODEL_NORMAL_GLSL
/* Quake alias-model matrix columns include the MDL coordinate encoding
 * scales. Normals are already in decoded model space; remove those scales
 * while retaining entity rotation and reflection. */
vec3 RC_TransformModelNormal (mat4 transform, vec3 normal)
{
	vec3 x = transform[0].xyz, y = transform[1].xyz, z = transform[2].xyz;
	float lx = length (x), ly = length (y), lz = length (z);
	if (lx < 1e-10 || ly < 1e-10 || lz < 1e-10) return vec3 (0.0, 0.0, 1.0);
	vec3 result = mat3 (x / lx, y / ly, z / lz) * normal;
	float magnitude = length (result);
	return magnitude > 1e-10 ? result / magnitude : vec3 (0.0, 0.0, 1.0);
}

/* Prepass and forward lighting must use the same interpolated smooth normal,
 * including the safe zero-normal case and two-sided receiver orientation. */
vec3 RC_ModelReceiverNormal (vec3 interpolated, bool frontFacing)
{
	float magnitude = length (interpolated);
	vec3 normal = magnitude > 1e-10 ? interpolated / magnitude : vec3 (0.0, 0.0, 1.0);
	return frontFacing ? normal : -normal;
}
#endif
