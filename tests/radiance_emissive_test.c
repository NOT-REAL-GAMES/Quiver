#include "radiance_emissive.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void triangle(rc_scene_triangle_t *t, float x, uint32_t surface)
{
	memset(t,0,sizeof(*t));
	t->position[0][0]=x; t->position[1][0]=x+16; t->position[2][0]=x;
	t->position[2][1]=16; t->metadata[2]=surface;
}

static void persistent_material_dependencies(void)
{
	const uint32_t base_count=32768;
	rc_scene_triangle_t *triangles=calloc(base_count+1,sizeof(*triangles));assert(triangles);
	for(uint32_t i=0;i<base_count;++i) {triangle(&triangles[i],(float)(i%64)*16,1);
		triangles[i].metadata[1]=RC_SCENE_DYNAMIC;triangles[i].metadata[3]=10;}
	triangle(&triangles[base_count],32,2);triangles[base_count].metadata[1]=RC_SCENE_DYNAMIC;
	triangles[base_count].metadata[3]=20;triangles[base_count].metadata[0]=1;
	rc_scene_t scene={.triangles=triangles,.triangle_count=base_count};
	uint8_t pixels[]={255,0,0,255,0,255,0,255,0,0,255,255};
	radiance_material_t materials[3]={0};
	for(unsigned i=0;i<3;++i) {materials[i].layers_flags[2]=RADIANCE_MATERIAL_EMISSION_TEXTURE;
		materials[i].emission_rect[0]=i;materials[i].emission_rect[2]=materials[i].emission_rect[3]=1;
		for(unsigned c=0;c<4;++c)materials[i].emission[c]=1;}
	radiance_material_atlas_t atlas={.materials=materials,.material_count=1,.rgba=pixels,.bytes=sizeof(pixels),.width=3,.height=1,.layers=1};
	radiance_emissive_cache_t *cache=Radiance_EmissiveCacheCreate();assert(cache);
	radiance_light_t lights[8];float camera[3]={0};radiance_emissive_stats_t stats;bool changed;
	assert(Radiance_BuildEmissiveLightsCached(cache,&scene,base_count,&atlas,1,true,true,camera,lights,8,&stats,&changed));
	assert(stats.emitted_lights==1 && lights[0].color_intensity[0]==1);
	radiance_emissive_cache_counters_t original,current;Radiance_EmissiveCacheCounters(cache,&original);
	for(unsigned i=0;i<10;++i)
	{
		scene.triangle_count=base_count+1;atlas.material_count=2;
		assert(Radiance_BuildEmissiveLightsCached(cache,&scene,base_count,&atlas,2+i*2,false,true,camera,lights,8,&stats,&changed));
		assert(stats.emitted_lights==2 && lights[0].color_intensity[0]==1);
		Radiance_EmissiveCacheCounters(cache,&current);
		assert(current.static_rebuilds==original.static_rebuilds && current.static_triangle_scans==original.static_triangle_scans);
		scene.triangle_count=base_count;atlas.material_count=1;
		assert(Radiance_BuildEmissiveLightsCached(cache,&scene,base_count,&atlas,3+i*2,false,true,camera,lights,8,&stats,&changed));
		assert(stats.emitted_lights==1);
		Radiance_EmissiveCacheCounters(cache,&current);
		assert(current.static_rebuilds==original.static_rebuilds && current.static_triangle_scans==original.static_triangle_scans);
	}
	/* Actual base color, alpha, or represented-mask dependencies still
	 * invalidate its cached light and repaint its proxy flags correctly. */
	pixels[0]=0;pixels[1]=pixels[2]=255;
	assert(Radiance_BuildEmissiveLightsCached(cache,&scene,base_count,&atlas,30,false,false,camera,lights,8,&stats,&changed));
	assert(stats.emitted_lights==1 && lights[0].color_intensity[0]==0 && lights[0].color_intensity[1]==1);
	Radiance_EmissiveCacheCounters(cache,&current);assert(current.static_rebuilds==original.static_rebuilds+1);
	uint8_t represented[3]={1};
	assert(Radiance_BuildEmissiveLightsCachedFiltered(cache,&scene,base_count,&atlas,30,false,false,represented,camera,lights,8,&stats,&changed));
	assert(!stats.emitted_lights && (triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	represented[0]=0;pixels[3]=0;
	assert(Radiance_BuildEmissiveLightsCachedFiltered(cache,&scene,base_count,&atlas,31,false,false,represented,camera,lights,8,&stats,&changed));
	assert(!stats.emitted_lights && !(triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	Radiance_EmissiveCacheDestroy(cache);free(triangles);
}

int main(void)
{
	persistent_material_dependencies();
	/* One red emitting texel and one dark texel: half the surface emits. */
	uint8_t pixels[]={255,0,0,255, 0,0,0,0};
	radiance_material_t materials[1]={0};
	materials[0].layers_flags[2]=RADIANCE_MATERIAL_EMISSION_TEXTURE;
	materials[0].emission[0]=materials[0].emission[1]=materials[0].emission[2]=materials[0].emission[3]=1;
	materials[0].emission_rect[2]=2; materials[0].emission_rect[3]=1;
	radiance_material_atlas_t atlas={.materials=materials,.material_count=1,.rgba=pixels,.bytes=8,.width=2,.height=1,.layers=1};
	rc_scene_triangle_t triangles[2]; triangle(&triangles[0],0,1); triangle(&triangles[1],500,2);
	rc_scene_t scene={.triangles=triangles,.triangle_count=2}; float camera[3]={0};
	radiance_light_t lights[2]; radiance_emissive_stats_t stats; bool changed;
	assert(Radiance_BuildEmissiveLights(&scene,&atlas,camera,lights,1,&stats,&changed));
	assert(changed && stats.emissive_surfaces==2 && stats.emitted_lights==1 && stats.omitted_lights==1);
	assert((triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY) && !(triangles[1].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	assert(lights[0].metadata[0]==RADIANCE_LIGHT_EMISSIVE && lights[0].metadata[2]==RADIANCE_LIGHT_SOURCE_TEXTURE);
	assert(fabsf(lights[0].color_intensity[0]-1)<1e-5 && lights[0].color_intensity[1]==0 && lights[0].color_intensity[2]==0);
	assert(fabsf(lights[0].color_intensity[3]-.25f)<1e-5 && lights[0].position_radius[3]>96 && lights[0].direction_cone[3]>1);
	assert (!(lights[0].metadata[2] & RADIANCE_LIGHT_SOURCE_ONE_SIDED));
	assert(Radiance_BuildEmissiveLights(&scene,&atlas,camera,lights,1,&stats,&changed) && !changed);
	assert(Radiance_BuildEmissiveLights(&scene,&atlas,camera,lights,2,&stats,&changed) && changed);
	assert(stats.emitted_lights==2 && !stats.omitted_lights && (triangles[1].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	/* Invalid input must not change previous scene flags. */
	materials[0].emission_rect[2]=3;
	assert(!Radiance_BuildEmissiveLights(&scene,&atlas,camera,lights,2,&stats,&changed));
	assert(!changed && (triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY) && (triangles[1].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	materials[0].emission_rect[2]=2;
	assert(Radiance_BuildEmissiveLights(&scene,&atlas,camera,NULL,0,&stats,&changed));
	assert(changed && stats.omitted_lights==2 && !(triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY) && !(triangles[1].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	/* Long tilted wall lamp: the proxy retains its oriented rectangular extent
	 * instead of becoming a spherical light centered on a triangle. */
	const float s=.7071067811865475f, center[3]={100,50,20}, u[3]={s,s,0}, v[3]={0,0,1};
	float corners[4][3]; const int signs[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
	for(int i=0;i<4;++i) for(int c=0;c<3;++c) corners[i][c]=center[c]+16*signs[i][0]*u[c]+4*signs[i][1]*v[c];
	memset(triangles,0,sizeof(triangles)); const int index[2][3]={{0,1,2},{0,2,3}};
	for(int t=0;t<2;++t) {triangles[t].metadata[2]=7; for(int p=0;p<3;++p) for(int c=0;c<3;++c) triangles[t].position[p][c]=corners[index[t][p]][c];}
	assert(Radiance_BuildEmissiveLights(&scene,&atlas,camera,lights,2,&stats,&changed));
	assert(stats.emitted_lights==1 && stats.emissive_surfaces==1);
	assert(fabsf(lights[0].area_axis[3]-16)<1e-4f && fabsf(lights[0].direction_cone[3]-4)<1e-4f);
	for(int c=0;c<3;++c) assert(fabsf(lights[0].position_radius[c]-center[c])<1e-4f);
	assert(fabsf(lights[0].area_axis[0]-s)<1e-5f && fabsf(lights[0].area_axis[1]-s)<1e-5f && fabsf(lights[0].area_axis[2])<1e-5f);
	assert(fabsf(lights[0].direction_cone[0]-s)<1e-5f && fabsf(lights[0].direction_cone[1]+s)<1e-5f);
	for (int t=0;t<2;++t) triangles[t].metadata[1] |= RC_SCENE_ORIENTED;
	assert(Radiance_BuildEmissiveLights(&scene,&atlas,camera,lights,2,&stats,&changed));
	assert(lights[0].metadata[2]&RADIANCE_LIGHT_SOURCE_ONE_SIDED);
	triangles[1].metadata[1] &= ~RC_SCENE_ORIENTED;
	assert(Radiance_BuildEmissiveLights(&scene,&atlas,camera,lights,2,&stats,&changed));
	assert(!(lights[0].metadata[2]&RADIANCE_LIGHT_SOURCE_ONE_SIDED));
	/* Material pixels and static geometry remain cached across movement of a
	 * dynamic occluder; scene rebuilds still restore all proxy flags. */
	rc_scene_triangle_t cached_triangles[3]; triangle(&cached_triangles[0],0,1); triangle(&cached_triangles[1],80,2);
	triangle(&cached_triangles[2],160,3); cached_triangles[2].metadata[1]=RC_SCENE_DYNAMIC; cached_triangles[2].metadata[3]=1;
	rc_scene_t cached_scene={.triangles=cached_triangles,.triangle_count=3}; radiance_light_t cached_lights[3];
	radiance_emissive_cache_t *cache=Radiance_EmissiveCacheCreate(); assert(cache);
	assert(Radiance_BuildEmissiveLightsCached(cache,&cached_scene,2,&atlas,1,true,true,camera,cached_lights,3,&stats,&changed));
	assert(changed && stats.emitted_lights==3);
	radiance_emissive_cache_counters_t counters, prior;
	Radiance_EmissiveCacheCounters(cache,&prior);
	assert(prior.material_rebuilds==1 && prior.static_rebuilds==1 && prior.dynamic_rebuilds==1 && prior.static_triangle_scans==2);
	camera[0]=10;
	assert(Radiance_BuildEmissiveLightsCached(cache,&cached_scene,2,&atlas,1,false,false,camera,cached_lights,3,&stats,&changed) && !changed);
	Radiance_EmissiveCacheCounters(cache,&counters); assert(!memcmp(&prior,&counters,sizeof(prior)));
	for(int p=0;p<3;++p) cached_triangles[2].position[p][0]+=16;
	assert(Radiance_BuildEmissiveLightsCached(cache,&cached_scene,2,&atlas,1,false,true,camera,cached_lights,3,&stats,&changed));
	Radiance_EmissiveCacheCounters(cache,&counters);
	assert(counters.static_rebuilds==1 && counters.material_rebuilds==1 && counters.dynamic_rebuilds==2 && counters.static_triangle_scans==2);
	for(int i=0;i<3;++i) cached_triangles[i].metadata[1]&=~RC_SCENE_EMISSIVE_PROXY;
	assert(Radiance_BuildEmissiveLightsCached(cache,&cached_scene,2,&atlas,1,true,true,camera,cached_lights,3,&stats,&changed) && changed);
	for(int i=0;i<3;++i) assert(cached_triangles[i].metadata[1]&RC_SCENE_EMISSIVE_PROXY);
	Radiance_EmissiveCacheCounters(cache,&counters); assert(counters.static_rebuilds==1 && counters.static_triangle_scans==4);
	assert(Radiance_BuildEmissiveLightsCached(cache,&cached_scene,2,&atlas,2,false,false,camera,cached_lights,1,&stats,&changed) && changed);
	assert(stats.emitted_lights==1 && stats.omitted_lights==2);
	/* A revision alone need not rebuild unchanged material dependencies. */
	Radiance_EmissiveCacheCounters(cache,&counters); assert(counters.material_rebuilds==2 && counters.static_rebuilds==1);
	/* Explicit surface-template sources replace automatic proxies without
	 * also counting the hit texture as emitted radiance. Changing the mask
	 * must invalidate the cached material and geometry groups. */
	uint8_t represented[1]={1};
	assert(Radiance_BuildEmissiveLightsCachedFiltered(cache,&cached_scene,2,&atlas,2,false,false,represented,camera,cached_lights,3,&stats,&changed));
	assert(stats.emitted_lights==0);
	for(int i=0;i<3;++i) assert(cached_triangles[i].metadata[1]&RC_SCENE_EMISSIVE_PROXY);
	represented[0]=0;
	assert(Radiance_BuildEmissiveLightsCachedFiltered(cache,&cached_scene,2,&atlas,2,false,false,represented,camera,cached_lights,3,&stats,&changed));
	assert(stats.emitted_lights==3);
	Radiance_EmissiveCacheDestroy(cache);
	/* Under a tight budget, rank a long surface by its nearest point, not by
	 * the distance to its center. Both surfaces have equal area/brightness. */
	rc_scene_triangle_t ranked[4]={0};
	const float rectangle[2][4][3]={{{-100,-1,0},{100,-1,0},{100,1,0},{-100,1,0}},
		{{70,20,0},{90,20,0},{90,40,0},{70,40,0}}};
	for(int r=0;r<2;++r) for(int t=0;t<2;++t) {
		ranked[r*2+t].metadata[2]=(uint32_t)(10+r);
		for(int p=0;p<3;++p) for(int c=0;c<3;++c) ranked[r*2+t].position[p][c]=rectangle[r][index[t][p]][c];
	}
	rc_scene_t ranked_scene={.triangles=ranked,.triangle_count=4}; float end_camera[3]={100,10,5};
	cache=Radiance_EmissiveCacheCreate(); assert(cache);
	assert(Radiance_BuildEmissiveLightsCached(cache,&ranked_scene,4,&atlas,1,true,true,end_camera,lights,1,&stats,&changed));
	assert((ranked[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY) && !(ranked[2].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	Radiance_EmissiveCacheDestroy(cache);
	/* Emissive liquids produce source proxies while non-emissive water and
	 * sky do not. Liquid still has its liquid flag for transparent tracing. */
	radiance_material_t liquid_materials[2] = {0}; liquid_materials[0] = materials[0];
	radiance_material_atlas_t liquid_atlas = atlas;
	liquid_atlas.materials = liquid_materials; liquid_atlas.material_count = 2;
	rc_scene_triangle_t liquid_triangles[4];
	for (int i=0;i<4;++i) triangle(&liquid_triangles[i], i*32, (uint32_t)(20+i));
	liquid_triangles[0].metadata[1]=RC_SCENE_LIQUID|RC_SCENE_ORIENTED;
	liquid_triangles[1].metadata[1]=RC_SCENE_LIQUID; liquid_triangles[1].metadata[0]=1;
	liquid_triangles[2].metadata[1]=RC_SCENE_SKY;
	liquid_triangles[3].metadata[1]=RC_SCENE_LIQUID|RC_SCENE_DYNAMIC|RC_SCENE_ORIENTED;
	liquid_triangles[3].metadata[3]=1;
	rc_scene_t liquid_scene={.triangles=liquid_triangles,.triangle_count=4};
	radiance_light_t liquid_lights[4];
	assert(Radiance_BuildEmissiveLights(&liquid_scene,&liquid_atlas,camera,liquid_lights,4,&stats,&changed));
	assert(stats.emissive_surfaces==2 && stats.emitted_lights==2 && !stats.omitted_lights);
	assert((liquid_triangles[0].metadata[1]&(RC_SCENE_LIQUID|RC_SCENE_EMISSIVE_PROXY))==(RC_SCENE_LIQUID|RC_SCENE_EMISSIVE_PROXY));
	assert(!(liquid_triangles[1].metadata[1]&RC_SCENE_EMISSIVE_PROXY) && !(liquid_triangles[2].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	for(int i=0;i<2;++i) assert(liquid_lights[i].metadata[2]&RADIANCE_LIGHT_SOURCE_ONE_SIDED);
	cache=Radiance_EmissiveCacheCreate(); assert(cache);
	assert(Radiance_BuildEmissiveLightsCached(cache,&liquid_scene,3,&liquid_atlas,1,true,true,camera,liquid_lights,4,&stats,&changed));
	assert(stats.emissive_surfaces==2 && stats.emitted_lights==2 && !stats.omitted_lights);
	assert(liquid_triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY);
	assert(liquid_triangles[3].metadata[1]&RC_SCENE_EMISSIVE_PROXY);
	assert(Radiance_BuildEmissiveLightsCached(cache,&liquid_scene,3,&liquid_atlas,1,false,false,camera,liquid_lights,0,&stats,&changed));
	assert(changed && stats.omitted_lights==2 && !(liquid_triangles[0].metadata[1]&RC_SCENE_EMISSIVE_PROXY) && !(liquid_triangles[3].metadata[1]&RC_SCENE_EMISSIVE_PROXY));
	Radiance_EmissiveCacheDestroy(cache);
	puts("PASS: bounded, prioritized emissive surface proxies and transactional flags"); return 0;
}
