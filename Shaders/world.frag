#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable
#if RADIANCE_LIGHTING && defined(RC_HARDWARE_RAY_QUERY)
#extension GL_EXT_ray_query : require
#endif

// Compiled in multiple variants: WBOIT writes weighted blended OIT accumulation,
// MBOIT writes power moments, MBOIT + MBOIT_COMPOSITE reconstructs transmittance
// from them and MSAA reads multisampled moment buffers

// keep in sync with glquake.h
#define LMBLOCK_WIDTH  1024
#define LMBLOCK_HEIGHT 1024

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	float alpha;
#if RADIANCE_LIGHTING
	uint  instance_base;
#endif
#if RC_RECEIVER_REUSE
	uint  receiver_token;
#endif
}
push_constants;

layout (set = 0, binding = 0) uniform sampler2D diffuse_tex;
layout (set = 1, binding = 0) uniform sampler2D lightmap_tex;
layout (set = 2, binding = 0) uniform sampler2D fullbright_tex;

layout (location = 0) in vec4 in_texcoords;
layout (location = 1) in float in_fog_frag_coord;
#if RADIANCE_LIGHTING
layout (location = 2) in vec3 in_world_position;
#define RC_SET 5
#define RC_SCENE_SET 5
#define RC_SCENE_BINDING_BASE 9
#define RC_LIGHT_SET 5
#define RC_LIGHT_BINDING_BASE 14
#define RC_RECEIVER_READONLY 1
#include "radiance_cascades.glsl"
#include "radiance_lookup.glsl"
#include "radiance_scene_trace.glsl"
#include "radiance_lighting.glsl"
#include "radiance_world_normal.glsl"
#if RC_RECEIVER_REUSE
#include "radiance_receiver_resolve.glsl"
#endif
#endif

layout (constant_id = 0) const bool use_fullbright = false;
layout (constant_id = 1) const bool use_alpha_test = false;
layout (constant_id = 2) const bool use_alpha_blend = false;
layout (constant_id = 3) const bool quantize_lm = false;
layout (constant_id = 4) const bool scaled_lm = false;

#include "world_common.inc"

#if MBOIT
#define MBOIT_INPUT_SET 3
#include "mboit.inc"
#elif WBOIT
#include "wboit.inc"
#else
layout (location = 0) out vec4 out_frag_color;
#endif

void main ()
{
#if MBOIT
	MBOITWrite (WorldFragmentColor ());
#elif WBOIT
	WBOITWrite (WorldFragmentColor ());
#else
	out_frag_color = WorldFragmentColor ();
#endif
}
