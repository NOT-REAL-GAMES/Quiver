#version 460
#extension GL_GOOGLE_include_directive : require
/* Receiver companion for world.vert compiled with RADIANCE_LIGHTING=1.
 * The world vertex transform, camera descriptor, raster state and alpha
 * threshold must match the forward draw. Each direct indexed draw gets a
 * nonzero, frame-unique token in fog_color.x bits; PrimitiveID is local to
 * that draw. Only non-instanced direct opaque draws may use this contract. */
layout(push_constant) uniform PushConsts {
	mat4 mvp;
	vec3 fog_color;
	float fog_density;
	float alpha;
	uint instance_base;
} push_constants;
layout(set = 0, binding = 0) uniform sampler2D diffuse_tex;
layout(location = 0) in vec4 in_texcoords;
layout(location = 2) in vec3 in_world_position;
layout(constant_id = 1) const bool use_alpha_test = false;
#include "radiance_world_normal.glsl"
layout(location = 0) out vec4 outNormal;
layout(location = 1) out uvec2 outID;
void main() {
	if (use_alpha_test && texture(diffuse_tex, in_texcoords.xy).a < 0.666f) discard;
	outNormal = vec4(RC_WorldSurfaceNormal(in_world_position), 0.0);
	outID = uvec2(floatBitsToUint(push_constants.fog_color.x), uint(gl_PrimitiveID) + 1u);
}
