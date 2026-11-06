#include "radiance_lights.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

static bool near (float a, float b) { return fabsf (a - b) < 0.0001f; }

int main (void)
{
	_Static_assert (sizeof (radiance_light_t) == 80, "std430 light layout");
	_Static_assert (sizeof (radiance_material_t) == 80, "std430 material layout");
	_Static_assert (sizeof (radiance_lighting_parameters_t) == 160 &&
		offsetof(radiance_lighting_parameters_t,shadow_parameters) == 144, "std140 light parameter layout");
	/* Overlapping x spans must start and stop within each grid row; preserve
	 * source order and report the required capacity even if output is short. */
	{
		radiance_light_grid_t rows = {{0,0,0}, {10,10,10}, {4,2,1}};
		radiance_light_t row_lights[5] = {0};
		row_lights[0].metadata[0] = RADIANCE_LIGHT_DIRECTIONAL;
		const float positions[4][4] = {{15,5,5,4}, {20,5,5,9}, {35,15,5,4}, {5,15,5,4}};
		for (int i = 0; i < 4; ++i)
			memcpy (row_lights[i + 1].position_radius, positions[i], sizeof positions[i]);
		const uint32_t expected_offsets[] = {0,1,4,6,7,9,11,13,15};
		const uint32_t expected_indices[] = {0,0,1,2,0,2,0,0,4,0,2,0,2,0,3};
		uint32_t row_offsets[9], row_indices[15];
		radiance_light_grid_stats_t row_stats;
		assert (!Radiance_BuildLightGrid (&rows, row_lights, 5, row_offsets, 9, row_indices, 14, &row_stats));
		assert (row_stats.required_indices == 15 && row_stats.indexed_lights == 5);
		assert (Radiance_BuildLightGrid (&rows, row_lights, 5, row_offsets, 9, row_indices, 15, &row_stats));
		assert (!memcmp (row_offsets, expected_offsets, sizeof expected_offsets));
		assert (!memcmp (row_indices, expected_indices, sizeof expected_indices));
	}
	/* World-stable finer grids cover every world corner with padding, even
	 * at non-aligned and large coordinates; invalid input is transactional. */
	const float bounds[][6] = {{-607,-431,-607,1519,3079,431}, {-1,-2,-3,1,2,3},
		{-10000,-5000,-700,10000,5000,700}, {1e8f,1e8f,1e8f,1e8f+4096,1e8f+2048,1e8f+512}};
	for (size_t i = 0; i < sizeof(bounds)/sizeof(bounds[0]); ++i)
	{
		radiance_light_grid_t planned;
		assert (Radiance_PlanLightGrid (bounds[i], bounds[i] + 3, &planned));
		for (int axis = 0; axis < 3; ++axis)
		{
			assert (planned.cell_size[axis] >= 128 && planned.dimensions[axis] <= 32);
			assert (planned.origin[axis] <= bounds[i][axis]);
			assert ((double)planned.origin[axis] + (double)planned.cell_size[axis] * planned.dimensions[axis] > bounds[i][axis + 3]);
		}
		if (!i) assert (planned.cell_size[0] == 128);
	}
	radiance_light_grid_t unchanged = {{1,2,3},{4,5,6},{7,8,9}}, invalid = unchanged;
	const float bad_min[3] = {NAN,0,0}, bad_max[3] = {1,1,1};
	assert (!Radiance_PlanLightGrid (bad_min, bad_max, &invalid) && !memcmp (&unchanged, &invalid, sizeof invalid));
	const char map[] =
		"// before worldspawn\n{\n\"classname\" \"worldspawn\" \"_sunlight\" \"300\" "
		"\"_sun_mangle\" \"0 -90 0\" \"_sunlight_color\" \"255 128 0\" }\n"
		"/* spot target appears later */ { \"classname\" \"light\" \"origin\" \"1 2 3\" "
		"\"_light\" \"255 128 0 600\" \"wait\" \"2\" \"style\" \"3\" "
		"\"target\" \"aim\" \"_cone\" \"60\" }\n"
		"{ \"classname\" \"info_null\" \"targetname\" \"aim\" \"origin\" \"1 12 3\" }\n"
		"{ \"classname\" \"light_torch\" \"origin\" \"10 20 30\" \"angle\" \"90\" }\n"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 5\" \"_shadowlightradius\" \"55\" "
		"\"_shadowlightintensity\" \"7\" }\n"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"target\" \"missing\" }\n"
		"{ \"classname\" \"light\" \"origin\" \"nan 0 0\" }\n";
	radiance_light_t lights[8];
	radiance_light_parse_stats_t stats;
	assert (Radiance_ParseMapLights (map, sizeof (map) - 1, lights, 8, &stats));
	assert (stats.entities == 7 && stats.found == 6 && stats.written == 5 && stats.malformed == 1);
	assert (stats.unresolved_targets == 1 && stats.dropped == 0);
	assert (lights[0].metadata[0] == RADIANCE_LIGHT_DIRECTIONAL && near (lights[0].direction_cone[2], -1));
	assert (near (lights[0].color_intensity[0], 1) && near (lights[0].color_intensity[1], 128.0f / 255));
	assert (lights[1].metadata[0] == RADIANCE_LIGHT_SPOT && near (lights[1].direction_cone[1], 1));
	assert (near (lights[1].position_radius[3], 300) && near (lights[1].color_intensity[3], 2));
	assert (lights[1].metadata[1] == 3 && near (lights[1].direction_cone[3], cosf (3.1415926535f / 6)));
	assert (lights[2].metadata[0] == RADIANCE_LIGHT_POINT); /* angle alone is not a spotlight */
	assert (lights[3].metadata[0] == RADIANCE_LIGHT_POINT && near (lights[3].position_radius[3], 55));
	assert (near (lights[3].color_intensity[3], 7));
	assert (lights[4].metadata[0] == RADIANCE_LIGHT_POINT); /* unresolved target falls back to omni */
	int styles[] = {256, 256, 256, 128};
	radiance_light_t styled = Radiance_ApplyLightstyle (&lights[1], styles, 4);
	assert (near (styled.color_intensity[3], 1) && near (lights[1].color_intensity[3], 2));
	assert (Radiance_ParseMapLights (map, sizeof (map), lights, 1, &stats));
	assert (stats.found == 6 && stats.written == 1 && stats.dropped == 4 && stats.malformed == 1);
	assert (Radiance_ParseMapLights (map, sizeof (map), NULL, 0, &stats));
	assert (stats.written == 0 && stats.dropped == 5);
	const char bad[] = "{ \"classname\" \"light\" \"origin\"";
	assert (!Radiance_ParseMapLights (bad, sizeof (bad), lights, 8, &stats));
	const char bad_comment[] = "/* unfinished";
	assert (!Radiance_ParseMapLights (bad_comment, sizeof (bad_comment), lights, 8, &stats));
	const char rgb_sun[] = "{ \"classname\" \"worldspawn\" \"_sunlight_color\" \"0 255 0\" "
		"\"_sunlight\" \"1 1 1\" }";
	assert (Radiance_ParseMapLights (rgb_sun, sizeof (rgb_sun), lights, 8, &stats));
	assert (stats.written == 1 && near (lights[0].color_intensity[0], 0) && near (lights[0].color_intensity[1], 1));
	assert (stats.sky_upper[3] == 0 && stats.sky_lower[3] == 0);
	const char sky[] = "{ \"classname\" \"worldspawn\" \"_sunlight2\" \"150\" "
		"\"_sunlight2_color\" \"255 128 0\" \"_sunlight3\" \"75\" "
		"\"_sunlight3_color\" \"0 0 255\" }";
	assert (Radiance_ParseMapLights (sky, sizeof (sky), lights, 8, &stats));
	assert (stats.written == 0 && near (stats.sky_upper[0], 1) && near (stats.sky_upper[1], 128.0f / 255));
	assert (near (stats.sky_upper[3], 0.5f) && near (stats.sky_lower[2], 1) && near (stats.sky_lower[3], 0.25f));
	assert (stats.sky_authored);
	const char no_sky[] = "{ \"classname\" \"worldspawn\" }";
	assert (Radiance_ParseMapLights (no_sky, sizeof (no_sky), lights, 8, &stats));
	assert (!stats.sky_authored);
	const float sky_color[3] = {0.1f, 0.2f, 0.3f};
	float fallback[4] = {0, 0, 0, 0};
	assert (!Radiance_ApplySkyFallback (&stats, false, sky_color, 0.15f, fallback));
	assert (!Radiance_ApplySkyFallback (&stats, true, sky_color, 0.0f, fallback));
	assert (Radiance_ApplySkyFallback (&stats, true, sky_color, 0.15f, fallback));
	assert (near (fallback[0], 0.1f) && near (fallback[3], 0.15f));
	const char explicit_zero[] = "{ \"classname\" \"worldspawn\" \"_sunlight2\" \"0\" }";
	assert (Radiance_ParseMapLights (explicit_zero, sizeof (explicit_zero), lights, 8, &stats));
	assert (stats.sky_authored && !Radiance_ApplySkyFallback (&stats, true, sky_color, 0.15f, fallback));
	const float invalid_sky[3] = {NAN, 0.2f, 0.3f};
	stats.sky_authored = false;
	assert (!Radiance_ApplySkyFallback (&stats, true, invalid_sky, 0.15f, fallback));
	/* Identical color but a different sky-miss policy still invalidates the field. */
	float previous_sky[8] = {0.1f,0.2f,0.3f,0.15f,0,0,0,0};
	radiance_lighting_parameters_t sky_parameters = {0};
	memcpy (sky_parameters.sky_radiance, previous_sky, 4 * sizeof (float));
	assert (!Radiance_SkyLightingChanged (previous_sky, false, &sky_parameters));
	sky_parameters.counts[3] |= 16u;
	assert (Radiance_SkyLightingChanged (previous_sky, false, &sky_parameters));
	assert (!Radiance_SkyLightingChanged (previous_sky, true, &sky_parameters));
	sky_parameters.ground_radiance[1] = 0.1f;
	assert (Radiance_SkyLightingChanged (previous_sky, true, &sky_parameters));
	const char color_precedence[] = "{ \"classname\" \"light\" \"origin\" \"0 0 1\" "
		"\"_color\" \"0 1 0\" \"_light\" \"255 0 0 300\" }";
	assert (Radiance_ParseMapLights (color_precedence, sizeof (color_precedence), lights, 8, &stats));
	assert (stats.written == 1 && near (lights[0].color_intensity[0], 0) && near (lights[0].color_intensity[1], 1));
	/* Stock start.bsp uses bright infinite spotlights with mangle. Ignoring
	 * mangle turns them into enormous omnidirectional sources. */
	const char mangled[] =
		"{ \"classname\" \"light\" \"origin\" \"696 704 96\" \"light\" \"4000\" \"delay\" \"3\" \"mangle\" \"90 0 0\" \"angle\" \"60\" }"
		"{ \"classname\" \"light\" \"origin\" \"128 528 72\" \"light\" \"3500\" \"delay\" \"3\" \"mangle\" \"0 270 0\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"400\" \"delay\" \"1\" \"wait\" \"2\" }";
	assert (Radiance_ParseMapLights (mangled, sizeof (mangled) - 1, lights, 8, &stats));
	assert (stats.written == 3 && lights[0].metadata[0] == RADIANCE_LIGHT_SPOT && (lights[0].metadata[3] & 7u) == 3u);
	assert (fabsf (lights[0].direction_cone[0]) < .0001f && near (lights[0].direction_cone[1], 1) && near (lights[0].direction_cone[3], .8660254f));
	assert (lights[1].metadata[0] == RADIANCE_LIGHT_SPOT && near (lights[1].direction_cone[2], -1));
	uint32_t wait_bits = lights[2].metadata[3] & ~7u; float encoded_wait;
	memcpy (&encoded_wait, &wait_bits, sizeof (encoded_wait));
	assert ((lights[2].metadata[3] & 7u) == 1u && near (encoded_wait, 2) && near (lights[2].position_radius[3], 25600));
	/* Compiler lights use the world angle sensitivity (.5 by default), unless
	 * overridden, and an optional inner spotlight cone. Worldspawn may occur
	 * after a light in a nonstandard entity lump. KEX lights stay separate. */
	const char angles[] =
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"mangle\" \"0 -90 0\" \"angle\" \"60\" \"_softangle\" \"30\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"_anglesense\" \"0.25\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"_anglescale\" \"2\" }"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"_anglescale\" \"0.2\" }"
		"{ \"classname\" \"worldspawn\" \"_anglescale\" \"0.75\" }";
	radiance_light_t angle_lights[4];
	assert (Radiance_ParseMapLights (angles, sizeof (angles) - 1, angle_lights, 4, &stats) && stats.written == 4);
	assert (near (angle_lights[0].area_axis[0], .75f) && near (angle_lights[0].area_axis[1], cosf (15.0f * 3.14159265358979323846f / 180.0f)) && near (angle_lights[0].area_axis[2], 1));
	assert (near (angle_lights[1].area_axis[0], .25f) && near (angle_lights[2].area_axis[0], .75f));
	assert (near (angle_lights[3].area_axis[2], 0));
	/* Match R_ParseEntityDlights: KEX lights without a positive cone are
	 * omnidirectional even when they have a valid target or an angle. */
	const char kex_cones[] =
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"angle\" \"90\" \"_shadowlightconeangle\" \"0\" }"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"angle\" \"90\" \"_shadowlightconeangle\" \"-1\" }"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"target\" \"aim\" }"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"target\" \"aim\" \"_shadowlightconeangle\" \"30\" }"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"angle\" \"90\" \"_shadowlightconeangle\" \"45\" }"
		"{ \"classname\" \"info_null\" \"origin\" \"0 10 0\" \"targetname\" \"aim\" }";
	radiance_light_t kex_lights[5];
	assert (Radiance_ParseMapLights (kex_cones, sizeof kex_cones, kex_lights, 5, &stats) && stats.written == 5);
	for (int i = 0; i < 3; ++i)
		assert (kex_lights[i].metadata[0] == RADIANCE_LIGHT_POINT && kex_lights[i].direction_cone[3] == -1);
	assert (kex_lights[3].metadata[0] == RADIANCE_LIGHT_SPOT && near (kex_lights[3].direction_cone[1], 1) && near (kex_lights[3].direction_cone[3], cosf (3.14159265358979323846f / 6)));
	assert (kex_lights[4].metadata[0] == RADIANCE_LIGHT_SPOT && near (kex_lights[4].direction_cone[1], 1) && near (kex_lights[4].direction_cone[3], cosf (3.14159265358979323846f / 4)));
	/* ericw: default 300, negative lights, and linear _falloff which
	 * overrides wait without changing intensity. Nonlinear falloff ignores it. */
	const char ericw[] =
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"-300\" \"wait\" \"2\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"600\" \"wait\" \"4\" \"_falloff\" \"256\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"400\" \"wait\" \"2\" \"_falloff\" \"256\" \"delay\" \"1\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"delay\" \"4\" }";
	radiance_light_t ericw_lights[5];
	assert (Radiance_ParseMapLights (ericw, sizeof (ericw) - 1, ericw_lights, 5, &stats));
	assert (stats.written == 5 && near (ericw_lights[0].position_radius[3], 300) && near (ericw_lights[0].color_intensity[3], 1));
	assert (near (ericw_lights[0].area_axis[0], .5f) && near (ericw_lights[0].area_axis[2], 1));
	assert (near (ericw_lights[1].position_radius[3], 150) && near (ericw_lights[1].color_intensity[3], -1));
	assert (near (ericw_lights[2].position_radius[3], 256) && near (ericw_lights[2].color_intensity[3], 2));
	assert (near (ericw_lights[3].position_radius[3], 25600) && (ericw_lights[4].metadata[3] & 7u) == 4u);
	radiance_light_t negative_styled = Radiance_ApplyLightstyle (&ericw_lights[1], (const int[]){128}, 1);
	assert (near (negative_styled.color_intensity[3], -.5f));
	/* World settings are resolved even after their light entities. _range
	 * changes brightness but not distance; _dist changes distance but is
	 * overridden by a linear _falloff. KEX sources retain their own values. */
	const char world_scales[] =
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"400\" \"wait\" \"2\" \"delay\" \"1\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"600\" \"_falloff\" \"256\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"-300\" }"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"_shadowlightradius\" \"55\" \"_shadowlightintensity\" \"7\" \"delay\" \"3\" \"wait\" \"2\" }"
		"{ \"classname\" \"worldspawn\" \"_dist\" \"2\" \"_range\" \"1\" \"_sunlight\" \"300\" \"_sunlight2\" \"150\" }";
	radiance_light_t scaled[5];
	assert (Radiance_ParseMapLights (world_scales, sizeof world_scales, scaled, 5, &stats));
	assert (stats.written == 5 && near (scaled[0].position_radius[3], 12800));
	uint32_t scaled_wait_bits = scaled[0].metadata[3] & ~7u; float scaled_wait;
	memcpy (&scaled_wait, &scaled_wait_bits, sizeof scaled_wait);
	assert (near (scaled_wait, 4) && near (scaled[0].color_intensity[3], 8.0f / 3.0f));
	assert (near (scaled[1].position_radius[3], 256) && near (scaled[1].color_intensity[3], 4));
	assert (near (scaled[2].position_radius[3], 150) && near (scaled[2].color_intensity[3], -2));
	assert (near (scaled[3].position_radius[3], 55) && near (scaled[3].color_intensity[3], 7) && scaled[3].metadata[3] == 0);
	assert (near (scaled[4].color_intensity[3], 2) && near (stats.sky_upper[3], 1));
	const char zero_scales[] = "{ \"classname\" \"worldspawn\" \"_dist\" \"0\" \"_range\" \"0\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"_falloff\" \"64\" }";
	assert (Radiance_ParseMapLights (zero_scales, sizeof zero_scales, scaled, 5, &stats));
	assert (stats.written == 2 && (scaled[0].metadata[3] & 7u) == 3u && scaled[0].color_intensity[3] == 0);
	assert ((scaled[1].metadata[3] & 7u) == 0u && near (scaled[1].position_radius[3], 64));
	/* Infinite lights must appear in cells even far beyond their radius. */
	radiance_light_grid_t far_grid = {{9000,9000,9000},{32,32,32},{1,1,1}};
	uint32_t far_offsets[2], far_indices[2]; radiance_light_grid_stats_t far_stats;
	radiance_light_t infinite_points[2] = {lights[0], lights[1]};
	infinite_points[0].metadata[0] = infinite_points[1].metadata[0] = RADIANCE_LIGHT_POINT;
	assert (Radiance_BuildLightGrid (&far_grid, infinite_points, 2, far_offsets, 2, far_indices, 2, &far_stats));
	assert (far_indices[0] == 0 && far_indices[1] == 1 && far_stats.required_indices == 2);
	/* A long area emitter must reach cells near its ends, beyond the radius
	 * measured from its center. */
	radiance_light_t long_emitter={0}; long_emitter.metadata[0]=RADIANCE_LIGHT_EMISSIVE;
	long_emitter.position_radius[3]=100; long_emitter.area_axis[3]=400; long_emitter.direction_cone[3]=2;
	radiance_light_grid_t end_grid={{440,-4,-4},{16,8,8},{1,1,1}};
	uint32_t end_offsets[2], end_indices[1]; radiance_light_grid_stats_t end_stats;
	assert (Radiance_BuildLightGrid (&end_grid,&long_emitter,1,end_offsets,2,end_indices,1,&end_stats));
	assert (end_stats.required_indices==1 && end_indices[0]==0);
	long_emitter.area_axis[3]=NAN;
	assert (!Radiance_BuildLightGrid (&end_grid,&long_emitter,1,end_offsets,2,end_indices,1,&end_stats));

	float position[] = {1, 2, 3}, color[] = {1, 0.5f, 0}, direction[] = {0, 3, 0};
	radiance_light_t dynamic;
	assert (Radiance_MakeDynamicLight (&dynamic, position, 100, color, 2, direction, 0.5f, 5, 5));
	assert (dynamic.metadata[0] == RADIANCE_LIGHT_SPOT && near (dynamic.direction_cone[1], 1));
	assert (dynamic.metadata[2] == RADIANCE_LIGHT_SOURCE_DYNAMIC);
	Radiance_ConfigureLegacyDynamicLight (&dynamic, 32);
	float encoded_minlight; memcpy (&encoded_minlight, &dynamic.metadata[1], sizeof (encoded_minlight));
	assert ((dynamic.metadata[2] & RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC) && near (encoded_minlight, 32));
	assert (near (dynamic.color_intensity[3], 100.0f * 3.14159265358979323846f / 128.0f));
	assert (!Radiance_MakeDynamicLight (&dynamic, position, 100, color, 2, direction, 0.5f, 4, 5));
	assert (!Radiance_MakeDynamicLight (&dynamic, position, NAN, color, 2, NULL, -1, 5, 5));

	uint8_t palette[1024] = {0};
	palette[224 * 4] = 255; palette[224 * 4 + 3] = 255;
	palette[255 * 4 + 1] = 255; palette[255 * 4 + 3] = 255;
	const uint8_t pixels[] = {0, 224, 255, 1};
	radiance_emissive_t emission;
	assert (Radiance_AnalyzeIndexedEmission (pixels, 4, palette, true, &emission));
	assert (emission.emitting_texels == 1 && near (emission.coverage, 0.25f));
	assert (near (emission.radiance[0], 0.25f) && near (emission.radiance[1], 0));
	assert (Radiance_AnalyzeIndexedEmission (pixels, 4, palette, false, &emission));
	assert (emission.emitting_texels == 2 && near (emission.radiance[1], 0.25f));
	const uint8_t mask[] = {255, 0, 0, 255, 0, 255, 0, 0};
	assert (Radiance_AnalyzeMaskEmission (mask, 2, &emission));
	assert (emission.emitting_texels == 1 && near (emission.coverage, 0.5f) && near (emission.radiance[0], 0.5f));
	assert (Radiance_AnalyzeMaskEmission (NULL, 0, &emission) && emission.coverage == 0);

	/* Every light capable of reaching a sampled point must be in that cell's
	 * CSR list; over-inclusion is permitted, omission is not. */
	radiance_light_grid_t grid = {{-128, -128, -128}, {32, 32, 32}, {8, 8, 8}};
	radiance_light_t grid_lights[33] = {0};
	grid_lights[0].metadata[0] = RADIANCE_LIGHT_DIRECTIONAL;
	uint32_t rng = 0xa13fc241u;
	for (int i = 1; i < 33; ++i)
	{
		for (int a = 0; a < 3; ++a)
		{
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			grid_lights[i].position_radius[a] = (float)((int)(rng % 512) - 256);
		}
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		grid_lights[i].position_radius[3] = 1 + rng % 180;
	}
	uint32_t offsets[513], indices[33 * 512];
	radiance_light_grid_stats_t grid_stats;
	assert (!Radiance_BuildLightGrid (&grid, grid_lights, 33, offsets, 513, NULL, 0, &grid_stats));
	assert (grid_stats.required_indices > 0 && grid_stats.cell_count == 512);
	assert (Radiance_BuildLightGrid (&grid, grid_lights, 33, offsets, 513, indices, 33 * 512, &grid_stats));
	assert (offsets[512] == grid_stats.required_indices);
	for (int n = 0; n < 10000; ++n)
	{
		float point[3]; unsigned cell_coord[3];
		for (int a = 0; a < 3; ++a)
		{
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			point[a] = -128.0f + (rng % 25600) * 0.01f;
			cell_coord[a] = (unsigned)((point[a] + 128.0f) / 32.0f);
		}
		unsigned cell = (cell_coord[2] * 8 + cell_coord[1]) * 8 + cell_coord[0];
		for (unsigned i = 0; i < 33; ++i)
		{
			float distance2 = 0;
			for (int a = 0; a < 3; ++a)
			{
				float delta = point[a] - grid_lights[i].position_radius[a];
				distance2 += delta * delta;
			}
			if (i && distance2 >= grid_lights[i].position_radius[3] * grid_lights[i].position_radius[3]) continue;
			bool found = false;
			for (uint32_t j = offsets[cell]; j < offsets[cell + 1]; ++j) if (indices[j] == i) found = true;
			assert (found);
		}
	}
	/* Cone pruning must retain every point the shader could illuminate, for
	 * finite and unattenuated spotlights, including wide and non-unit axes.
	 * A delay-4 minimum ignores the cone in the shader. */
	radiance_light_t spots[5] = {0};
	for (int i = 0; i < 5; ++i)
	{
		spots[i].metadata[0] = RADIANCE_LIGHT_SPOT;
		spots[i].metadata[2] = RADIANCE_LIGHT_SOURCE_MAP;
		spots[i].metadata[3] = 3;
		spots[i].direction_cone[0] = 1;
		spots[i].direction_cone[3] = 0.8f;
		spots[i].area_axis[2] = 1;
	}
	spots[1].direction_cone[0] = 0.8f; spots[1].direction_cone[1] = 0.6f;
	spots[1].direction_cone[3] = -0.5f;
	spots[2].metadata[2] = RADIANCE_LIGHT_SOURCE_DYNAMIC; spots[2].position_radius[3] = 100;
	spots[2].direction_cone[0] = 0.6f; spots[2].direction_cone[2] = 0.8f;
	spots[3].metadata[3] = 4; /* local minimum ignores spotlight direction */
	spots[4].direction_cone[0] = 0.99f; spots[4].direction_cone[3] = 0.99999f;
	assert (Radiance_BuildLightGrid (&grid, spots, 5, offsets, 513, indices, 33 * 512, &grid_stats));
	assert (grid_stats.required_indices < 5 * 512); /* directional rejection is active */
	for (int n = 0; n < 30000; ++n)
	{
		float point[3]; unsigned c[3];
		for (int a = 0; a < 3; ++a)
		{
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			point[a] = -128.0f + (rng % 25600) * 0.01f;
			c[a] = (unsigned)((point[a] + 128.0f) / 32.0f);
		}
		unsigned cell = (c[2] * 8 + c[1]) * 8 + c[0];
		for (unsigned i = 0; i < 5; ++i)
		{
			float len2 = point[0]*point[0] + point[1]*point[1] + point[2]*point[2];
			if (i == 2 && len2 >= 10000) continue;
			if (i != 3 && len2 > 1e-10f)
			{
				float dot = 0;
				for (int a = 0; a < 3; ++a) dot += point[a] * spots[i].direction_cone[a];
				float cutoff = i == 2 ? fminf (spots[i].direction_cone[3], 0.9999f) : spots[i].direction_cone[3];
				if (dot / sqrtf (len2) < cutoff) continue;
			}
			bool found = false;
			for (uint32_t j = offsets[cell]; j < offsets[cell + 1]; ++j) if (indices[j] == i) found = true;
			assert (found);
		}
	}
	/* Exercise arbitrary orientations and off-center sources, rather than only
	 * axis-aligned cones through the grid origin. */
	radiance_light_t random_spots[24] = {0};
	for (int i = 0; i < 24; ++i)
	{
		radiance_light_t *light = &random_spots[i];
		light->metadata[0] = RADIANCE_LIGHT_SPOT;
		light->metadata[2] = i & 1 ? RADIANCE_LIGHT_SOURCE_MAP : RADIANCE_LIGHT_SOURCE_DYNAMIC;
		light->metadata[3] = i & 1 ? 3 : 0;
		light->area_axis[2] = i & 1 ? 1 : 0;
		light->position_radius[3] = 80 + i * 7;
		float len2 = 0;
		for (int a = 0; a < 3; ++a)
		{
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			light->position_radius[a] = (int)(rng % 240) - 120;
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			light->direction_cone[a] = ((int)(rng % 2001) - 1000) / 1000.0f;
			len2 += light->direction_cone[a] * light->direction_cone[a];
		}
		for (int a = 0; a < 3; ++a) light->direction_cone[a] /= sqrtf (len2);
		light->direction_cone[3] = (i % 10) / 10.0f;
	}
	assert (Radiance_BuildLightGrid (&grid, random_spots, 24, offsets, 513, indices, 33 * 512, &grid_stats));
	for (int n = 0; n < 30000; ++n)
	{
		float point[3]; unsigned c[3];
		for (int a = 0; a < 3; ++a)
		{
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			point[a] = -128.0f + (rng % 25600) * 0.01f;
			c[a] = (unsigned)((point[a] + 128.0f) / 32.0f);
		}
		unsigned cell = (c[2] * 8 + c[1]) * 8 + c[0];
		for (unsigned i = 0; i < 24; ++i)
		{
			float len2 = 0, dot = 0;
			for (int a = 0; a < 3; ++a)
			{
				float delta = point[a] - random_spots[i].position_radius[a];
				len2 += delta * delta; dot += delta * random_spots[i].direction_cone[a];
			}
			if (!(i & 1) && len2 >= random_spots[i].position_radius[3] * random_spots[i].position_radius[3]) continue;
			if (len2 > 1e-10f && dot / sqrtf (len2) < random_spots[i].direction_cone[3]) continue;
			bool found = false;
			for (uint32_t j = offsets[cell]; j < offsets[cell + 1]; ++j) if (indices[j] == i) found = true;
			assert (found);
		}
	}
	/* Float cell selection near boundaries at large coordinates must also
	 * retain every light that can reach the point. */
	radiance_light_grid_t large = {{1e8f, 1e8f, 1e8f}, {16, 16, 16}, {8, 8, 8}};
	radiance_light_t edge = {0};
	edge.position_radius[0] = 1e8f + 40;
	edge.position_radius[1] = 1e8f + 40;
	edge.position_radius[2] = 1e8f + 40;
	edge.position_radius[3] = 20;
	assert (Radiance_BuildLightGrid (&large, &edge, 1, offsets, 513, indices, 33 * 512, &grid_stats));
	for (int z = 0; z < 128; z += 8)
		for (int y = 0; y < 128; y += 8)
			for (int x = 0; x < 128; x += 8)
			{
				float point[3] = {1e8f + x, 1e8f + y, 1e8f + z}, distance2 = 0;
				unsigned c[3];
				for (int a = 0; a < 3; ++a)
				{
					float delta = point[a] - edge.position_radius[a];
					distance2 += delta * delta;
					c[a] = (unsigned)floorf ((point[a] - large.origin[a]) * (1.0f / large.cell_size[a]));
				}
				if (distance2 >= edge.position_radius[3] * edge.position_radius[3]) continue;
				unsigned cell = (c[2] * 8 + c[1]) * 8 + c[0];
				assert (offsets[cell + 1] - offsets[cell] == 1 && indices[offsets[cell]] == 0);
			}
	/* The grid reuse predicate must never omit a cell in a fresh build. Try
	 * moving and shrinking ordinary and rectangular emissive lights, including
	 * bounds partially outside the grid. */
	uint32_t old_offsets[513], new_offsets[513], old_indices[512], new_indices[512];
	radiance_light_grid_stats_t old_stats, new_stats;
	unsigned retained = 0;
	for (unsigned trial = 0; trial < 3000; ++trial)
	{
		radiance_light_t old = {0}, next;
		old.metadata[0] = trial % 2 ? RADIANCE_LIGHT_EMISSIVE : RADIANCE_LIGHT_POINT;
		for (int a = 0; a < 3; ++a)
		{
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			old.position_radius[a] = (int)(rng % 768) - 384;
		}
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		old.position_radius[3] = 1 + rng % 300;
		old.area_axis[3] = 12; old.direction_cone[3] = 20;
		next = old;
		for (int a = 0; a < 3; ++a)
		{
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			next.position_radius[a] += (int)(rng % 65) - 32;
		}
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		next.position_radius[3] = 1 + rng % 300;
		if (!Radiance_LightGridBoundsCover (&grid, &old, &next)) continue;
		++retained;
		assert (Radiance_BuildLightGrid (&grid, &old, 1, old_offsets, 513, old_indices, 512, &old_stats));
		assert (Radiance_BuildLightGrid (&grid, &next, 1, new_offsets, 513, new_indices, 512, &new_stats));
		for (unsigned cell = 0; cell < 512; ++cell)
			if (new_offsets[cell + 1] != new_offsets[cell])
				assert (old_offsets[cell + 1] != old_offsets[cell]);
	}
	assert (retained > 100);
	edge.metadata[0] = RADIANCE_LIGHT_SPOT;
	assert (!Radiance_LightGridBoundsCover (&grid, &edge, &edge));
	puts ("PASS: runtime map, style, dynamic and emissive light sources");
	return 0;
}
