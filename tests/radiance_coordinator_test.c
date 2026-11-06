/* Coordinator logic with a fake upload/atlas; actual atlas and Vulkan upload
 * have separate focused tests. This exercises lifecycle and frame contents. */
#include "quakedef.h"
#include "radiance_coordinator.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint64_t revision = 1;
static bool atlas_ok = true, upload_ok = true;
static int builds, uploads;
static radiance_light_t captured[16];
static radiance_input_snapshot_t last;
static radiance_material_t material;
static uint8_t pixel[] = {1, 2, 3, 255};
static void test_surface_subdivision (void)
{
	const char map[]="{\"classname\" \"light\" \"_surface\" \"glow\" \"_surface_offset\" \"2\"}";
	radiance_coordinator_limits_t limits={4,16,64,256,1024,8,4};
	radiance_coordinator_t c; assert(RadianceCoordinator_InitLights(&c,&limits));
	assert(RadianceCoordinator_LoadMap(&c,map,sizeof(map)-1));
	texture_t texture={0}; strcpy(texture.name,"glow");
	radiance_quake_material_t entry={.brush=&texture};
	radiance_quake_materials_t materials={.entries=&entry,.count=1};
	/* An inclined 256 x 128 convex face. One boundary vertex lies exactly
	 * on the split plane; fan triangles share and cancel their interior edges. */
	const float points[5][3]={{0,0,0},{128,0,64},{256,0,128},{256,128,128},{0,128,0}};
	rc_scene_triangle_t tri[3]={0};
	for(int i=0;i<3;++i) for(int a=0;a<3;++a) {
		tri[i].position[0][a]=points[0][a]; tri[i].position[1][a]=points[i+1][a]; tri[i].position[2][a]=points[i+2][a];
	}
	rc_scene_t scene={.triangles=tri,.triangle_count=3};
	assert(RadianceCoordinator_PrepareSurfaceLights(&c,&scene,3,&materials,1));
	assert(c.surface_static_count==2 && c.surface_unmatched_count==0);
	const double nx=-2.0/sqrt(5.0), nz=4.0/sqrt(5.0);
	assert(fabs(c.surface_static_lights[0].position_radius[0]-(192+nx))<1e-4);
	assert(fabs(c.surface_static_lights[0].position_radius[2]-(96+nz))<1e-4);
	assert(fabs(c.surface_static_lights[1].position_radius[0]-(64+nx))<1e-4);
	assert(fabs(c.surface_static_lights[1].position_radius[2]-(32+nz))<1e-4);
	RadianceCoordinator_Clear(&c);
	/* A tiny reversed triangle emits once, with the face-normal offset flipped. */
	assert(RadianceCoordinator_InitLights(&c,&limits)); assert(RadianceCoordinator_LoadMap(&c,map,sizeof(map)-1));
	memset(tri,0,sizeof(tri)); tri[0].position[1][1]=4; tri[0].position[2][0]=8; scene.triangle_count=1;
	assert(RadianceCoordinator_PrepareSurfaceLights(&c,&scene,1,&materials,1));
	assert(c.surface_static_count==1 && fabs(c.surface_static_lights[0].position_radius[2]+2)<1e-5);
	RadianceCoordinator_Clear(&c);
	/* Nonfinite geometry and subdivision overflow both require fallback. */
	assert(RadianceCoordinator_InitLights(&c,&limits)); assert(RadianceCoordinator_LoadMap(&c,map,sizeof(map)-1));
	tri[0].position[1][0]=NAN;
	assert(!RadianceCoordinator_PrepareSurfaceLights(&c,&scene,1,&materials,1));
	RadianceCoordinator_Clear(&c);
	limits.max_lights=4; assert(RadianceCoordinator_InitLights(&c,&limits)); assert(RadianceCoordinator_LoadMap(&c,map,sizeof(map)-1));
	memset(tri,0,sizeof(tri)); tri[0].position[1][0]=2048; tri[0].position[2][1]=128;
	assert(!RadianceCoordinator_PrepareSurfaceLights(&c,&scene,1,&materials,1));
	RadianceCoordinator_Clear(&c);
}
void RadianceQuakeMaterials_Init (radiance_quake_materials_t *m) { memset (m, 0, sizeof (*m)); }
void RadianceQuakeMaterials_Clear (radiance_quake_materials_t *m) { memset (m, 0, sizeof (*m)); }
uint64_t RadianceQuakeMaterials_Revision (const radiance_quake_materials_t *m) { (void)m; return revision; }
void RadianceMaterialAtlas_Clear (radiance_material_atlas_t *a) { memset (a, 0, sizeof (*a)); }
bool RadianceQuakeMaterials_BuildAtlas (const radiance_quake_materials_t *m, radiance_material_atlas_t *a,
	uint32_t dim, uint32_t layers, size_t bytes)
{
	(void)m; (void)dim; (void)layers; (void)bytes; ++builds;
	if (!atlas_ok) return false;
	*a = (radiance_material_atlas_t){.materials = &material, .rgba = pixel, .material_count = 1,
		.bytes = 4, .width = 1, .height = 1, .layers = 1};
	return true;
}
bool RadianceInputs_Update (radiance_gpu_inputs_t *gpu, uint32_t frame, const radiance_input_snapshot_t *s)
{
	(void)gpu; assert (frame < 2); ++uploads; last = *s;
	assert (s->light_count <= 16); memcpy (captured, s->lights, s->light_count * sizeof (*captured));
	assert (s->grid_offsets[s->grid_offset_count - 1] == s->grid_index_count);
	for (size_t i = 0; i < s->grid_index_count; ++i) assert (s->grid_indices[i] < s->light_count);
	return upload_ok;
}
int main (void)
{
	test_surface_subdivision ();
	/* A compiler surface template never casts at its authored origin. Matching
	 * small brush faces produce one source per face and instance;
	 * a changed dynamic transform moves that source without rebuilding static. */
	{
		const char surface_map[] =
			"{ \"classname\" \"light\" \"origin\" \"1000 1000 1000\" \"_surface\" \"GLOW\" \"light\" \"300\" \"style\" \"2\" \"_color\" \"255 0 0\" \"_surface_offset\" \"3\" \"_surface_spotlight\" \"1\" }"
			"{ \"classname\" \"light\" \"_surface\" \"missing\" }"
			"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"200\" }";
		radiance_coordinator_limits_t sl = {8, 16, 64, 256, 1024, 8, 4};
		radiance_coordinator_t sc; assert (RadianceCoordinator_InitLights (&sc, &sl));
		assert (RadianceCoordinator_LoadMap (&sc, surface_map, sizeof (surface_map) - 1));
		assert (sc.map_count == 1 && sc.surface_template_count == 2);
		texture_t tex = {0}, other = {0}; strcpy (tex.name, "glow"); strcpy (other.name, "stone");
		radiance_quake_material_t entries[2] = {{.brush=&tex}, {.brush=&other}};
		radiance_quake_materials_t materials = {.entries=entries, .count=2};
		rc_scene_triangle_t tri[4] = {0};
		/* Two triangles of a quad and a translated instance of one triangle. */
		tri[0].position[1][0]=8; tri[0].position[2][0]=8; tri[0].position[2][1]=4;
		tri[1].position[1][0]=8; tri[1].position[1][1]=4; tri[1].position[2][1]=4;
		tri[2]=tri[0]; tri[2].metadata[1]=RC_SCENE_DYNAMIC; tri[2].metadata[3]=1;
		for (int p=0;p<3;++p) tri[2].position[p][0]+=20;
		tri[3]=tri[0]; tri[3].metadata[0]=1; tri[3].metadata[2]=99;
		for (int i=0;i<3;++i) tri[i].metadata[2]=7;
		rc_scene_t geometry = {.triangles=tri,.triangle_count=4};
		assert (RadianceCoordinator_PrepareSurfaceLights (&sc, &geometry, 2, &materials, 1));
		assert (sc.surface_static_count == 1 && sc.surface_dynamic_count == 1 && sc.surface_unmatched_count == 1);
		assert (fabsf (sc.surface_static_lights[0].position_radius[0]-4) < 1e-5f);
		assert (fabsf (sc.surface_static_lights[0].position_radius[1]-2) < 1e-5f);
		assert (fabsf (sc.surface_static_lights[0].position_radius[2]-3) < 1e-5f);
		assert (sc.surface_static_lights[0].metadata[0] == RADIANCE_LIGHT_SPOT && sc.surface_static_lights[0].direction_cone[2] > .99f);
		radiance_light_t *static_ptr = sc.surface_static_lights;
		for (int p=0;p<3;++p) tri[2].position[p][0]+=10;
		assert (RadianceCoordinator_PrepareSurfaceLights (&sc, &geometry, 2, &materials, 2));
		assert (sc.surface_static_lights == static_ptr && sc.surface_dynamic_lights[0].position_radius[0] > 30);
		radiance_light_grid_t sg = {{-64,-64,-64},{64,64,64},{2,2,2}};
		int ss[3]={256,256,128}; radiance_input_snapshot_t shot;
		assert (RadianceCoordinator_PrepareLights (&sc, ss, 3, NULL, 0, 1, &sg, &shot));
		assert (shot.light_count == 3 && shot.lights[1].color_intensity[0] == 1 && shot.lights[1].color_intensity[1] == 0);
		assert (fabsf (shot.lights[1].color_intensity[3] - .5f) < 1e-5f);
		RadianceCoordinator_Clear (&sc);
	}
	/* KEX dynamiclight spawnflag 1 disables the source only in co-op. Classic
	 * light spawnflags have unrelated meanings and must not suppress them. */
	{
		const char coop_map[] =
			"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"spawnflags\" \"1\" }"
			"{ \"classname\" \"dynamiclight\" \"origin\" \"16 0 0\" \"spawnflags\" \"2\" }"
			"{ \"classname\" \"light\" \"origin\" \"32 0 0\" \"spawnflags\" \"1\" }";
		radiance_coordinator_limits_t coop_limits = {4,4,64,256,0,0,0};
		radiance_coordinator_t coop; radiance_input_snapshot_t shot;
		radiance_light_grid_t cg = {{-64,-64,-64},{64,64,64},{2,2,2}};
		assert (RadianceCoordinator_InitLights (&coop, &coop_limits));
		assert (RadianceCoordinator_LoadMap (&coop, coop_map, sizeof (coop_map) - 1));
		assert (coop.map_count == 3 && (coop.map_lights[0].metadata[2] & RADIANCE_LIGHT_SOURCE_NOT_IN_COOP));
		assert (!(coop.map_lights[1].metadata[2] & RADIANCE_LIGHT_SOURCE_NOT_IN_COOP));
		assert (!(coop.map_lights[2].metadata[2] & RADIANCE_LIGHT_SOURCE_NOT_IN_COOP));
		assert (RadianceCoordinator_PrepareLights (&coop, NULL, 0, NULL, 0, 0, &cg, &shot));
		float normal = shot.lights[0].color_intensity[3];
		assert (normal > 0 && shot.lights[1].color_intensity[3] > 0 && shot.lights[2].color_intensity[3] > 0);
		uint64_t grid_revision = shot.grid_revision;
		RadianceCoordinator_SetCooperative (&coop, true);
		assert (RadianceCoordinator_PrepareLights (&coop, NULL, 0, NULL, 0, 0, &cg, &shot));
		assert (shot.lights[0].color_intensity[3] == 0 && shot.lights[1].color_intensity[3] > 0 && shot.lights[2].color_intensity[3] > 0);
		assert (shot.grid_revision == grid_revision);
		RadianceCoordinator_SetCooperative (&coop, false);
		assert (RadianceCoordinator_PrepareLights (&coop, NULL, 0, NULL, 0, 0, &cg, &shot));
		assert (shot.lights[0].color_intensity[3] == normal && shot.grid_revision == grid_revision);
		RadianceCoordinator_Clear (&coop);
	}
	/* Rerelease map lights fade with camera distance, not receiver distance.
	 * The immutable map source and conservative grid survive camera motion. */
	{
		const char fade_map[] =
			"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"_shadowlightstartfadedistance\" \"100\" \"_shadowlightendfadedistance\" \"300\" \"_shadowlightstyle\" \"1\" }"
			"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" }"
			"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"_shadowlightendfadedistance\" \"1\" }"
			"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"_shadowlightstartfadedistance\" \"400\" \"_shadowlightendfadedistance\" \"300\" }";
		radiance_coordinator_limits_t limits = {4,4,64,256,0,0,0};
		radiance_coordinator_t c; radiance_input_snapshot_t shot;
		radiance_light_grid_t grid = {{-512,-512,-512},{512,512,512},{2,2,2}};
		int styles[] = {256,128};
		assert (RadianceCoordinator_InitLights (&c, &limits));
		assert (RadianceCoordinator_LoadMap (&c, fade_map, sizeof (fade_map) - 1));
		float origin[3] = {100,0,0};
		RadianceCoordinator_SetViewOrigin (&c, origin);
		assert (RadianceCoordinator_PrepareLights (&c, styles, 2, NULL, 0, 0, &grid, &shot));
		float full = shot.lights[0].color_intensity[3]; uint64_t rev = shot.grid_revision;
		assert (fabsf (full - 5.0f) < 1e-5f && shot.lights[1].color_intensity[3] > 0 && shot.lights[2].color_intensity[3] > 0);
		origin[0] = 120; origin[1] = 160; RadianceCoordinator_SetViewOrigin (&c, origin);
		assert (RadianceCoordinator_PrepareLights (&c, styles, 2, NULL, 0, 0, &grid, &shot));
		assert (fabsf (shot.lights[0].color_intensity[3] - full * .5f) < 1e-5f && shot.lights[3].color_intensity[3] == 10 && shot.grid_revision == rev);
		origin[0] = 300; origin[1] = 0; RadianceCoordinator_SetViewOrigin (&c, origin);
		assert (RadianceCoordinator_PrepareLights (&c, styles, 2, NULL, 0, 0, &grid, &shot));
		assert (shot.lights[0].color_intensity[3] == 0 && shot.lights[1].color_intensity[3] > 0 && shot.lights[2].color_intensity[3] > 0 && shot.lights[3].color_intensity[3] == 0 && shot.grid_revision == rev);
		origin[0] = NAN; RadianceCoordinator_SetViewOrigin (&c, origin);
		assert (RadianceCoordinator_PrepareLights (&c, styles, 2, NULL, 0, 0, &grid, &shot));
		assert (shot.lights[0].color_intensity[3] == full && shot.grid_revision == rev);
		RadianceCoordinator_Clear (&c);
	}
	const char map[] = "{ \"classname\" \"worldspawn\" \"_sunlight2\" \"90\" }"
		"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"light\" \"300\" \"style\" \"2\" }"
		"{ \"classname\" \"dynamiclight\" \"origin\" \"32 0 0\" \"light\" \"100\" }";
	radiance_gpu_inputs_t gpu = {0};
	radiance_coordinator_limits_t limits = {4, 4, 64, 256, 1024, 8, 4};
	radiance_coordinator_t c;
	assert (RadianceCoordinator_Init (&c, &gpu, &limits));
	assert (RadianceCoordinator_LoadMap (&c, map, sizeof (map) - 1));
	assert (c.map_count == 2 && c.map_stats.sky_upper[3] > 0);
	radiance_light_grid_t grid = {{-64, -64, -64}, {64, 64, 64}, {2, 2, 2}};
	int styles[] = {256, 256, 128};
	dlight_t d[3] = {0};
	for (int i = 0; i < 3; ++i) { d[i].radius = 64; d[i].die = 2; d[i].color[0] = 1; d[i].cone_cos = -1; }
	d[1].key = 0x40000000; d[2].die = 0;
	/* A CPU-only coordinator hands lighting to the collector without creating
	 * another atlas or assigning material IDs. No GPU device or atlas limit is
	 * required, and map/style/dynamic source behavior matches the upload path. */
	radiance_coordinator_t lights_only;
	radiance_coordinator_limits_t light_limits = limits;
	light_limits.max_atlas_dimension = light_limits.max_atlas_layers = 0; light_limits.max_atlas_bytes = 0;
	assert (RadianceCoordinator_InitLights (&lights_only, &light_limits));
	assert (RadianceCoordinator_LoadMap (&lights_only, map, sizeof (map) - 1));
	radiance_input_snapshot_t sources;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, d, 3, 1, &grid, &sources));
	assert (!builds && !uploads && !sources.materials && !sources.atlas_rgba && !sources.material_count);
	assert (sources.light_count == 3 && sources.parameters.counts[0] == 3 && !sources.parameters.counts[1] &&
		!sources.parameters.counts[2] && sources.parameters.counts[3] == 5);
	assert (lights_only.grid_builds == 1);
	uint64_t initial_grid_revision = sources.grid_revision;
	assert (initial_grid_revision != 0);
	assert (sources.parameters.grid_origin[3] == 1.0f && sources.parameters.grid_inv_cell_size[3] == 1.0f);
	assert (RadianceCoordinator_SetGains (&lights_only, 0.2f, 2.0f));
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, d, 3, 1, &grid, &sources));
	assert (lights_only.grid_builds == 1); /* Changing gains doesn't alter membership. */
	assert (sources.grid_revision == initial_grid_revision);
	styles[2] = 64;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, d, 3, 1, &grid, &sources));
	assert (lights_only.grid_builds == 1 && sources.lights[0].color_intensity[3] ==
		lights_only.map_lights[0].color_intensity[3] * 0.25f); /* Flicker only updates light data. */
	styles[2] = 128;
	assert (sources.parameters.grid_origin[3] == 0.2f && sources.parameters.grid_inv_cell_size[3] == 2.0f);
	assert (!RadianceCoordinator_SetGains (&lights_only, -1.0f, 1.0f));
	radiance_light_t emitter={0}; emitter.metadata[0]=RADIANCE_LIGHT_EMISSIVE;
	emitter.metadata[2]=RADIANCE_LIGHT_SOURCE_TEXTURE; emitter.position_radius[3]=64;
	assert (RadianceCoordinator_PrepareLightsWithEmitters (&lights_only, styles, 3, d, 3, 1, &grid, &emitter, 1, &sources));
	assert (lights_only.grid_builds == 2);
	assert (sources.grid_revision != 0 && sources.grid_revision != initial_grid_revision);
	assert (sources.light_count==4 && sources.lights[3].metadata[0]==RADIANCE_LIGHT_EMISSIVE);
	assert (sources.lights[0].color_intensity[3] == lights_only.map_lights[0].color_intensity[3] * 0.5f &&
		(sources.lights[2].metadata[2] & (RADIANCE_LIGHT_SOURCE_DYNAMIC|RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC)) ==
		(RADIANCE_LIGHT_SOURCE_DYNAMIC|RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC));
	assert (sources.parameters.sky_radiance[3] == lights_only.map_stats.sky_upper[3] &&
		sources.grid_offsets[sources.grid_offset_count - 1] == sources.grid_index_count);
	assert (!RadianceCoordinator_PrepareLightsWithEmitters (&lights_only, styles, 3, d, 3, 1, &grid, &emitter, 2, &sources));
	assert (!sources.lights && !sources.grid_offsets && !sources.light_count);
	/* Direction and cone changes affect spotlight grid membership even when
	 * its center, intensity and radius stay unchanged. */
	d[0].cone_cos = 0.8f; d[0].cone_dir[0] = 1;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, d, 3, 1, &grid, &sources));
	uint64_t spot_builds = lights_only.grid_builds;
	d[0].cone_dir[0] = 0; d[0].cone_dir[1] = 1;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, d, 3, 1, &grid, &sources));
	assert (lights_only.grid_builds == spot_builds + 1);
	d[0].cone_cos = 0.3f;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, d, 3, 1, &grid, &sources));
	assert (lights_only.grid_builds == spot_builds + 2);
	/* CL_ParseTEnt's rocket explosion allocates an anonymous white dlight,
	 * radius 350 and lifetime 0.5 seconds. It must enter the live snapshot,
	 * carry classic light units, update as it decays, and disappear on expiry. */
	dlight_t explosion={0}; explosion.origin[0]=16; explosion.radius=350; explosion.die=10.5f;
	explosion.decay=300; explosion.color[0]=explosion.color[1]=explosion.color[2]=1; explosion.cone_cos=-2;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, &explosion, 1, 10, &grid, &sources));
	assert (sources.light_count==3 && sources.lights[2].metadata[0]==RADIANCE_LIGHT_POINT &&
		(sources.lights[2].metadata[2]&RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC));
	assert (sources.lights[2].position_radius[0]==16 && sources.lights[2].position_radius[3]==350 &&
		fabsf(sources.lights[2].color_intensity[3]-350.0f*3.14159265358979323846f/128.0f)<1e-5f);
	explosion.radius=275;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, &explosion, 1, 10.25, &grid, &sources));
	assert (sources.light_count==3 && sources.lights[2].position_radius[3]==275);
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, &explosion, 1, 10.5, &grid, &sources));
	assert (sources.light_count==3);
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, &explosion, 1, 10.51, &grid, &sources));
	assert (sources.light_count==2);
	/* A live KEX dynamic source must retain KEX shading rather than being
	 * interpreted as an ordinary Quake dynamic light. */
	explosion.die=12; explosion.kex_intensity=3;
	assert (RadianceCoordinator_PrepareLights (&lights_only, styles, 3, &explosion, 1, 11, &grid, &sources));
	assert (sources.light_count==3 && sources.lights[2].color_intensity[3]==3 &&
		(sources.lights[2].metadata[2] & (RADIANCE_LIGHT_SOURCE_DYNAMIC|RADIANCE_LIGHT_SOURCE_KEX|RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC)) ==
		(RADIANCE_LIGHT_SOURCE_DYNAMIC|RADIANCE_LIGHT_SOURCE_KEX));
	grid.cell_size[0] = 0;
	assert (!RadianceCoordinator_PrepareLights (&lights_only, styles, 3, d, 3, 1, &grid, &sources));
	assert (!sources.lights && !sources.grid_offsets && !sources.light_count);
	grid.cell_size[0] = 64;
	assert (!RadianceCoordinator_Prepare (&lights_only, 0, styles, 3, d, 3, 1, &grid));
	RadianceCoordinator_Clear (&lights_only);
	/* Default spotlight softening is a per-frame approximation. Authored
	 * _softangle, including zero, and non-compiler lights stay untouched.
	 * The unchanged outer cone retains the same conservative grid. */
	{
		const char spot_map[] =
			"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"mangle\" \"0 0 0\" }"
			"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"mangle\" \"0 0 0\" \"_softangle\" \"0\" }"
			"{ \"classname\" \"light\" \"origin\" \"0 0 0\" \"mangle\" \"0 0 0\" \"_softangle\" \"20\" }"
			"{ \"classname\" \"dynamiclight\" \"origin\" \"0 0 0\" \"mangle\" \"0 0 0\" \"_shadowlightconeangle\" \"40\" }";
		radiance_coordinator_t spot;
		assert (RadianceCoordinator_InitLights (&spot, &light_limits));
		assert (RadianceCoordinator_LoadMap (&spot, spot_map, sizeof (spot_map) - 1));
		assert (spot.map_count == 4);
		assert (RadianceCoordinator_PrepareLights (&spot, NULL, 0, NULL, 0, 1, &grid, &sources));
		uint64_t rev = sources.grid_revision, builds_before = spot.grid_builds;
		float edge = sources.lights[0].direction_cone[3];
		float explicit_zero = sources.lights[1].area_axis[1];
		float explicit_soft = sources.lights[2].area_axis[1];
		float kex = sources.lights[3].area_axis[1];
		assert (sources.lights[0].area_axis[1] == edge);
		assert (sources.lights[1].metadata[2] & RADIANCE_LIGHT_SOURCE_EXPLICIT_SOFTANGLE);
		assert (sources.lights[2].metadata[2] & RADIANCE_LIGHT_SOURCE_EXPLICIT_SOFTANGLE);
		assert (RadianceCoordinator_SetSpotSoftness (&spot, 0.5f));
		assert (RadianceCoordinator_PrepareLights (&spot, NULL, 0, NULL, 0, 1, &grid, &sources));
		assert (fabsf (sources.lights[0].area_axis[1] - cosf (acosf (edge) * 0.5f)) < 1e-6f);
		assert (sources.lights[0].direction_cone[3] == edge && spot.map_lights[0].area_axis[1] == edge);
		assert (sources.lights[0].position_radius[3] == spot.map_lights[0].position_radius[3] &&
			sources.lights[0].metadata[3] == spot.map_lights[0].metadata[3] &&
			sources.lights[0].color_intensity[3] == spot.map_lights[0].color_intensity[3]);
		assert (sources.lights[1].area_axis[1] == explicit_zero && sources.lights[2].area_axis[1] == explicit_soft && sources.lights[3].area_axis[1] == kex);
		assert (spot.grid_builds == builds_before && sources.grid_revision == rev);
		assert (RadianceCoordinator_SetSpotSoftness (&spot, 0.0f));
		assert (RadianceCoordinator_PrepareLights (&spot, NULL, 0, NULL, 0, 1, &grid, &sources));
		assert (sources.lights[0].area_axis[1] == edge && sources.grid_revision == rev);
		assert (RadianceCoordinator_SetSpotSoftness (&spot, 2.0f));
		assert (RadianceCoordinator_PrepareLights (&spot, NULL, 0, NULL, 0, 1, &grid, &sources));
		assert (sources.lights[0].area_axis[1] == 1.0f && sources.grid_revision == rev);
		assert (RadianceCoordinator_SetSpotSoftness (&spot, -1.0f));
		assert (RadianceCoordinator_PrepareLights (&spot, NULL, 0, NULL, 0, 1, &grid, &sources));
		assert (sources.lights[0].area_axis[1] == edge && sources.grid_revision == rev);
		assert (!RadianceCoordinator_SetSpotSoftness (&spot, NAN));
		RadianceCoordinator_Clear (&spot);
	}
	assert (RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid));
	assert (uploads == 1 && builds == 1 && last.light_count == 3 && last.parameters.counts[3] == 5);
	assert (captured[0].color_intensity[3] == c.map_lights[0].color_intensity[3] * 0.5f);
	assert ((captured[2].metadata[2] & (RADIANCE_LIGHT_SOURCE_DYNAMIC|RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC)) ==
		(RADIANCE_LIGHT_SOURCE_DYNAMIC|RADIANCE_LIGHT_SOURCE_LEGACY_DYNAMIC));
	assert (RadianceCoordinator_Prepare (&c, 1, styles, 3, d, 3, 1, &grid) && builds == 1);
	++revision; assert (RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid) && builds == 2);
	++revision; atlas_ok = false; assert (!RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid));
	atlas_ok = true; assert (RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid));
	unsigned old_uploads = (unsigned)uploads; upload_ok = false;
	assert (!RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid)); upload_ok = true;
	grid.cell_size[0] = 0; assert (!RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid));
	assert ((unsigned)uploads == old_uploads + 1); grid.cell_size[0] = 64;
	c.limits.max_grid_indices = 1; assert (!RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid));
	assert (!RadianceCoordinator_LoadMap (&c, "{", 1));
	assert (!RadianceCoordinator_Prepare (&c, 0, styles, 3, d, 3, 1, &grid));
	c.limits.max_map_lights = 1;
	assert (!RadianceCoordinator_LoadMap (&c, map, sizeof (map) - 1) && c.map_stats.dropped);
	RadianceCoordinator_Clear (&c);
	/* Rebuilding into an existing index allocation and growing it must both
	 * produce the same lists as a fresh build. A reduced limit must still be
	 * enforced even when the old allocation is larger than that limit. */
	radiance_coordinator_t reuse;
	assert (RadianceCoordinator_InitLights (&reuse, &light_limits));
	assert (RadianceCoordinator_LoadMap (&reuse, map, sizeof (map) - 1));
	radiance_light_grid_t reuse_grid = {{-64, -64, -64}, {64, 64, 64}, {2, 2, 2}};
	uint32_t expected_offsets[65], expected_indices[256];
	radiance_light_grid_stats_t expected;
	assert (RadianceCoordinator_PrepareLights (&reuse, styles, 3, NULL, 0, 1, &reuse_grid, &sources));
	uint32_t *old_indices = reuse.grid_indices;
	reuse_grid.origin[0] += 1; /* Invalidates the grid without growing it. */
	assert (RadianceCoordinator_PrepareLights (&reuse, styles, 3, NULL, 0, 1, &reuse_grid, &sources));
	assert (reuse.grid_indices == old_indices);
	assert (Radiance_BuildLightGrid (&reuse_grid, sources.lights, sources.light_count,
		expected_offsets, 65, expected_indices, 256, &expected));
	assert (sources.grid_index_count == expected.required_indices &&
		!memcmp (sources.grid_offsets, expected_offsets, sources.grid_offset_count * sizeof (*expected_offsets)) &&
		!memcmp (sources.grid_indices, expected_indices, sources.grid_index_count * sizeof (*expected_indices)));
	for (int axis = 0; axis < 3; ++axis) reuse_grid.dimensions[axis] = 4;
	assert (RadianceCoordinator_PrepareLights (&reuse, styles, 3, NULL, 0, 1, &reuse_grid, &sources));
	assert (Radiance_BuildLightGrid (&reuse_grid, sources.lights, sources.light_count,
		expected_offsets, 65, expected_indices, 256, &expected));
	assert (sources.grid_index_count == expected.required_indices &&
		!memcmp (sources.grid_offsets, expected_offsets, sources.grid_offset_count * sizeof (*expected_offsets)) &&
		!memcmp (sources.grid_indices, expected_indices, sources.grid_index_count * sizeof (*expected_indices)));
	reuse.limits.max_grid_indices = 1;
	reuse_grid.origin[0] += 1;
	assert (!RadianceCoordinator_PrepareLights (&reuse, styles, 3, NULL, 0, 1, &reuse_grid, &sources));
	RadianceCoordinator_Clear (&reuse);
	/* A fading or moving non-spot source may retain its previous conservative
	 * cell list. A fresh grid must always be a subset, and an expanding source
	 * must rebuild as soon as it needs any previously unindexed cell. */
	{
		radiance_coordinator_t moving;
		radiance_coordinator_limits_t moving_limits = {4, 4, 512, 4096, 0, 0, 0};
		const char empty_map[] = "{ \"classname\" \"worldspawn\" }";
		radiance_light_grid_t moving_grid = {{-512,-512,-512}, {128,128,128}, {8,8,8}};
		dlight_t light = {0}; light.radius = 300; light.die = 100; light.color[0] = 1; light.cone_cos = -1;
		assert (RadianceCoordinator_InitLights (&moving, &moving_limits));
		assert (RadianceCoordinator_LoadMap (&moving, empty_map, sizeof (empty_map) - 1));
		assert (RadianceCoordinator_PrepareLights (&moving, NULL, 0, &light, 1, 1, &moving_grid, &sources));
		assert (moving.grid_builds == 1);
		uint64_t cached_revision = sources.grid_revision;
		light.radius = 100;
		light.origin[0] = 140;
		assert (RadianceCoordinator_PrepareLights (&moving, NULL, 0, &light, 1, 1, &moving_grid, &sources));
		assert (moving.grid_builds == 1 && sources.lights[0].position_radius[3] == 100);
		assert (sources.grid_revision == cached_revision);
		uint32_t fresh_offsets[513], fresh_indices[512]; radiance_light_grid_stats_t fresh;
		assert (Radiance_BuildLightGrid (&moving_grid, sources.lights, sources.light_count,
			fresh_offsets, 513, fresh_indices, 512, &fresh));
		for (size_t cell = 0; cell < fresh.cell_count; ++cell)
			if (fresh_offsets[cell] != fresh_offsets[cell + 1])
				assert (sources.grid_offsets[cell] != sources.grid_offsets[cell + 1] &&
					sources.grid_indices[sources.grid_offsets[cell]] == 0);
		light.radius = 500;
		assert (RadianceCoordinator_PrepareLights (&moving, NULL, 0, &light, 1, 1, &moving_grid, &sources));
		assert (moving.grid_builds == 2);
		assert (sources.grid_revision != 0 && sources.grid_revision != cached_revision);
		light.cone_cos = 0.8f; light.cone_dir[0] = 1;
		assert (RadianceCoordinator_PrepareLights (&moving, NULL, 0, &light, 1, 1, &moving_grid, &sources));
		assert (moving.grid_builds == 3);
		light.origin[0] += 1;
		assert (RadianceCoordinator_PrepareLights (&moving, NULL, 0, &light, 1, 1, &moving_grid, &sources));
		assert (moving.grid_builds == 4); /* Spotlight clipping is direction-dependent. */
		cached_revision = sources.grid_revision;
		assert (RadianceCoordinator_LoadMap (&moving, empty_map, sizeof (empty_map) - 1));
		assert (RadianceCoordinator_PrepareLights (&moving, NULL, 0, &light, 1, 1, &moving_grid, &sources));
		assert (sources.grid_revision != 0 && sources.grid_revision != cached_revision);
		cached_revision = sources.grid_revision;
		RadianceCoordinator_Clear (&moving);
		assert (RadianceCoordinator_InitLights (&moving, &moving_limits));
		assert (RadianceCoordinator_LoadMap (&moving, empty_map, sizeof (empty_map) - 1));
		assert (RadianceCoordinator_PrepareLights (&moving, NULL, 0, &light, 1, 1, &moving_grid, &sources));
		assert (sources.grid_revision != 0 && sources.grid_revision != cached_revision);
		RadianceCoordinator_Clear (&moving);
	}
	puts ("PASS: map/lightstyle/dlight snapshots, deduplication, refresh and bounded fallback");
	return 0;
}
