#version 460
#extension GL_GOOGLE_include_directive : require
layout(std430, set = 0, binding = 0) buffer Results { vec4 value; } output_data;
#include "radiance_highlight.glsl"
void main ()
{
	bool ok = true;
	vec3 dim = vec3(0.10, 0.20, 0.30);
	ok = ok && all(equal(RC_CompressHighlights(dim, 1.4), dim));
	for (int c = 1; c <= 2; ++c)
	{
		float contrast = float(c);
		float ceiling = 1.0 / contrast;
		float start = 0.8 * ceiling;
		vec3 at = RC_CompressHighlights(vec3(start), contrast);
		vec3 justAbove = RC_CompressHighlights(vec3(start + 0.0001), contrast);
		ok = ok && abs(at.x - start) < 0.00001;
		ok = ok && justAbove.x > at.x && justAbove.x < start + 0.0001;
		vec3 nearLight = RC_CompressHighlights(vec3(1.5, 0.75, 0.375), contrast);
		vec3 strongLight = RC_CompressHighlights(vec3(4.0, 2.0, 1.0), contrast);
		ok = ok && nearLight.x < ceiling && strongLight.x < ceiling && strongLight.x > nearLight.x;
		ok = ok && abs(nearLight.y / nearLight.x - 0.5) < 0.00001;
		ok = ok && abs(nearLight.z / nearLight.x - 0.25) < 0.00001;
	}
	/* Reuse the world-normal test's readback contract. */
	output_data.value = ok ? vec4(-1, 0, 0, 1) : vec4(1, 0, 0, 0);
}
