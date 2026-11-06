#ifndef RC_LIGHT_QUERY_FREE_GLSL
#define RC_LIGHT_QUERY_FREE_GLSL
/* Light records and shared radiometric math only: no scene, material,
 * acceleration-structure or visibility declarations. */
const uint RC_LIGHT_POINT = 0u;
const uint RC_LIGHT_SPOT = 1u;
const uint RC_LIGHT_DIRECTIONAL = 2u;
const uint RC_LIGHT_EMISSIVE = 3u;

struct RCLight { vec4 position_radius; vec4 color_intensity; vec4 direction_cone; uvec4 metadata; vec4 area_axis; };
#ifndef RC_LIGHT_QUERY_FREE_TEST
layout (std430, set = RC_LIGHT_SET, binding = RC_LIGHT_BINDING_BASE + 0) readonly buffer RCLights { RCLight rcLights[]; };
#include "radiance_parameters.glsl"
#endif

bool RC_EmissiveBasis (RCLight light, out vec3 axis, out vec3 tangent, out vec3 bitangent)
{
	axis = light.direction_cone.xyz;
	float axisLength = length (axis);
	if (axisLength < 1e-6 || light.area_axis.w < 1e-4 || light.direction_cone.w < 1e-4) return false;
	axis /= axisLength;
	tangent = light.area_axis.xyz - axis * dot (axis, light.area_axis.xyz);
	float tangentLength = length (tangent);
	if (tangentLength < 1e-6) return false;
	tangent /= tangentLength;
	bitangent = cross (axis, tangent);
	return true;
}

vec3 RC_ClosestEmissionPoint (RCLight light, vec3 position)
{
	vec3 axis, tangent, bitangent;
	if (!RC_EmissiveBasis (light, axis, tangent, bitangent)) return light.position_radius.xyz;
	vec3 offset = position - light.position_radius.xyz;
	float x = clamp (dot (offset, tangent), -light.area_axis.w, light.area_axis.w);
	float y = clamp (dot (offset, bitangent), -light.direction_cone.w, light.direction_cone.w);
	return light.position_radius.xyz + tangent * x + bitangent * y;
}

float RC_LightFalloff (RCLight light, float distance)
{
	uint formula = (light.metadata.z & 17u) == 1u ? light.metadata.w & 7u : 0u;
	float radius = light.position_radius.w;
	if ((light.metadata.z & 8u) != 0u)
	{
		float minlight = uintBitsToFloat (light.metadata.y);
		if (isnan (minlight) || isinf (minlight) || minlight < 0.0) minlight = 0.0;
		if (radius <= 0.0 || distance >= radius - minlight) return 0.0;
		return max (1.0 - distance / radius, 0.0);
	}
	if (light.metadata.x == RC_LIGHT_EMISSIVE)
	{
		if (radius <= 0.0 || distance >= radius) return 0.0;
		return 1.0 - max (distance, 0.0) / radius;
	}
	if (formula != 3u && formula != 4u && (radius <= 0.0 || distance >= radius)) return 0.0;
	if (formula == 3u || formula == 4u) return 1.0;
	if (formula >= 1u && formula <= 5u)
	{
		float wait = uintBitsToFloat (light.metadata.w & ~7u);
		if (isnan (wait) || isinf (wait) || wait <= 0.0) wait = 1.0;
		float scaledDistance = max (distance * wait / 128.0, 0.0001);
		if (formula == 1u) return 1.0 / scaledDistance;
		if (formula == 2u) return 1.0 / (scaledDistance * scaledDistance);
		if (formula == 5u) return 1.0 / ((scaledDistance + 1.0) * (scaledDistance + 1.0));
	}
	float falloff = 1.0 - distance / radius;
	/* Compiler delay 0 and rerelease KEX dynamic lights use linear
	 * attenuation. Other synthetic lights keep their existing falloff. */
	return (light.metadata.z & 17u) != 0u ? falloff : falloff * falloff;
}

bool RC_IsLocalMinLight (RCLight light)
{
	return (light.metadata.z & 17u) == 1u && (light.metadata.w & 7u) == 4u;
}

/* Keep compiler grazing tolerance identical in hard and query-free paths. */
const float RC_COMPILER_GRAZING_COSINE = 1e-3;
#endif
