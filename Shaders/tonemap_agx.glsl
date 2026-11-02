// SPDX-License-Identifier: MIT
// Adapted from Benjamin Wrensch's minimal implementation of Troy Sobotka's AgX:
// https://iolite-engine.com/blog_posts/minimal_agx_implementation
//
// Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef QUIVER_TONEMAP_AGX_GLSL
#define QUIVER_TONEMAP_AGX_GLSL

// Linear BT.709/sRGB radiance -> display-encoded RGB, neutral AgX look.
// These GLSL constructors contain columns, not rows. The scene attachment is
// UNORM, so leave the output encoded; applying the reference's final EOTF here
// would darken it, while another gamma encoding would brighten it twice.
vec3 AgX_Display (vec3 radiance)
{
	const mat3 inset = mat3 (
		0.842479062253094, 0.0423282422610123, 0.0423756549057051,
		0.0784335999999992, 0.878468636469772, 0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const mat3 outset = mat3 (
		1.19687900512017, -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	const float MIN_EV = -12.47393;
	const float MAX_EV = 4.026069;

	vec3 value = max (inset * max (radiance, vec3 (0.0)), vec3 (1e-10));
	vec3 x = (clamp (log2 (value), MIN_EV, MAX_EV) - MIN_EV) / (MAX_EV - MIN_EV);
	vec3 x2 = x * x;
	vec3 x4 = x2 * x2;
	vec3 sigmoid = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
	return clamp (outset * sigmoid, 0.0, 1.0);
}

vec3 AgX_ForScene (vec3 radiance, float display_scale)
{
	// Apply the player's contrast boost before the shoulder, then reserve its
	// reciprocal in the LDR buffer. The existing final contrast pass therefore
	// cannot clip away AgX's highlight roll-off. User gamma still applies there.
	display_scale = clamp (display_scale, 0.49, 1.0);
	return AgX_Display (radiance / display_scale) * display_scale;
}

#endif
