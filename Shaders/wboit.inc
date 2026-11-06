layout (location = 0) out vec4 out_oit_accum;
layout (location = 1) out float out_oit_reveal;

void WBOITWrite (vec4 frag_color)
{
#if RADIANCE_LIGHTING
	if (rcLighting.bounce_parameters.w >= 1.5) { frag_color.rgb = max (frag_color.rgb, vec3 (0.0)); frag_color.a = clamp (frag_color.a, 0.0, 1.0); }
	else
#endif
	frag_color = clamp (frag_color, 0.0f, 1.0f);
	float z = 1.0f / gl_FragCoord.w;
	float weight = sqrt (0.01f + 0.99f * frag_color.a / (1.0f + z / 25.0f));
	out_oit_accum = vec4 (frag_color.rgb * frag_color.a * weight, frag_color.a * weight);
	out_oit_reveal = frag_color.a;
}
