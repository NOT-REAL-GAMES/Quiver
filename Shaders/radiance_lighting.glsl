/* Runtime direct-light and material evaluation shared by cascade backends.
 * Include radiance_scene_trace.glsl before this file. All radiometric values
 * are linear; the host applies Quake lightstyles before uploading lights.
 *
 * set 2 binding 0: RCLight[] (80-byte records, radiance_lights.h)
 *       binding 1: counts, sky radiance, and optional light-grid parameters
 *       binding 2: RCMaterial[] (80-byte records)
 *       binding 3: albedo/emission texture array, sampled with repeat UVs
 *       binding 4: CSR light-grid cell offsets
 *       binding 5: CSR light-grid light indices
 * A material with no corresponding texture uses its constant value. Texture
 * layers are supplied by the scene uploader, not BSP lightmap atlases.
 */
#ifndef RC_LIGHTING_GLSL
#define RC_LIGHTING_GLSL
#ifndef RC_LIGHT_SET
#define RC_LIGHT_SET 2
#endif
#ifndef RC_LIGHT_BINDING_BASE
#define RC_LIGHT_BINDING_BASE 0
#endif

#include "radiance_light_query_free.glsl"

const uint RC_MATERIAL_ALBEDO_TEXTURE = 1u;
const uint RC_MATERIAL_EMISSION_TEXTURE = 2u;

struct RCMaterial
{
	vec4 albedo_alpha;
	vec4 emission;
	uvec4 layers_flags; // albedo layer, emission layer, flags, reserved
	uvec4 albedo_rect; // x, y, width, height in texels
	uvec4 emission_rect;
};
layout (std430, set = RC_LIGHT_SET, binding = RC_LIGHT_BINDING_BASE + 2) readonly buffer RCMaterials { RCMaterial rcMaterials[]; };
layout (set = RC_LIGHT_SET, binding = RC_LIGHT_BINDING_BASE + 3) uniform sampler2DArray rcMaterialAtlas;
layout (std430, set = RC_LIGHT_SET, binding = RC_LIGHT_BINDING_BASE + 4) readonly buffer RCLightGridOffsets { uint rcLightGridOffsets[]; };
layout (std430, set = RC_LIGHT_SET, binding = RC_LIGHT_BINDING_BASE + 5) readonly buffer RCLightGridIndices { uint rcLightGridIndices[]; };

float RC_BounceGain ()
{
	if ((rcLighting.counts.w & 8u) == 0u) return 1.0;
	float gain = rcLighting.bounce_parameters.x;
	return isnan (gain) || isinf (gain) ? 1.0 : max (gain, 0.0);
}

/* Apply only at receivers. Probe transport remains camera-independent. */
float RC_IndirectDistanceWeight (vec3 position)
{
	float endDistance = rcLighting.gi_camera_fade.w;
	if (endDistance <= 0.0) return 1.0;
	endDistance = max (endDistance, 1.0); /* keep smoothstep edges distinct */
	return 1.0 - smoothstep (endDistance * 0.75, endDistance, distance (position, rcLighting.gi_camera_fade.xyz));
}

vec4 RC_AtlasNearest (vec2 uv, uint layer, uvec4 rect)
{
	if (rect.z == 0u || rect.w == 0u) return vec4 (0.0);
	/* Fetch only original texels, including across negative/repeated UVs.
	 * Unlike normalized sampling this never bleeds into a neighboring rect. */
	uvec2 pixel = min (uvec2 (fract (uv) * vec2 (rect.zw)), rect.zw - uvec2 (1));
	return texelFetch (rcMaterialAtlas, ivec3 (ivec2 (rect.xy + pixel), int (layer)), 0);
}

vec4 RC_MaterialAlbedo (RCSceneHit hit)
{
	if (hit.material >= rcLighting.counts.y) return vec4 (0.0);
	RCMaterial material = rcMaterials[hit.material];
	vec4 result = material.albedo_alpha;
	if ((material.layers_flags.z & RC_MATERIAL_ALBEDO_TEXTURE) != 0u && material.layers_flags.x < rcLighting.counts.z)
		result *= RC_AtlasNearest (hit.uv, material.layers_flags.x, material.albedo_rect);
	return result;
}

vec3 RC_MaterialEmission (RCSceneHit hit)
{
	if (hit.material >= rcLighting.counts.y) return vec3 (0.0);
	RCMaterial material = rcMaterials[hit.material];
	vec3 result = material.emission.rgb * material.emission.a;
	if ((material.layers_flags.z & RC_MATERIAL_EMISSION_TEXTURE) != 0u && material.layers_flags.y < rcLighting.counts.z)
	{
		vec4 texel = RC_AtlasNearest (hit.uv, material.layers_flags.y, material.emission_rect);
		result *= texel.rgb * texel.a;
	}
	return max (result, vec3 (0.0));
}

bool RC_AcceptHit (RCSceneHit hit)
{
	/* Sky is a terminal environment hit; otherwise a ray could continue into
	 * unrelated geometry behind the sky boundary. Liquids remain transparent. */
	if ((hit.flags & RC_SCENE_SKY) != 0u) return true;
	if ((hit.flags & RC_SCENE_LIQUID) != 0u) return false;
	if ((hit.flags & RC_SCENE_CUTOUT) != 0u) return RC_MaterialAlbedo (hit).a >= 0.666;
	return true;
}

/* Ray direction points away from the shaded point toward the light. */
bool RC_LightVisible (vec3 position, vec3 normal, vec3 direction, float distance, bool directional)
{
	RCSceneHit blocker;
	vec3 start = position + normal * 0.05;
	if (!directional)
		return !RC_SceneOccluded (start, direction, 0.01, max (0.01, distance - 0.1));
	float reach = max (0.01, distance - 0.1);
	if (!RC_SceneTrace (start, direction, 0.01, reach, blocker)) return !RC_SceneBatchOccluded (start, direction, 0.01, reach);
	/* A sky portal exposes an environment/directional source, but it is still
	 * a boundary between the receiver and an ordinary point or spot light. */
	return directional && (blocker.flags & RC_SCENE_SKY) != 0u && !RC_SceneBatchOccluded (start, direction, 0.01, blocker.distance);
}





float RC_EmissiveVisibilitySamples (RCLight light, vec3 position, vec3 normal, bool singleSample)
{
	vec3 centerDelta = light.position_radius.xyz - position;
	float centerDistance = length (centerDelta);
	if (centerDistance <= 1e-5) return 0.0;
	vec3 axis, tangent, bitangent;
	float halfWidth = max (light.area_axis.w, 0.0), halfHeight = max (light.direction_cone.w, 0.0);
	if (!RC_EmissiveBasis (light, axis, tangent, bitangent))
		return float (RC_LightVisible (position, normal, centerDelta / centerDistance, centerDistance, false));
	/* Tap a rectangle in the actual emitting plane. Move endpoints slightly toward
	 * the receiver so that the emissive polygon does not shadow itself. */
	float facing = dot (position - light.position_radius.xyz, axis) >= 0.0 ? 1.0 : -1.0;
	vec3 center = light.position_radius.xyz + axis * (0.1 * facing);
	/* Low quality keeps every source and its occlusion test, but approximates
	 * area visibility with one sample. Aim at the same closest point used by
	 * its attenuation: a large emitter's center can be hidden while the
	 * nearby part that illuminates this receiver is visible. */
	if (singleSample)
	{
		vec3 offset = position - light.position_radius.xyz;
		vec3 target = center + tangent * clamp (dot (offset, tangent), -halfWidth, halfWidth) +
			bitangent * clamp (dot (offset, bitangent), -halfHeight, halfHeight);
		vec3 delta = target - position;
		float distance = length (delta);
		return distance > 1e-5 ? float (RC_LightVisible (position, normal, delta / distance, distance, false)) : 0.0;
	}
	const vec2 taps[4] = vec2[4](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
	float visible = 0.0;
	for (uint i = 0u; i < 4u; ++i)
	{
		vec3 target = center + tangent * (halfWidth * taps[i].x) + bitangent * (halfHeight * taps[i].y);
		vec3 delta = target - position;
		float distance = length (delta);
		if (distance > 1e-5 && RC_LightVisible (position, normal, delta / distance, distance, false)) visible += 0.25;
	}
	return visible;
}

float RC_EmissiveVisibility (RCLight light, vec3 position, vec3 normal)
{
	return RC_EmissiveVisibilitySamples (light, position, normal, rcLighting.bounce_parameters.y >= 0.5);
}

/* Quake's compiled map `delay` selects the attenuation formula. `wait` is
 * stored in the upper bits of metadata.w; dynamic lights retain their usual
 * finite-range falloff. Values match GetLightValue in the map light tool,
 * with finite inverse-light bounds chosen by the CPU light grid. */




/* Compiler angle sensitivity gives exactly grazing receivers a nonzero
 * contribution. Reconstruction and derivative noise can cross zero; keep
 * that numerical neighborhood on the same side of the angular step. */

vec3 RC_EvaluateOneLight (RCLight light, vec3 position, vec3 normal)
{
	/* Brush texture emission belongs to the visible side of the BSP face.
	 * Unoriented model emitters keep their previous two-sided behavior. */
	if (light.metadata.x == RC_LIGHT_EMISSIVE && (light.metadata.z & 32u) != 0u &&
		dot (position - light.position_radius.xyz, light.direction_cone.xyz) <= 0.0)
		return vec3 (0.0);
	/* area_axis.z marks compiler-style angle and spot parameters. Emissive,
	 * dynamic, KEX and older synthetic sources retain their existing shading. */
	bool compilerLight = (light.metadata.z & 17u) == 1u && light.metadata.x != RC_LIGHT_EMISSIVE && light.area_axis.z == 1.0;
	float gain = 1.0;
	if ((rcLighting.counts.w & 4u) != 0u)
	{
		if (light.metadata.x == RC_LIGHT_EMISSIVE) gain = max (rcLighting.grid_inv_cell_size.w, 0.0);
		else if (light.metadata.x != RC_LIGHT_DIRECTIONAL) gain = max (rcLighting.grid_origin.w, 0.0);
	}
	vec3 color = max (light.color_intensity.rgb, vec3 (0.0));
	if (gain <= 0.0 || light.color_intensity.w == 0.0 ||
		(light.color_intensity.w < 0.0 && (light.metadata.z & 17u) != 1u) ||
		max (max (color.r, color.g), color.b) <= 0.0) return vec3 (0.0);
	if (RC_IsLocalMinLight (light))
	{
		vec3 delta = light.position_radius.xyz - position;
		float distance = length (delta);
		if (distance < 1e-5 || light.color_intensity.w < 0.0) return vec3 (0.0);
		return RC_LightVisible (position, normal, delta / distance, distance, false) ?
			color * (light.color_intensity.w * gain) : vec3 (0.0);
	}
	vec3 direction;
	float distance, attenuation = 1.0;
	if (light.metadata.x == RC_LIGHT_DIRECTIONAL)
	{
		direction = -light.direction_cone.xyz;
		float len = length (direction);
		if (len < 1e-6) return vec3 (0.0);
		direction /= len;
		/* The sun has no finite position. Limiting this ray to a fixed map
		 * distance leaks light through occluders farther away on large maps.
		 * Keep the endpoint finite for ray-query implementations while
		 * covering any representable Quake map geometry. */
		distance = 1e30;
	}
	else
	{
		vec3 source = light.metadata.x == RC_LIGHT_EMISSIVE ? RC_ClosestEmissionPoint (light, position) : light.position_radius.xyz;
		vec3 delta = source - position;
		/* Scale the compiler-only grazing tolerance by source distance so this
		 * early rejection agrees with the normalized cosine test below. */
		float facing = dot (normal, delta);
		if (!compilerLight && facing <= 0.0) return vec3 (0.0);
		distance = length (delta);
		if (distance < 1e-5 || (compilerLight && facing < -RC_COMPILER_GRAZING_COSINE * distance)) return vec3 (0.0);
		direction = delta / distance;
		attenuation = RC_LightFalloff (light, distance);
		if (light.metadata.x == RC_LIGHT_SPOT)
		{
			float cone = dot (-direction, light.direction_cone.xyz);
			float edge = clamp (light.direction_cone.w, -1.0, compilerLight ? 1.0 : 0.9999);
			if (compilerLight)
			{
				float inner = clamp (light.area_axis.y, edge, 1.0);
				/* ericw uses a hard cone by default; _softangle supplies a
				 * smaller inner cone with a linear transition in cosine space. */
				attenuation *= cone < edge ? 0.0 : (inner > edge && cone < inner ? (cone - edge) / (inner - edge) : 1.0);
			}
			else if ((light.metadata.z & 16u) != 0u)
				/* KEX ComputeSpotLight fades linearly in cosine space from
				 * the cone edge to its axis. */
				attenuation *= clamp ((cone - edge) / (1.0 - edge), 0.0, 1.0);
			else attenuation *= smoothstep (edge, mix (edge, 1.0, 0.25), cone);
		}
	}
	float rawCosine = dot (normal, direction);
	if (rawCosine < (compilerLight ? -RC_COMPILER_GRAZING_COSINE : 0.0) ||
		(rawCosine == 0.0 && !compilerLight) || attenuation <= 0.0) return vec3 (0.0);
	float cosine = max (rawCosine, 0.0);
	if (compilerLight) cosine = mix (1.0, cosine, clamp (light.area_axis.x, 0.0, 1.0));
	float visibility = light.metadata.x == RC_LIGHT_EMISSIVE ? RC_EmissiveVisibility (light, position, normal) :
		float (RC_LightVisible (position, normal, direction, distance, light.metadata.x == RC_LIGHT_DIRECTIONAL));
	if (visibility <= 0.0) return vec3 (0.0);
	return color * (light.color_intensity.w * cosine * attenuation * gain * visibility);
}

#if RC_RECEIVER_SHADOW_EXPORT || RC_RECEIVER_SOFT_POST
#include "radiance_light_unshadowed.glsl"
#include "radiance_receiver_shadow_source.glsl"
#endif
#if RC_RECEIVER_SHADOW_EXPORT
#include "radiance_receiver_shadow_metadata.glsl"
#endif
#if RC_RECEIVER_SOFT_POST
#include "radiance_receiver_shadows.glsl"
#endif

/* Keep signed additive light separate from the local minimum, which must be
 * applied after indirect lighting. Both outputs are in irradiance units. */
void RC_EvaluateDirectComponents (vec3 position, vec3 normal, out vec3 direct, out vec3 minimum)
{
#if RC_RECEIVER_SHADOW_EXPORT
    RC_ReceiverShadowReset();
#endif
	direct = vec3 (0.0);
	minimum = vec3 (0.0);
	vec3 cell_float = floor ((position - rcLighting.grid_origin.xyz) * rcLighting.grid_inv_cell_size.xyz);
	bool use_grid = (rcLighting.counts.w & 1u) != 0u && all (greaterThanEqual (cell_float, vec3 (0.0))) &&
		all (lessThan (cell_float, vec3 (rcLighting.grid_dimensions.xyz)));
	if (use_grid)
	{
		uvec3 cell = uvec3 (cell_float);
		uint index = (cell.z * rcLighting.grid_dimensions.y + cell.y) * rcLighting.grid_dimensions.x + cell.x;
		uint first = rcLightGridOffsets[index], end = rcLightGridOffsets[index + 1u];
		if (first <= end && end <= rcLighting.grid_dimensions.w)
		{
			for (uint j = first; j < end; ++j)
			{
				uint light_index = rcLightGridIndices[j];
				if (light_index < rcLighting.counts.x)
				{
					RCLight light = rcLights[light_index];
					vec3 value = RC_EvaluateOneLight (light, position, normal);
#if RC_RECEIVER_SHADOW_EXPORT
                    RC_ReceiverShadowSelect(light_index,light,position,normal,value);
#endif
					if (RC_IsLocalMinLight (light)) minimum = max (minimum, value);
					else direct += value;
				}
			}
			direct *= 0.3183098861837907;
			minimum *= 0.3183098861837907;
			return;
		}
	}
	/* Invalid/unavailable grids and hits outside the grid keep all sources. */
	for (uint i = 0u; i < rcLighting.counts.x; ++i)
	{
		RCLight light = rcLights[i];
		vec3 value = RC_EvaluateOneLight (light, position, normal);
#if RC_RECEIVER_SHADOW_EXPORT
        RC_ReceiverShadowSelect(i,light,position,normal,value);
#endif
		if (RC_IsLocalMinLight (light)) minimum = max (minimum, value);
		else direct += value;
	}
	direct *= 0.3183098861837907;
	minimum *= 0.3183098861837907;
}

/* `indirect` is in receiver irradiance units. Apply local minimums after all
 * additive and subtractive sources, so the result does not depend on source
 * order and a minimum never increases already brighter lighting. */
vec3 RC_CombineDirectIndirect (vec3 direct, vec3 minimum, vec3 indirect)
{
	return max (max (direct + indirect, minimum), vec3 (0.0));
}

vec3 RC_EvaluateLighting (vec3 position, vec3 normal, vec3 indirect)
{
	vec3 direct, minimum;
	RC_EvaluateDirectComponents (position, normal, direct, minimum);
	return RC_CombineDirectIndirect (direct, minimum, indirect);
}

/* Compatibility helper for direct-only callers and tests. */
vec3 RC_EvaluateDirect (vec3 position, vec3 normal)
{
	return RC_EvaluateLighting (position, normal, vec3 (0.0)) * 3.141592653589793;
}

vec3 RC_SkyRadiance (vec3 ray_direction)
{
	vec3 upper = max (rcLighting.sky_radiance.rgb * rcLighting.sky_radiance.a, vec3 (0.0));
	vec3 lower = max (rcLighting.ground_radiance.rgb * rcLighting.ground_radiance.a, vec3 (0.0));
	return mix (lower, upper, smoothstep (-0.05, 0.05, ray_direction.z));
}

vec3 RC_ShadeHit (RCSceneHit hit, vec3 ray_direction)
{
	if ((hit.flags & RC_SCENE_SKY) != 0u)
		return RC_SkyRadiance (ray_direction);
	vec3 normal = dot (hit.normal, ray_direction) > 0.0 ? -hit.normal : hit.normal;
	vec3 diffuse = clamp (RC_MaterialAlbedo (hit).rgb, vec3 (0.0), vec3 (1.0));
	vec3 indirect = vec3 (0.0);
#if RC_INDIRECT_BOUNCE
	/* Trace reads the previous merged field while writing raw intervals; the
	 * dispatch barrier precedes the merge. Repeated updates therefore converge
	 * toward multi-bounce diffuse transport rather than stopping after the
	 * first reflection. Missing/invalid probes contribute zero until ready. */
	/* Bound the recursive gain below one even when the artist raises the
	 * displayed bounce gain above one. */
	float bounce_gain = min (RC_BounceGain (), 0.95);
	if (bounce_gain > 0.0 && RC_SampleDiffuse (hit.position + normal * 0.05, normal, indirect)) indirect *= bounce_gain;
#endif
	vec3 incoming = RC_EvaluateLighting (hit.position, normal, indirect);
	/* Complete proxy coverage supplies direct emission separately; preserve
	 * hit emission for any material with omitted proxies. */
	bool backFace = (hit.flags & RC_SCENE_ORIENTED) != 0u && dot (hit.normal, ray_direction) >= 0.0;
	vec3 emission = (hit.flags & RC_SCENE_EMISSIVE_PROXY) != 0u || backFace ? vec3 (0.0) : RC_MaterialEmission (hit);
	/* Proxy-covered emitters receive this gain in direct-light evaluation.
	 * Omitted surfaces still emit through traced hits and must use the same
	 * control, including when emission is disabled. */
	if ((rcLighting.counts.w & 4u) != 0u) emission *= max (rcLighting.grid_inv_cell_size.w, 0.0);
	return emission + diffuse * incoming;
}

#endif
