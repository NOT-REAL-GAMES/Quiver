#version 460
/* Standalone prototype. A production producer must feed the same transformed
 * positions and interpolated normals as each existing renderer path. */
layout(set = 0, binding = 0) uniform Transforms
{
	mat4 modelViewProjection;
	mat4 modelToWorld;
	mat4 normalToWorld;
} transform;
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord;
/* Supplied as a per-instance attribute; unique among all draws in this frame. */
layout(location = 3) in uint receiverToken;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 uv;
layout(location = 3) flat out uint token;
void main()
{
	gl_Position = transform.modelViewProjection * vec4(position, 1);
	worldPosition = (transform.modelToWorld * vec4(position, 1)).xyz;
	worldNormal = (transform.normalToWorld * vec4(normal, 0)).xyz;
	uv = texcoord;
	token = receiverToken;
}
