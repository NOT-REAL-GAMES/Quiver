/* Camera-relative displacement; keep constants and equations synchronized with planet_curvature.h. */
vec2 planet_sag (float distance)
{
	const float radius = 203872000.0;
	float d = max (distance, 0.0);
	float u = clamp ((d - 4096.0) / 4096.0, 0.0, 1.0);
	float blend = u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
	float slope = 30.0 * u * u * (u - 1.0) * (u - 1.0) / 4096.0;
	float bounded = min (d, radius * 0.5);
	float root = sqrt (radius * radius - bounded * bounded);
	float beyond = d - bounded;
	float sag = bounded * bounded / (radius + root) + beyond * bounded / root + beyond * beyond / (2.0 * radius);
	return vec2 (sag * blend, (bounded / root + beyond / radius) * blend + sag * slope);
}
