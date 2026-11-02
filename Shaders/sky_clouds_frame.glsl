/* SPDX-License-Identifier: GPL-2.0-or-later */
layout (std140, set = 2, binding = 0) uniform CloudFrame
{
	vec4 viewport;	 // full-resolution origin and extent in framebuffer pixels
	vec4 target;	 // active reduced extent, resolution divisor, linear-filter flag
	vec4 far_target; // far extent, divisor and filter; target remains near/baseline
	vec4 params;	 // eye-distance split, cascades enabled, diagnostic band, trace band
}
cloud_frame;
