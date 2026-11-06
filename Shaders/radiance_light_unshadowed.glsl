#ifndef RC_LIGHT_UNSHADOWED_GLSL
#define RC_LIGHT_UNSHADOWED_GLSL
/* Query-free receiver scoring/delta math. Keep attenuation and cosine rules
 * aligned with RC_EvaluateOneLight; this file must never trace visibility. */
vec3 RC_EvaluateUnshadowedLight (RCLight light, vec3 position, vec3 normal,
    out vec3 receiverDirection, out float receiverDistance)
{
    receiverDirection=vec3(0); receiverDistance=0.0;
    if(RC_IsLocalMinLight(light)) return vec3(0);
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
    receiverDirection=direction; receiverDistance=distance;
    return color * (light.color_intensity.w * cosine * attenuation * gain);
}
#endif
