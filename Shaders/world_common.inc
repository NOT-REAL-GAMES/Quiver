#if RADIANCE_LIGHTING
#include "radiance_highlight.glsl"
#endif
vec4 WorldFragmentColor ()
{
	vec4 diffuse = texture (diffuse_tex, in_texcoords.xy);
	if (use_alpha_test && diffuse.a < 0.666f)
		discard;

	vec3  light;

#if RADIANCE_LIGHTING
	/* Raster derivatives give the receiver's geometric normal in world space,
	 * including rotated/moved brush submodels and mesh/task shader paths. */
	/* Quake's lava warp texture is displayed unlit in the classic renderer.
	 * Preserve that appearance without consulting baked lighting while its
	 * fullbright pixels illuminate nearby Radiance receivers. */
	if ((push_constants.instance_base & 0x80000000u) != 0u)
		light = vec3 (1.0);
	else
	{
		vec3 normal = RC_WorldSurfaceNormal (in_world_position);
		vec3 indirect = vec3 (0.0);
		float bounce_gain = RC_BounceGain () * RC_IndirectDistanceWeight (in_world_position);
		if (bounce_gain > 0.0) {
#if RC_RECEIVER_REUSE
			if (!RC_ReceiverIndirect (ivec2(gl_FragCoord.xy), uvec2(push_constants.receiver_token, uint(gl_PrimitiveID) + 1u), in_world_position, normal, indirect))
#endif
#if RC_RECEIVER_TEXTURE_FILTER && !RC_RECEIVER_GAUSSIAN_FALLBACK
				RC_SampleDiffuse (in_world_position, normal, indirect);
#else
				RC_SampleDiffuseMode (in_world_position, normal, rcLighting.receiver_filter.x, indirect);
#endif
		}
		vec3 direct, minimum;
#if RC_RECEIVER_REUSE
		if (!RC_ReceiverDirect (ivec2(gl_FragCoord.xy), uvec2(push_constants.receiver_token, uint(gl_PrimitiveID) + 1u), in_world_position, normal, direct, minimum))
#endif
			RC_EvaluateDirectComponents (in_world_position, normal, direct, minimum);
		light = RC_CombineDirectIndirect (direct, minimum, indirect * bounce_gain);
	}
	/* Renderer sets this only for the single-player fullbright debug mode. */
	if ((rcLighting.counts.w & 2u) != 0u) light = vec3 (1.0);
#else
	float lm_multiplier = scaled_lm ? 8.0f : 2.0f;
	if (quantize_lm)
	{
		ivec2 lm_size = ivec2 (LMBLOCK_WIDTH, LMBLOCK_HEIGHT);
		vec2  uv_exp = (floor ((lm_size * 16) * in_texcoords.zw) + 0.5) / (lm_size * 16);
		light = texture (lightmap_tex, uv_exp).rgb * lm_multiplier;
	}
	else
		light = texture (lightmap_tex, in_texcoords.zw).rgb * lm_multiplier;
#endif

	vec4 frag_color;
#if RADIANCE_LIGHTING
	frag_color.a = 1.0;
#endif
	frag_color.rgb = diffuse.rgb * light.rgb;

#if RADIANCE_LIGHTING
	/* Keep fullbright debug and classic lighting exactly on their old path. */
	if ((push_constants.instance_base & 0x80000000u) == 0u &&
		(rcLighting.counts.w & 2u) == 0u && rcLighting.bounce_parameters.w > 0.0 && rcLighting.bounce_parameters.w < 1.5)
		frag_color.rgb = RC_CompressHighlights (frag_color.rgb, rcLighting.bounce_parameters.z);
#endif

	if (use_fullbright)
	{
		vec3 fullbright = texture (fullbright_tex, in_texcoords.xy).rgb;
		frag_color.rgb += fullbright;
	}

	float fog = exp (-push_constants.fog_density * push_constants.fog_density * in_fog_frag_coord * in_fog_frag_coord);
	fog = clamp (fog, 0.0, 1.0);
	frag_color.rgb = mix (push_constants.fog_color, frag_color.rgb, fog);

	if (use_alpha_blend)
		frag_color.a = push_constants.alpha;

	return frag_color;
}
