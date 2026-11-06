/* Radiance receivers write into the engine's UNORM scene target. Preserve the
 * shape and hue of bright diffuse illumination before the target clips it.
 * This is a display-space approximation, not an HDR scene tone mapper. */
#ifndef RC_HIGHLIGHT_GLSL
#define RC_HIGHLIGHT_GLSL

vec3 RC_CompressHighlights (vec3 color, float contrast)
{
	/* Match the later postprocess contrast clamp, so its multiplication does
	 * not immediately clip the compressed result. Invalid/zero values arise in
	 * older test callers and must behave like contrast 1. */
	if (isnan (contrast) || isinf (contrast)) contrast = 1.0;
	contrast = clamp (contrast, 1.0, 2.0);
	float ceiling = 1.0 / contrast;
	float start = ceiling * 0.8;
	float peak = max (max (color.r, color.g), color.b);
	if (peak <= start) return color;
	float width = ceiling - start;
	float excess = peak - start;
	float mapped = start + width * (excess / (width + excess));
	return color * (mapped / peak);
}
#endif
