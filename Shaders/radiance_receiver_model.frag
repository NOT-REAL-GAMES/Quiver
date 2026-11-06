#version 460
#extension GL_GOOGLE_include_directive : require
/* Match the existing Radiance alias/MD3/MD5 vertices and alpha coverage.
 * Skinning, pose interpolation, skin selection and the actual GPU model matrix
 * are shared with the forward draw. This pass only captures receiver guides. */
#ifndef ALIAS_ALPHA_TEST
#define ALIAS_ALPHA_TEST 0
#endif
layout(set = 0, binding = 0) uniform sampler2D diffuse_tex;
layout(push_constant) uniform ModelReceiver {
    layout(offset = 80) uint receiver_token;
} push_constants;
layout(location = 0) in vec2 in_texcoord;
layout(location = 3) in vec3 in_world_position;
layout(location = 4) in vec3 in_world_normal;
layout(location = 0) out vec4 outNormal;
layout(location = 1) out uvec2 outID;
#include "radiance_model_normal.glsl"
void main()
{
#if ALIAS_ALPHA_TEST
    if (texture(diffuse_tex, in_texcoord).a < 0.666) discard;
#endif
    outNormal = vec4(RC_ModelReceiverNormal(in_world_normal, gl_FrontFacing), 0.0);
    outID = uvec2(push_constants.receiver_token, uint(gl_PrimitiveID) + 1u);
}
