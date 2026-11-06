#version 460
/* Alpha cutouts must match the forward pipeline. This shader does not handle
 * liquids, blended geometry, or viewmodels, which keep the full-res path. */
layout(set = 0, binding = 1) uniform sampler2D diffuseTexture;
layout(push_constant) uniform Options { uint alphaTest; uint geometricNormal; } options;
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 uv;
layout(location = 3) flat in uint token;
layout(location = 0) out vec4 outNormal;
layout(location = 1) out uvec2 outID;
void main()
{
	vec3 n = options.geometricNormal != 0u ? cross(dFdy(worldPosition), dFdx(worldPosition)) : worldNormal;
	if (options.alphaTest != 0u && texture(diffuseTexture, uv).a < 0.666) discard;
	if (options.geometricNormal == 0u && !gl_FrontFacing) n = -n;
	float len = length(n);
	outNormal = vec4(len > 1e-10 ? n / len : vec3(0,0,1), 0);
	/* Zero is reserved for cleared background. */
	outID = uvec2(token, uint(gl_PrimitiveID) + 1u);
}
