#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	vec3  sun_dir;
	float sun_intensity;
	vec4  world_phase;
	vec4  curvature;
}
push_constants;

layout (set = 0, binding = 0) uniform sampler2D grass_tex;
layout (set = 1, binding = 0) uniform sampler2D soil_tex;
layout (set = 2, binding = 0) uniform sampler2D rock_tex;
layout (set = 3, binding = 0) uniform sampler2D sand_tex;

layout (location = 0) in vec4 in_material_weights;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec3 in_position;
layout (location = 3) in float in_fog_frag_coord;
layout (location = 4) in vec4 in_material_tint;
layout (location = 0) out vec4 out_frag_color;

float hash (vec2 cell, float period)
{
	// The vertex shader adds the canonical chunk phase modulo 8192 units.
	// Periodic cells keep blends continuous across chunks and floating origins.
	uvec2 p = uvec2 (mod (cell, period));
	uint  h = p.x * 1597334677u ^ p.y * 3812015801u;
	h = (h ^ (h >> 16)) * 2246822519u;
	return float (h ^ (h >> 13)) * (1.0 / 4294967295.0);
}

float surface_noise (vec2 position, float scale)
{
	vec2 p = position / scale;
	vec2 cell = floor (p);
	vec2 f = fract (p);
	f = f * f * (3.0 - 2.0 * f);
	float period = 8192.0 / scale;
	return mix (
		mix (hash (cell, period), hash (cell + vec2 (1, 0), period), f.x), mix (hash (cell + vec2 (0, 1), period), hash (cell + vec2 (1, 1), period), f.x),
		f.y);
}

float filtered_noise (vec2 position, float scale, float footprint)
{
	return mix (surface_noise (position, scale), 0.5, smoothstep (scale * 0.5, scale * 2.0, footprint));
}

vec3 sample_material (sampler2D material, vec3 position, vec3 dx, vec3 dy, vec3 projection)
{
	// Explicit gradients remain valid inside these branches. Most ground needs
	// only the XY projection; cliffs blend into the two vertical projections.
	vec3 color = vec3 (0.0);
	if (projection.x > 0.0)
		color += textureGrad (material, position.yz, dx.yz, dy.yz).rgb * projection.x;
	if (projection.y > 0.0)
		color += textureGrad (material, position.xz, dx.xz, dy.xz).rgb * projection.y;
	if (projection.z > 0.0)
		color += textureGrad (material, position.xy, dx.xy, dy.xy).rgb * projection.z;
	return color;
}

void main ()
{
	vec3 normal = normalize (in_normal);
	float sky = clamp (normal.z * 0.5 + 0.5, 0.0, 1.0);
	vec3  ambient = mix (vec3 (0.18, 0.16, 0.14), vec3 (0.32, 0.37, 0.44), sky);
	float daylight = smoothstep (-0.08, 0.12, push_constants.sun_dir.z);
	float diffuse = max (dot (normal, push_constants.sun_dir), 0.0);
	float sunlight = 1.35 * (1.0 - exp (-max (push_constants.sun_intensity, 0.0)));
	vec3  light = ambient + vec3 (1.0, 0.94, 0.82) * diffuse * sunlight * daylight;
	float fog = clamp (exp (-push_constants.fog_density * push_constants.fog_density * in_fog_frag_coord * in_fog_frag_coord), 0.0, 1.0);
	// r_lightmap displays lighting alone. Streamed terrain uses ambient and sun
	// in place of a baked BSP lightmap. Fog still applies in either debug mode.
	if (push_constants.curvature.w == 2.0)
	{
		out_frag_color = vec4 (mix (push_constants.fog_color, light, fog), 1.0);
		return;
	}
	vec3 projection = max (abs (normal) - 0.15, 0.0);
	projection *= projection;
	projection *= projection;
	projection /= max (dot (projection, vec3 (1.0)), 0.0001);
	// Prune projections whose contribution would be below one 8-bit color step.
	// The subtraction makes their appearance continuous at the threshold.
	projection = max (projection - 0.004, 0.0);
	projection /= max (dot (projection, vec3 (1.0)), 0.0001);
	vec4 weights = max (in_material_weights, 0.0);
	weights /= max (dot (weights, vec4 (1.0)), 0.0001);
	float footprint = max (length (dFdx (in_position.xy)), length (dFdy (in_position.xy)));
	// Generated grass and soil share a continuous cover field instead of their
	// former quantized voxel-height IDs. Evaluate it per fragment so coarse LOD
	// triangles retain exactly the same patch boundaries as detailed terrain.
	float cover_noise = 0.68 * filtered_noise (in_position.xy, 512.0, footprint) + 0.32 * filtered_noise (in_position.xy, 128.0, footprint);
	float grass = smoothstep (0.36, 0.64, cover_noise);
	float rock = clamp (1.0 - smoothstep (0.50, 0.86, normal.z) + 0.35 * smoothstep (0.67, 0.83, filtered_noise (in_position.xy, 1024.0, footprint)), 0.0, 1.0);
	float cover = weights.x;
	weights.x = cover * (1.0 - rock) * grass;
	weights.y += cover * (1.0 - rock) * (1.0 - grass);
	weights.z += cover * rock;
	// All repeats divide the 1024-unit chunk size. Phased local coordinates are
	// therefore world anchored, including distant tiles and floating origins.
	// Repeat sampling and full mip chains avoid atlas bleeding at the horizon.
	vec3 position = in_position / 64.0;
	vec3 dx = dFdx (position), dy = dFdy (position);
	vec3 albedo = vec3 (0.0);
	if (weights.x > 0.0)
		albedo += weights.x * sample_material (grass_tex, position, dx, dy, projection);
	if (weights.y > 0.0)
		albedo += weights.y * sample_material (soil_tex, position, dx, dy, projection);
	if (weights.z > 0.0)
		albedo += weights.z * sample_material (rock_tex, position * 0.5, dx * 0.5, dy * 0.5, projection);
	if (weights.w > 0.0)
		albedo += weights.w * sample_material (sand_tex, position * 0.5, dx * 0.5, dy * 0.5, projection);
	// Preserve explicit non-ground materials without discrete triangle colors
	// leaking into the procedural grass/soil/rock/sand classification.
	float grain = dot (albedo, vec3 (0.299, 0.587, 0.114));
	albedo = albedo * (1.0 - in_material_tint.a) + in_material_tint.rgb * (0.85 + 0.3 * grain);
	// Low-frequency variation breaks up visible texture repeats. Fade before
	// it becomes subpixel; the textures themselves use derivative-selected mips.
	float broad = filtered_noise (in_position.xy, 256.0, footprint) - 0.5;
	albedo *= 1.0 + 0.18 * broad;
	// Static sea surface shares terrain depth/occlusion. A separate index range
	// keeps it visible when a distant ground tile is replaced by nearby detail.
	if (push_constants.world_phase.w > 0.5)
		albedo = vec3 (0.035, 0.16, 0.23);

	vec3 color = push_constants.curvature.w == 1.0 ? albedo : albedo * light; // r_fullbright
	out_frag_color = vec4 (mix (push_constants.fog_color, color, fog), 1.0);
}
