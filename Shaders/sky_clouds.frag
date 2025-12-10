#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	vec3  eye_pos;
	float time;
	vec3  sun_dir;
	float sun_intensity;
	float cloud_height;     // base height of cloud layer
	float cloud_thickness;  // thickness of cloud layer
	float cloud_density;    // overall opacity
	float cloud_coverage;   // how much sky is covered
	float cloud_speed;      // wind animation speed
}
push_constants;

layout (set = 0, binding = 0) uniform sampler3D noise_tex;

layout (location = 0) in vec3 in_view_dir;

layout (location = 0) out vec4 out_frag_color;

// Hash function for dithering (interleaved gradient noise)
float interleaved_gradient_noise (vec2 screen_pos)
{
	vec3 magic = vec3 (0.06711056, 0.00583715, 52.9829189);
	return fract (magic.z * fract (dot (screen_pos, magic.xy)));
}

// Ray-box intersection for cloud layer (Z-up coordinate system)
// Returns distances to near and far intersections
vec2 ray_cloud_layer_intersect (vec3 ray_origin, vec3 ray_dir, float layer_bottom, float layer_top)
{
	// Horizontal infinite slab intersection (Z is up in Quake)
	if (abs(ray_dir.z) < 0.0001)
		return vec2(-1.0); // Ray is horizontal, no intersection

	float t_bottom = (layer_bottom - ray_origin.z) / ray_dir.z;
	float t_top = (layer_top - ray_origin.z) / ray_dir.z;

	float t_near = min (t_bottom, t_top);
	float t_far = max (t_bottom, t_top);

	// Must be in front of us
	if (t_far < 0.0)
		return vec2 (-1.0);

	return vec2 (max (t_near, 0.0), t_far);
}

// Sample cloud density from 3D noise texture
float sample_cloud_density (vec3 pos, float time)
{
	// Scale position to noise texture coordinates
	vec3 uv = pos * 0.0005;  // Scale factor for cloud scale

	// Animate with wind (XY plane in Quake)
	uv.xy += time * push_constants.cloud_speed;

	// Sample base noise
	float noise = texture (noise_tex, uv).r;

	// Apply coverage threshold
	noise = smoothstep (1.0 - push_constants.cloud_coverage, 1.0, noise);

	// Height-based density falloff (clouds thicker in middle)
	// Z is up in Quake
	float layer_bottom = push_constants.cloud_height;
	float height_fraction = (pos.z - layer_bottom) / push_constants.cloud_thickness;
	float height_falloff = 4.0 * height_fraction * (1.0 - height_fraction);  // Parabola

	return noise * height_falloff * push_constants.cloud_density;
}

// Beer's law light attenuation
float beer_law (float optical_depth)
{
	return exp (-optical_depth);
}

// Ray march through cloud layer
vec4 raymarch_clouds (vec3 ray_origin, vec3 ray_dir, vec2 t_range, vec3 sun_dir, float time, float dither)
{
	const int MAX_STEPS = 48;
	const int LIGHT_STEPS = 6;

	float ray_length = t_range.y - t_range.x;
	float step_size = ray_length / float (MAX_STEPS);

	vec3 accumulated_color = vec3 (0.0);
	float accumulated_alpha = 0.0;

	// Sun color (warm for daytime)
	vec3 sun_color = vec3 (1.0, 0.95, 0.8) * push_constants.sun_intensity;

	// Dither the starting position to break up banding
	float t_offset = dither * step_size;

	for (int i = 0; i < MAX_STEPS; i++)
	{
		if (accumulated_alpha > 0.99)
			break;

		float t = t_range.x + t_offset + float (i) * step_size;
		if (t > t_range.y)
			break;

		vec3 sample_pos = ray_origin + ray_dir * t;

		float density = sample_cloud_density (sample_pos, time);

		if (density > 0.01)
		{
			// Light march towards sun
			float light_optical_depth = 0.0;
			float light_step = push_constants.cloud_thickness * 0.5 / float (LIGHT_STEPS);

			for (int j = 0; j < LIGHT_STEPS; j++)
			{
				vec3 light_pos = sample_pos + sun_dir * (float (j) + 0.5) * light_step;
				light_optical_depth += sample_cloud_density (light_pos, time) * light_step;
			}

			// Light reaching this point (Beer's law)
			float light_transmittance = beer_law (light_optical_depth * 10.0);

			// Ambient sky light from above (Z is up)
			float ambient = 0.3 + 0.2 * (sample_pos.z - push_constants.cloud_height) / push_constants.cloud_thickness;

			// Cloud color: sunlit + ambient
			vec3 cloud_color = sun_color * light_transmittance + vec3 (0.6, 0.7, 0.9) * ambient;

			// Accumulate with front-to-back compositing
			float sample_alpha = 1.0 - exp (-density * step_size * 50.0);
			accumulated_color += cloud_color * sample_alpha * (1.0 - accumulated_alpha);
			accumulated_alpha += sample_alpha * (1.0 - accumulated_alpha);
		}
	}

	return vec4 (accumulated_color, accumulated_alpha);
}

void main ()
{
	vec3 view_dir = normalize (in_view_dir);
	vec3 sun_dir = normalize (push_constants.sun_dir);

	// Only render clouds above horizon (Z is up in Quake)
	if (view_dir.z <= 0.0)
	{
		out_frag_color = vec4 (0.0);
		return;
	}

	// Ray-cloud layer intersection
	float layer_bottom = push_constants.cloud_height;
	float layer_top = push_constants.cloud_height + push_constants.cloud_thickness;

	vec3 ray_origin = push_constants.eye_pos;
	vec2 t_range = ray_cloud_layer_intersect (ray_origin, view_dir, layer_bottom, layer_top);

	if (t_range.x < 0.0)
	{
		out_frag_color = vec4 (0.0);
		return;
	}

	// Compute dither value for this pixel (temporal variation with time)
	float dither = interleaved_gradient_noise (gl_FragCoord.xy + fract (push_constants.time * 100.0));

	// Ray march clouds
	vec4 cloud_color = raymarch_clouds (ray_origin, view_dir, t_range, sun_dir, push_constants.time, dither);

	// Output with premultiplied alpha for blending
	out_frag_color = cloud_color;

	// Apply fog
	if (push_constants.fog_density > 0.0)
	{
		out_frag_color.rgb = mix (out_frag_color.rgb, push_constants.fog_color, push_constants.fog_density * out_frag_color.a);
	}
}
