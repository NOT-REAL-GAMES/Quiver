// Shared by the direct reference path and cached unit-radiance sky.
// Atmospheric scattering constants
const float EARTH_RADIUS = 6371000.0;       // meters
const float ATMOSPHERE_HEIGHT = 100000.0;   // meters
const float ATMOSPHERE_RADIUS = EARTH_RADIUS + ATMOSPHERE_HEIGHT;

// Scattering coefficients (at sea level)
const vec3 RAYLEIGH_COEFF = vec3 (5.8e-6, 13.5e-6, 33.1e-6);  // wavelength-dependent
const float MIE_COEFF = 21e-6;

// Scale heights
const float RAYLEIGH_SCALE_HEIGHT = 8500.0;   // meters
const float MIE_SCALE_HEIGHT = 1200.0;        // meters

// Mie scattering parameters
const float MIE_G = 0.76;  // asymmetry factor (forward scattering)

// Number of ray march samples
const int PRIMARY_STEPS = 16;
const int LIGHT_STEPS = 4;

// Ray-sphere intersection
// Returns distances to near and far intersections, or -1 if no intersection
vec2 ray_sphere_intersect (vec3 ray_origin, vec3 ray_dir, float sphere_radius)
{
	float b = dot (ray_origin, ray_dir);
	float c = dot (ray_origin, ray_origin) - sphere_radius * sphere_radius;
	float d = b * b - c;

	if (d < 0.0)
		return vec2 (-1.0);

	d = sqrt (d);
	return vec2 (-b - d, -b + d);
}

// Rayleigh phase function
float rayleigh_phase (float cos_theta)
{
	return (3.0 / (16.0 * 3.14159265)) * (1.0 + cos_theta * cos_theta);
}

// Henyey-Greenstein phase function for Mie scattering
float mie_phase (float cos_theta, float g)
{
	float g2 = g * g;
	float denom = 1.0 + g2 - 2.0 * g * cos_theta;
	return (3.0 / (8.0 * 3.14159265)) * ((1.0 - g2) * (1.0 + cos_theta * cos_theta)) / ((2.0 + g2) * pow (denom, 1.5));
}

// Density at height for Rayleigh/Mie
float density_at_height (float height, float scale_height)
{
	return exp (-max(height, 0.0) / scale_height);
}

// Main atmospheric scattering computation
// ray_dir: normalized view direction (Y = up in atmosphere space)
// sun_dir: normalized sun direction (Y = up in atmosphere space)
vec3 compute_atmosphere (vec3 ray_dir, vec3 sun_dir)
{
	// Camera position: assume we're at sea level looking up
	vec3 ray_origin = vec3 (0.0, EARTH_RADIUS + 100.0, 0.0);

	// Intersect with atmosphere
	vec2 atmos_intersect = ray_sphere_intersect (ray_origin, ray_dir, ATMOSPHERE_RADIUS);

	if (atmos_intersect.y < 0.0)
		return vec3 (0.0);  // No intersection with atmosphere

	// Clamp to Earth surface
	vec2 earth_intersect = ray_sphere_intersect (ray_origin, ray_dir, EARTH_RADIUS);
	if (earth_intersect.x > 0.0)
		atmos_intersect.y = earth_intersect.x;

	float ray_length = atmos_intersect.y - max (atmos_intersect.x, 0.0);
	float step_size = ray_length / float (PRIMARY_STEPS);

	// Phase functions
	float cos_theta = dot (ray_dir, sun_dir);
	float phase_r = rayleigh_phase (cos_theta);
	float phase_m = mie_phase (cos_theta, MIE_G);

	// Accumulated optical depth and scattering
	vec3 total_rayleigh = vec3 (0.0);
	vec3 total_mie = vec3 (0.0);
	float optical_depth_r = 0.0;
	float optical_depth_m = 0.0;

	// March along the primary ray
	for (int i = 0; i < PRIMARY_STEPS; i++)
	{
		float t = (float (i) + 0.5) * step_size + max (atmos_intersect.x, 0.0);
		vec3 sample_pos = ray_origin + ray_dir * t;

		float height = length (sample_pos) - EARTH_RADIUS;
		float density_r = density_at_height (height, RAYLEIGH_SCALE_HEIGHT) * step_size;
		float density_m = density_at_height (height, MIE_SCALE_HEIGHT) * step_size;

		optical_depth_r += density_r;
		optical_depth_m += density_m;

		// Light ray march towards sun
		vec2 light_intersect = ray_sphere_intersect (sample_pos, sun_dir, ATMOSPHERE_RADIUS);
		float light_step_size = light_intersect.y / float (LIGHT_STEPS);

		float light_optical_depth_r = 0.0;
		float light_optical_depth_m = 0.0;

		for (int j = 0; j < LIGHT_STEPS; j++)
		{
			float lt = (float (j) + 0.5) * light_step_size;
			vec3 light_pos = sample_pos + sun_dir * lt;
			float light_height = length (light_pos) - EARTH_RADIUS;

			light_optical_depth_r += density_at_height (light_height, RAYLEIGH_SCALE_HEIGHT) * light_step_size;
			light_optical_depth_m += density_at_height (light_height, MIE_SCALE_HEIGHT) * light_step_size;
		}

		// Transmittance
		vec3 tau = RAYLEIGH_COEFF * (optical_depth_r + light_optical_depth_r) +
		           MIE_COEFF * 1.1 * (optical_depth_m + light_optical_depth_m);
		vec3 attenuation = exp (-tau);

		total_rayleigh += density_r * attenuation;
		total_mie += density_m * attenuation;
	}

	// Final color
	vec3 color = phase_r * RAYLEIGH_COEFF * total_rayleigh + phase_m * MIE_COEFF * total_mie;

	return color;
}

