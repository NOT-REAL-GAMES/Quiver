#ifndef ALIAS_ALPHA_TEST
#define ALIAS_ALPHA_TEST 0
#endif
#if RADIANCE_LIGHTING
#include "radiance_highlight.glsl"
#include "radiance_model_normal.glsl"
#endif

vec4 AliasFragmentColor ()
{
	vec4 result = texture (diffuse_tex, in_texcoord.xy);

	/* Host sets this only for native blended draws without depth writes.
	 * Zero output contributes neither color nor depth and avoids adding a
	 * discard instruction to the ordinary opaque shader variant. */
	if ((ubo.flags & 0x20u) != 0u && result.a == 0.0)
		return vec4(0.0);

#if ALIAS_ALPHA_TEST
	if (result.a < 0.666f)
		discard;
#endif

#if RC_RECEIVER_MODEL_CULL
	/* The shared prepass already knows a nearer opaque surface. Do not run
	 * costly full-fragment fallback for this hidden earlier model surface. */
	if (RC_ReceiverOccluded (ivec2(gl_FragCoord.xy), uvec2(push_constants.receiver_token, uint(gl_PrimitiveID) + 1u), gl_FragCoord.z))
		discard;
#endif

	float original_diffuse_tex_a = result.a;
	result.a = 1.0;

#if RADIANCE_LIGHTING
	if ((ubo.flags & 0x2u) != 0u)
		result *= in_color * 2.0f; /* fullbright debug behavior */
	else
	{
		vec3 normal = RC_ModelReceiverNormal (in_world_normal, gl_FrontFacing);
		vec3 indirect = vec3 (0.0);
		float bounce_gain = RC_BounceGain () * RC_IndirectDistanceWeight (in_world_position);
#if RC_RECEIVER_FULL_LIGHT
		vec3 direct, minimum;
		RC_ReceiverFullLight (ivec2(gl_FragCoord.xy), indirect, direct, minimum);
#else
#if RC_RECEIVER_REUSE
		vec3 direct, minimum;
		bool reused_indirect, reused_direct;
		RC_ReceiverLighting (ivec2(gl_FragCoord.xy), uvec2(push_constants.receiver_token, uint(gl_PrimitiveID) + 1u), in_world_position, normal,
			bounce_gain > 0.0, indirect, reused_indirect, direct, minimum, reused_direct);
#endif
		if (bounce_gain > 0.0)
		{
#if RC_RECEIVER_REUSE
			if (!reused_indirect && RC_NoteFallback (1u))
#endif
				RC_SampleDiffuseMode (RC_GI_LOOKUP_POINT (in_world_position, normal), normal, rcLighting.receiver_filter.x, indirect);
		}
#if RC_RECEIVER_REUSE
		if (!reused_direct && RC_NoteFallback (2u))
			RC_EvaluateDirectComponents (in_world_position, normal, direct, minimum);
#endif
#endif
#if RC_RECEIVER_REUSE
		vec3 light = RC_CombineDirectIndirect (direct, minimum, indirect * bounce_gain);
		/* Debug view: green reused, red direct missed, blue indirect missed, magenta both. */
		if ((rcLighting.counts.w & 64u) != 0u)
			return vec4 ((rcFallbackMask & 2u) != 0u ? 1.0 : 0.0, rcFallbackMask == 0u ? 1.0 : 0.0, (rcFallbackMask & 1u) != 0u ? 1.0 : 0.0, 1.0);
#else
		vec3 light = RC_EvaluateLighting (in_world_position, normal, indirect * bounce_gain);
#endif
		/* Quake's viewmodel/player minimums keep them visible in dark areas. */
		float minimum_sum = ((ubo.flags & 0x8u) != 0u) ? 0.72 : (((ubo.flags & 0x10u) != 0u) ? 0.24 : 0.0);
		float deficit = max (0.0, minimum_sum - (light.r + light.g + light.b));
		light += vec3 (deficit / 3.0);
		result.rgb *= light;
		if (rcLighting.bounce_parameters.w > 0.0 && rcLighting.bounce_parameters.w < 1.5)
			result.rgb = RC_CompressHighlights (result.rgb, rcLighting.bounce_parameters.z);
	}
#else
	result *= in_color * 2.0f;
#endif

	if ((ubo.flags & 0x1) != 0)
		result += texture (fullbright_tex, in_texcoord.xy);

	result.a = original_diffuse_tex_a * ubo.entalpha;

	float fog = exp (-push_constants.fog_density * push_constants.fog_density * in_fog_frag_coord * in_fog_frag_coord);
	fog = clamp (fog, 0.0, 1.0);
	result.rgb = mix (push_constants.fog_color, result.rgb, fog);

	return result;
}
