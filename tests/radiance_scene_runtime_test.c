#include "../Quake/quakedef.h"
#include "../Quake/radiance_scene_runtime.h"
#include "../Quake/radiance_materials_quake.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Isolate the scene-manager behavior; the real renderer supplies matrices. */
void IdentityMatrix (float m[16])
{
	memset (m, 0, 16 * sizeof (*m)); m[0]=m[5]=m[10]=m[15]=1;
}
void R_RotateForEntityScale (float m[16], vec3_t origin, vec3_t angles, float scale)
{
	(void)angles; (void)scale; m[12]=origin[0]; m[13]=origin[1]; m[14]=origin[2];
}
static uint32_t material_id (const texture_t *texture, void *user)
{
	(void)user; return texture ? 4 : UINT32_MAX;
}
static bool alias_supported (const entity_t *entity, void *user)
{
	(void)user; return entity->frame != 1;
}
static uint32_t alias_material;
static bool alias_append (rc_scene_t *scene, const entity_t *entity, const float matrix[16],
	uint32_t instance, rc_scene_material_fn material, void *user)
{
	(void)material; (void)user;
	rc_scene_triangle_t triangle = {0};
	triangle.position[0][0]=triangle.position[1][0]=triangle.position[2][0]=matrix[12] + entity->syncbase;
	triangle.position[1][0]+=1; triangle.position[2][1]=1; triangle.metadata[0]=alias_material; triangle.metadata[3]=instance;
	assert (RCScene_AppendTriangle (scene, &triangle));
	return entity->frame != 2; /* emulate failure after a partial append */
}

/* An alias surface above the world lets a shadow ray distinguish an ECS
 * occluder from the floor, while retaining each batch instance's identity. */
static bool batch_alias_append (rc_scene_t *scene, const entity_t *entity, const float matrix[16],
	uint32_t instance, rc_scene_material_fn material, void *user)
{
	(void)entity; (void)material; (void)user;
	rc_scene_triangle_t triangle = {0};
	for (int i = 0; i < 3; ++i)
	{
		triangle.position[i][0] = matrix[12];
		triangle.position[i][1] = matrix[13];
		triangle.position[i][2] = matrix[14];
	}
	triangle.position[1][0] += 1; triangle.position[2][1] += 1;
	triangle.metadata[1] = RC_SCENE_DYNAMIC; triangle.metadata[3] = instance;
	return RCScene_AppendTriangle (scene, &triangle);
}

static uint64_t alias_revision (const entity_t *entity, void *user)
{
	(void)user;
	uint32_t bits; memcpy (&bits, &entity->syncbase, sizeof (bits)); return bits;
}

typedef struct
{
	uint64_t revision[2];
	uint32_t count[2], material, flags;
	float uv, displacement;
	bool reverse, fail, invalid_direct;
} summary_fixture_t;

static uint64_t summary_revision (const entity_t *entity, void *user)
{
	return ((summary_fixture_t *)user)->revision[entity->skinnum];
}

static bool summary_append (rc_scene_t *scene, const entity_t *entity, const float matrix[16],
	uint32_t instance, rc_scene_material_fn material, void *user)
{
	(void)material;
	summary_fixture_t *f=user; unsigned kind=entity->skinnum;
	for (uint32_t i=0;i<f->count[kind];++i)
	{
		uint32_t source=kind && f->reverse ? f->count[kind]-i-1 : i;
		rc_scene_triangle_t triangle={0};
		for (unsigned v=0;v<3;++v)
		{
			triangle.position[v][0]=matrix[12]+source*2;
			triangle.position[v][1]=matrix[13];
			triangle.position[v][2]=matrix[14]+(kind ? f->displacement : 0);
		}
		triangle.position[1][0]+=1;triangle.position[2][1]+=1;
		triangle.metadata[0]=kind ? f->material : 0;
		triangle.metadata[1]=RC_SCENE_DYNAMIC|(kind ? f->flags : 0);
		triangle.metadata[2]=source;triangle.metadata[3]=instance;
		triangle.uv01[0]=kind ? f->uv : 0;
		if (!RCScene_AppendTriangle(scene,&triangle)) return false;
		if (f->fail && kind) return false;
	}
	/* Extractors may mutate their owned array without AppendTriangle. */
	if (f->invalid_direct && kind && scene->triangle_count) scene->triangles[0].position[0][0]=NAN;
	return true;
}

static uint64_t summary_fingerprint (const rc_scene_runtime_t *scene)
{
	float mins[3],maxs[3];uint64_t hash;
	assert(RCSceneRuntime_DynamicBounds(scene,mins,maxs,&hash));
	return hash;
}

static void test_dynamic_summaries (const qmodel_t *world)
{
	qmodel_t model={0};model.type=mod_alias;
	entity_t large={0},small={0};large.model=small.model=&model;small.skinnum=1;
	large.origin[2]=small.origin[2]=8;small.origin[0]=4;
	entity_t *candidates[]={&large,&small};float camera[3]={0};
	summary_fixture_t fixture={.revision={1,1},.count={32768,2},.material=5};
	rc_scene_runtime_t scene;RCSceneRuntime_Init(&scene);
	RCSceneRuntime_SetAliasRevision(&scene,summary_revision);
	assert(RCSceneRuntime_SetWorld(&scene,world,material_id,NULL));
	rc_scene_runtime_stats_t stats;
	assert(RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.summary_triangles_scanned==32770);
	uint64_t previous=summary_fingerprint(&scene);
	float mins[3],maxs[3];uint64_t same;
	assert(RCSceneRuntime_DynamicBounds(&scene,mins,maxs,&same) && mins[2]==8 && maxs[2]==8 && same==previous);
	/* Queries and unrelated tiny animation updates never scan the large
	 * static mesh. Counters make this deterministic, without time limits. */
	for(unsigned i=0;i<10;++i)assert(summary_fingerprint(&scene)==previous);
	assert(RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.cached==2 && stats.summary_triangles_scanned==0);
	/* An updated interpolation revision can emit exactly the prior bytes.
	 * Commit that revision without assembly, refit or a scene upload. */
	++fixture.revision[1];
	assert(RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.updated==1 && stats.cached==1 && !stats.refit && !stats.rebuilt);
	assert(!stats.geometry_changed && !stats.materials_changed && !stats.assembly_triangles_copied);
	assert(stats.cache_triangles_compared==2 && stats.summary_triangles_scanned==2);
	assert(summary_fingerprint(&scene)==previous);
	assert(RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.cached==2 && !stats.updated && !stats.cache_triangles_compared);
	/* A collector-derived flag on an unchanged assembled range must survive
	 * a different entity's small update and any failed transaction. */
	uint32_t static_count=scene.static_scene.triangle_count;
	scene.scene.triangles[static_count].metadata[1]|=RC_SCENE_EMISSIVE_PROXY;
	for(unsigned field=0;field<6;++field)
	{
		switch(field)
		{
		case 0:fixture.uv=.25f;break;
		case 1:fixture.material=3;break;
		case 2:fixture.flags=RC_SCENE_CUTOUT;break;
		case 3:fixture.reverse=true;break;
		case 4:fixture.count[1]=3;break;
		case 5:fixture.displacement=2;break;
		}
		++fixture.revision[1];
		assert(RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
		assert(stats.cached==1 && stats.updated==1 && stats.summary_triangles_scanned==fixture.count[1]);
		/* A changed count uses general assembly; stable ranges copy only the
		 * tiny changed snapshot, never the 32,768-triangle cached mesh. */
		assert(stats.assembly_triangles_copied==(field==4?32771u:fixture.count[1]));
		if(field<4) assert(scene.scene.triangles[static_count].metadata[1]&RC_SCENE_EMISSIVE_PROXY);
		uint64_t changed=summary_fingerprint(&scene);assert(changed!=previous);previous=changed;
	}
	assert(RCSceneRuntime_DynamicBounds(&scene,mins,maxs,&same) && mins[2]==8 && maxs[2]==10);
	small.origin[2]+=3;
	assert(RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.summary_triangles_scanned==3 && summary_fingerprint(&scene)!=previous);
	previous=summary_fingerprint(&scene);
	/* A direct-array malformed callback must be rejected before summarizing
	 * or replacing anything in the committed scene. */
	size_t committed_bytes=(size_t)scene.scene.triangle_count*sizeof(*scene.scene.triangles);
	rc_scene_triangle_t *committed=malloc(committed_bytes);assert(committed);
	memcpy(committed,scene.scene.triangles,committed_bytes);
	fixture.invalid_direct=true;++fixture.revision[1];
	assert(!RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(!memcmp(committed,scene.scene.triangles,committed_bytes) && summary_fingerprint(&scene)==previous);
	fixture.invalid_direct=false;--fixture.revision[1];
	/* Refit validation failure restores the actual assembled range, including
	 * derived flags, instead of restoring a stale raw cache copy. */
	uint32_t saved_escape=scene.scene.nodes[0].metadata[2];scene.scene.nodes[0].metadata[2]=0;
	fixture.displacement+=1;++fixture.revision[1];
	assert(!RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(!memcmp(committed,scene.scene.triangles,committed_bytes) && summary_fingerprint(&scene)==previous);
	scene.scene.nodes[0].metadata[2]=saved_escape;fixture.displacement-=1;--fixture.revision[1];free(committed);
	/* A failed extractor must leave the committed old summary intact. */
	fixture.fail=true;++fixture.revision[1];
	assert(!RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(summary_fingerprint(&scene)==previous);fixture.fail=false;
	--fixture.revision[1];
	candidates[0]=&small;candidates[1]=&large;
	assert(RCSceneRuntime_Update(&scene,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.summary_triangles_scanned==0 && summary_fingerprint(&scene)!=previous);
	previous=summary_fingerprint(&scene);
	assert(RCSceneRuntime_Update(&scene,candidates,1,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.summary_triangles_scanned==0 && summary_fingerprint(&scene)!=previous);
	/* Material remapping updates the cached summary even without a new pose. */
	radiance_quake_materials_t materials={0};materials.count=materials.capacity=6;
	materials.entries=calloc(materials.count,sizeof(*materials.entries));assert(materials.entries);
	previous=summary_fingerprint(&scene);bool compacted=false;
	assert(RCSceneRuntime_CompactMaterials(&scene,&materials,&compacted) && compacted);
	assert(summary_fingerprint(&scene)!=previous);previous=summary_fingerprint(&scene);
	assert(RCSceneRuntime_Update(&scene,candidates,1,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.summary_triangles_scanned==0 && summary_fingerprint(&scene)==previous);
	RCSceneRuntime_SetAliasSupport(&scene,alias_supported);
	assert(!scene.valid && summary_fingerprint(&scene)==previous);
	free(materials.entries);RCSceneRuntime_Clear(&scene);
	assert(!RCSceneRuntime_DynamicBounds(&scene,mins,maxs,&same));
}

static bool persistent_alias (const entity_t *entity,void *user)
{ (void)user;return entity->skinnum==0; }

static void test_persistent_base (const qmodel_t *world)
{
	qmodel_t model={0};model.type=mod_alias;
	entity_t large={0},small={0},nail={0};large.model=small.model=nail.model=&model;
	large.origin[0]=100;large.origin[2]=small.origin[2]=nail.origin[2]=8;
	small.skinnum=nail.skinnum=1;small.origin[0]=4;nail.origin[0]=20;
	entity_t *candidates[]={&large,&small,&nail};float camera[3]={0};
	summary_fixture_t fixture={.revision={1,1},.count={32768,4},.material=5};
	rc_scene_runtime_t runtime;RCSceneRuntime_Init(&runtime);
	RCSceneRuntime_SetAliasRevision(&runtime,summary_revision);
	RCSceneRuntime_SetAliasPersistent(&runtime,persistent_alias);
	assert(RCSceneRuntime_SetWorld(&runtime,world,material_id,NULL));rc_scene_runtime_stats_t stats;
	assert(RCSceneRuntime_Update(&runtime,candidates,2,camera,0,material_id,summary_append,&fixture,&stats));
	uint32_t bsp=runtime.static_scene.triangle_count,base_count=bsp+32768;
	assert(RCSceneRuntime_BaseTriangleCount(&runtime)==base_count && stats.base_content_changed && stats.base_geometry_changed);
	assert(stats.base_triangles_built==base_count && stats.mutable_triangles_built==4);
	uint64_t generation=RCSceneRuntime_BaseGeneration(&runtime);assert(generation);
	uint64_t content_generation=RCSceneRuntime_BaseContentGeneration(&runtime);assert(content_generation);
	const rc_scene_node_t *base_nodes=runtime.base_scene.nodes;
	const rc_scene_triangle_t *base_triangles=runtime.base_scene.triangles;
	runtime.scene.triangles[bsp].metadata[1]|=RC_SCENE_EMISSIVE_PROXY;
	size_t prefix_bytes=(size_t)base_count*sizeof(*runtime.scene.triangles);
	rc_scene_triangle_t *prefix=malloc(prefix_bytes);assert(prefix);memcpy(prefix,runtime.scene.triangles,prefix_bytes);
	for(unsigned iteration=0;iteration<10;++iteration)
	{
		++fixture.revision[1];fixture.displacement=iteration+1;
		uint32_t selected=2+(iteration&1);
		assert(RCSceneRuntime_Update(&runtime,candidates,selected,camera,0,material_id,summary_append,&fixture,&stats));
		assert(!stats.base_content_changed && !stats.base_geometry_changed && !stats.base_triangles_built);
		assert(stats.mutable_triangles_built==4*(selected-1) && stats.assembly_triangles_copied==4*(selected-1));
		assert(runtime.base_scene.nodes==base_nodes && runtime.base_scene.triangles==base_triangles);
		assert(RCSceneRuntime_BaseGeneration(&runtime)==generation && !memcmp(prefix,runtime.scene.triangles,prefix_bytes));
		assert(RCSceneRuntime_BaseContentGeneration(&runtime)==content_generation);
		float down[3]={0,0,-1},o[3]={100.25f,.25f,50};rc_scene_hit_t hit;
		assert(RCScene_TraceReference(&runtime.scene,o,down,0,100,NULL,NULL,&hit));
		assert(hit.instance==0 && hit.material==0 && (hit.flags&RC_SCENE_DYNAMIC));
		o[0]=4.25f;assert(RCScene_TraceReference(&runtime.scene,o,down,0,100,NULL,NULL,&hit));
		assert(hit.instance==1 && hit.material==5 && fabsf(hit.position[2]-(8+fixture.displacement))<1e-5f);
		/* Independent monolithic BVH gives the same hit/UV for both trees. */
		rc_scene_t reference={0};
		for(uint32_t t=0;t<runtime.scene.triangle_count;++t) assert(RCScene_AppendTriangle(&reference,&runtime.scene.triangles[t]));
		assert(RCScene_Build(&reference));
		for(unsigned ray=0;ray<16;++ray)
		{
			o[0]=100.25f+ray*2;rc_scene_hit_t expected;
			assert(RCScene_TraceReference(&reference,o,down,0,100,NULL,NULL,&expected));
			assert(RCScene_TraceReference(&runtime.scene,o,down,0,100,NULL,NULL,&hit));
			assert(hit.material==expected.material && hit.instance==expected.instance && hit.distance==expected.distance);
		}
		RCScene_Clear(&reference);
	}
	/* Invalid mutable extraction leaves the complete base, forest and epoch. */
	uint32_t old_count=runtime.scene.triangle_count;fixture.invalid_direct=true;++fixture.revision[1];
	assert(!RCSceneRuntime_Update(&runtime,candidates,3,camera,0,material_id,summary_append,&fixture,&stats));
	assert(runtime.scene.triangle_count==old_count && RCSceneRuntime_BaseGeneration(&runtime)==generation);
	assert(!memcmp(prefix,runtime.scene.triangles,prefix_bytes));fixture.invalid_direct=false;--fixture.revision[1];
	/* A persistent transform rebuilds the base once; identity-only refresh
	 * with byte-identical output leaves that geometry generation unchanged. */
	large.origin[2]=10;
	assert(RCSceneRuntime_Update(&runtime,candidates,3,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.base_geometry_changed && stats.base_triangles_built==base_count && !stats.mutable_triangles_built);
	generation=RCSceneRuntime_BaseGeneration(&runtime);++large.render_generation;
	assert(RCSceneRuntime_Update(&runtime,candidates,3,camera,0,material_id,summary_append,&fixture,&stats));
	assert(!stats.base_content_changed && !stats.base_geometry_changed && RCSceneRuntime_BaseGeneration(&runtime)==generation);
	/* Reordering candidate IDs changes metadata, preserving base geometry. */
	candidates[0]=&small;candidates[1]=&large;
	assert(RCSceneRuntime_Update(&runtime,candidates,3,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.base_content_changed && !stats.base_geometry_changed && RCSceneRuntime_BaseGeneration(&runtime)==generation);
	assert(runtime.scene.triangles[bsp].metadata[3]==1);
	content_generation=RCSceneRuntime_BaseContentGeneration(&runtime);
	/* Every retained representation must follow material compaction. */
	radiance_quake_materials_t materials={0};materials.count=materials.capacity=6;
	materials.entries=calloc(6,sizeof(*materials.entries));assert(materials.entries);bool compacted=false;
	assert(RCSceneRuntime_CompactMaterials(&runtime,&materials,&compacted) && compacted);
	assert(runtime.base_scene.triangles[0].metadata[0]==1 && runtime.mutable_scene.triangles[0].metadata[0]==2);
	assert(runtime.scene.triangles[0].metadata[0]==1);free(materials.entries);
	assert(RCSceneRuntime_BaseContentGeneration(&runtime)!=content_generation);
	/* Removing a trailing unused nail material cannot invalidate unchanged
	 * base emitter/source caches: no actual base ID is remapped. */
	content_generation=RCSceneRuntime_BaseContentGeneration(&runtime);
	materials.count=materials.capacity=4;materials.entries=calloc(4,sizeof(*materials.entries));assert(materials.entries);
	assert(RCSceneRuntime_CompactMaterials(&runtime,&materials,&compacted) && compacted);
	assert(RCSceneRuntime_BaseContentGeneration(&runtime)==content_generation);free(materials.entries);
	/* A pinned skin keeps its ID with no triangles, so a projectile that later
	 * uses it changes neither the registry nor the atlas. */
	materials.count=materials.capacity=4;materials.entries=calloc(4,sizeof(*materials.entries));assert(materials.entries);
	materials.entries[3].pinned=true;
	assert(RCSceneRuntime_CompactMaterials(&runtime,&materials,&compacted) && !compacted && materials.count==4 && materials.entries[3].pinned);
	free(materials.entries);
	/* Alpha/removal/restoration invalidate the prefix, rather than retaining
	 * invisible persistent casters. All geometry remains selected normally. */
	large.alpha=128;
	assert(RCSceneRuntime_Update(&runtime,candidates,3,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.base_geometry_changed && RCSceneRuntime_BaseTriangleCount(&runtime)==bsp);
	large.alpha=ENTALPHA_DEFAULT;
	assert(RCSceneRuntime_Update(&runtime,candidates,3,camera,0,material_id,summary_append,&fixture,&stats));
	assert(stats.base_geometry_changed && RCSceneRuntime_BaseTriangleCount(&runtime)==base_count);
	free(prefix);RCSceneRuntime_Clear(&runtime);
}

int main (void)
{
	texture_t texture = {0}; mtexinfo_t texinfo = {0}; texinfo.texture = &texture;
	glpoly_t poly = {0}; poly.numverts=4;
	const float vertices[4][3]={{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
	for (int i=0;i<4;++i) for (int a=0;a<3;++a) poly.verts[i][a]=vertices[i][a];
	msurface_t surface = {0}; surface.texinfo=&texinfo; surface.polys=&poly;
	qmodel_t world = {0}, prop = {0};
	world.type=prop.type=mod_brush; world.surfaces=prop.surfaces=&surface;
	world.numsurfaces=prop.numsurfaces=1; world.nummodelsurfaces=prop.nummodelsurfaces=1;
	test_dynamic_summaries(&world);
	{
		entity_t compiled[300]={0},ordinary[300]={0};qsm_instance_t marker={0};
		entity_t *statics[300],*visible[600],*assembled[900];
		for(unsigned i=0;i<300;++i) {
			compiled[i].model=ordinary[i].model=&prop;compiled[i].compiled_instance=&marker;compiled[i].is_static=true;
			ordinary[i].origin[0]=(float)i*4;ordinary[i].netstate.scale=ENTSCALE_DEFAULT;
			statics[i]=visible[i]=&compiled[i];visible[300+i]=&ordinary[i];
		}
		uint32_t count=0;
		assert(RCSceneRuntime_AssembleCandidates(assembled,900,statics,300,visible,600,&count) && count==300);
		for(unsigned i=0;i<count;++i)assert(!assembled[i]->compiled_instance);
		rc_scene_runtime_t scene;RCSceneRuntime_Init(&scene);float camera[3]={0};rc_scene_runtime_stats_t stats;
		assert(RCSceneRuntime_SetWorld(&scene,&world,material_id,NULL));
		assert(RCSceneRuntime_Update(&scene,assembled,count,camera,0,material_id,NULL,NULL,&stats));
		assert(stats.selected==256 && stats.omitted_limit==44);
		RCSceneRuntime_Clear(&scene);
	}
	test_persistent_base(&world);
	/* A static outside the renderer's PVS still occludes radiance rays. A
	 * visible static must not be duplicated, and entering/leaving the visible
	 * list must not invalidate the unchanged scene. Particle replacements
	 * are not geometry. */
	{
		entity_t hidden = {0}, shown = {0}, replacement = {0}, dynamic = {0};
		qmodel_t particles = prop; particles.flags |= MOD_EMITREPLACE; particles.emiteffect = 1;
		hidden.is_static = shown.is_static = replacement.is_static = true;
		hidden.model = shown.model = dynamic.model = &prop; replacement.model = &particles;
		hidden.origin[0] = 4; shown.origin[0] = 8; dynamic.origin[0] = 12;
		entity_t *statics[] = {&hidden, &shown, &replacement};
		entity_t *visible[] = {&shown, &dynamic};
		entity_t *assembled[5] = {0}; uint32_t n = 99;
		assert (!RCSceneRuntime_AssembleCandidates (assembled, 4, statics, 3, visible, 2, &n) && n == 99);
		assert (!RCSceneRuntime_AssembleCandidates (assembled, UINT32_MAX, statics, UINT32_MAX, visible, 1, &n) && n == 99);
		assert (!RCSceneRuntime_AssembleCandidates (assembled, 5, NULL, 3, visible, 2, &n) && n == 99);
		assert (RCSceneRuntime_AssembleCandidates (assembled, 5, statics, 3, visible, 2, &n));
		assert (n == 3 && assembled[0] == &hidden && assembled[1] == &shown && assembled[2] == &dynamic);
		rc_scene_runtime_t scene; RCSceneRuntime_Init (&scene);
		assert (RCSceneRuntime_SetWorld (&scene, &world, material_id, NULL));
		float camera[3] = {0,0,2}, origin[3] = {4.25f,.25f,1}, direction[3] = {0,0,-1};
		rc_scene_runtime_stats_t stats; rc_scene_hit_t hit;
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, NULL, NULL, &stats));
		assert (stats.selected == 3 && RCScene_TraceReference (&scene.scene, origin, direction, 0, 2, NULL, NULL, &hit) && hit.instance == 0);
		entity_t *new_visible[] = {&hidden, &dynamic};
		assert (RCSceneRuntime_AssembleCandidates (assembled, 5, statics, 3, new_visible, 2, &n));
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, NULL, NULL, &stats) && !stats.rebuilt && !stats.refit);
		/* Invisible and translucent models cannot cast opaque shadows. */
		shown.alpha = ENTALPHA_ZERO;
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, NULL, NULL, &stats));
		assert (stats.selected == 2 && stats.omitted_transparent == 1);
		origin[0] = 8.25f;
		assert (!RCScene_TraceReference (&scene.scene, origin, direction, 0, 2, NULL, NULL, &hit));
		shown.alpha = 128;
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, NULL, NULL, &stats));
		assert (stats.selected == 2 && stats.omitted_transparent == 1);
		shown.alpha = ENTALPHA_DEFAULT;
		/* Explicit entity particle effects override model replacement. */
		replacement.netstate.emiteffectnum = 1;
		assert (RCSceneRuntime_AssembleCandidates (assembled, 5, statics, 3, new_visible, 2, &n) && n == 4 && assembled[2] == &replacement);
		/* A map reset releases old entity bindings before addresses can be
		 * reused; the next map must contain only its own static candidates. */
		qmodel_t next_world = world;
		assert (RCSceneRuntime_SetWorld (&scene, &next_world, material_id, NULL));
		entity_t *next_statics[] = {&shown};
		assert (RCSceneRuntime_AssembleCandidates (assembled, 5, next_statics, 1, NULL, 0, &n) && n == 1);
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, NULL, NULL, &stats) && stats.updated == 1);
		origin[0] = 4.25f;
		assert (!RCScene_TraceReference (&scene.scene, origin, direction, 0, 2, NULL, NULL, &hit));
		origin[0] = 8.25f;
		assert (RCScene_TraceReference (&scene.scene, origin, direction, 0, 2, NULL, NULL, &hit));
		assert (RCSceneRuntime_AssembleCandidates (NULL, 0, NULL, 0, NULL, 0, &n) && n == 0);
		RCSceneRuntime_Clear (&scene);
	}
	/* Stable cache order is common, but reordering and duplicate candidate
	 * pointers must still match the first unclaimed old entry. */
	{
		entity_t first = {0}, second = {0};
		first.model = second.model = &prop; first.origin[0] = 4; second.origin[0] = 12;
		entity_t *candidates[] = {&first, &first, &second};
		rc_scene_runtime_t scene; RCSceneRuntime_Init (&scene);
		assert (RCSceneRuntime_SetWorld (&scene, &world, material_id, NULL));
		float camera[3] = {0,0,2}, ray[3] = {12.25f,.25f,1}, down[3] = {0,0,-1};
		rc_scene_runtime_stats_t stats; rc_scene_hit_t hit;
		assert (RCSceneRuntime_Update (&scene, candidates, 3, camera, 0, material_id, NULL, NULL, &stats));
		assert (stats.updated == 3);
		assert (RCSceneRuntime_Update (&scene, candidates, 3, camera, 0, material_id, NULL, NULL, &stats));
		assert (stats.cached == 3 && stats.updated == 0 && !stats.refit && !stats.rebuilt);
		candidates[0] = &second; candidates[2] = &first;
		assert (RCSceneRuntime_Update (&scene, candidates, 3, camera, 0, material_id, NULL, NULL, &stats));
		assert (stats.cached == 3 && stats.updated == 0);
		assert (RCScene_TraceReference (&scene.scene, ray, down, 0, 2, NULL, NULL, &hit) && hit.instance == 0);
		candidates[0] = &first; candidates[2] = &second;
		assert (RCSceneRuntime_Update (&scene, candidates, 3, camera, 0, material_id, NULL, NULL, &stats));
		assert (stats.cached == 3 && stats.updated == 0);
		assert (RCScene_TraceReference (&scene.scene, ray, down, 0, 2, NULL, NULL, &hit) && hit.instance == 2);
		RCSceneRuntime_Clear (&scene);
	}
	/* Batched alias entities really become shadow casters, including after a
	 * move. When the batch exceeds the shared cap, a closer instance replaces
	 * a farther one and ordinary brush entities retain their priority. */
	{
		qmodel_t alias_batch_model = {0}; alias_batch_model.type = mod_alias;
		entity_t batch[300] = {0}, brush = {0}; entity_t *assembled[301];
		for (int i = 0; i < 300; ++i)
		{
			batch[i].model = &alias_batch_model; batch[i].netstate.scale = ENTSCALE_DEFAULT;
			batch[i].origin[0] = 4.0f + 2.0f * i; batch[i].origin[2] = 8.0f;
		}
		brush.model = &prop; brush.origin[0] = 2000; brush.netstate.scale = ENTSCALE_DEFAULT;
		entity_t *visible[] = {&brush}; uint32_t n = 0;
		assert (RCSceneRuntime_AssembleCandidates (assembled, 301, NULL, 0, visible, 1, &n) && n == 1);
		assert (RCSceneRuntime_AppendBatch (assembled, 301, &n, batch, 3) && n == 4);
		rc_scene_runtime_t scene; RCSceneRuntime_Init (&scene);
		RCSceneRuntime_SetAliasSupport (&scene, alias_supported);
		assert (RCSceneRuntime_SetWorld (&scene, &world, material_id, NULL));
		float camera[3] = {0,0,10}, ray[3] = {4.25f,.25f,10}, down[3] = {0,0,-1};
		rc_scene_runtime_stats_t stats; rc_scene_hit_t hit;
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, batch_alias_append, NULL, &stats));
		assert (stats.selected == 4 && stats.omitted_limit == 0);
		assert (RCSceneRuntime_CountBatchSelected (&scene, batch, 3) == 3);
		assert (RCScene_TraceReference (&scene.scene, ray, down, 0, 5, NULL, NULL, &hit) && hit.instance == 1 && (hit.flags & RC_SCENE_DYNAMIC));
		batch[0].origin[0] = 20; ray[0] = 20.25f;
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, batch_alias_append, NULL, &stats));
		assert (RCScene_TraceReference (&scene.scene, ray, down, 0, 5, NULL, NULL, &hit) && hit.instance == 1);
		ray[0] = 4.25f;
		assert (!RCScene_TraceReference (&scene.scene, ray, down, 0, 5, NULL, NULL, &hit));
		batch[0].origin[0] = 4;
		assert (RCSceneRuntime_AssembleCandidates (assembled, 301, NULL, 0, visible, 1, &n));
		assert (RCSceneRuntime_AppendBatch (assembled, 301, &n, batch, 300) && n == 301);
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, batch_alias_append, NULL, &stats));
		assert (stats.considered == 301 && stats.selected == 256 && stats.omitted_limit == 45);
		assert (RCSceneRuntime_CountBatchSelected (&scene, batch, 300) == 255);
		ray[0] = batch[254].origin[0] + .25f;
		assert (RCScene_TraceReference (&scene.scene, ray, down, 0, 5, NULL, NULL, &hit) && hit.instance == 255);
		ray[0] = batch[255].origin[0] + .25f;
		assert (!RCScene_TraceReference (&scene.scene, ray, down, 0, 5, NULL, NULL, &hit));
		batch[299].origin[0] = 1; ray[0] = 1.25f;
		assert (RCSceneRuntime_Update (&scene, assembled, n, camera, 0, material_id, batch_alias_append, NULL, &stats));
		assert (stats.selected == 256 && stats.omitted_limit == 45);
		assert (RCScene_TraceReference (&scene.scene, ray, down, 0, 5, NULL, NULL, &hit) && hit.instance == 300);
		ray[0] = batch[254].origin[0] + .25f;
		assert (!RCScene_TraceReference (&scene.scene, ray, down, 0, 5, NULL, NULL, &hit));
		RCSceneRuntime_Clear (&scene);
	}
	entity_t entities[300] = {0}; entity_t *candidates[300];
	for (int i=0;i<300;++i)
	{
		entities[i].model=&prop; entities[i].origin[0]=(float)(i+1)*2;
		entities[i].netstate.scale=ENTSCALE_DEFAULT;
	}
	/* ECS visual batches do not occur in the ordinary visible list. They
	 * compete for the same budget; rejecting insufficient storage leaves the
	 * previously assembled prefix intact. */
	uint32_t batch_candidates = 0;
	assert (!RCSceneRuntime_AppendBatch (candidates, 299, &batch_candidates, entities, 300) && batch_candidates == 0);
	assert (!RCSceneRuntime_AppendBatch (candidates, 300, &batch_candidates, NULL, 1) && batch_candidates == 0);
	assert (RCSceneRuntime_AppendBatch (candidates, 300, &batch_candidates, entities, 300) && batch_candidates == 300);
	for (int i=0; i<300; ++i) assert (candidates[i] == &entities[i]);
	rc_scene_runtime_t runtime; RCSceneRuntime_Init (&runtime);
	assert (RCSceneRuntime_SetWorld (&runtime,&world,material_id,NULL));
	float camera[3]={0,0,2}; rc_scene_runtime_stats_t stats;
	assert (RCSceneRuntime_Update (&runtime,candidates,2,camera,1,material_id,NULL,NULL,&stats));
	assert (stats.selected==2 && stats.omitted_limit==0);
	assert (RCSceneRuntime_Update (&runtime,candidates,300,camera,1,material_id,NULL,NULL,&stats));
	assert (stats.rebuilt && stats.considered==300 && stats.selected==256 && stats.omitted_limit==44);
	assert (runtime.scene.triangle_count==514 && runtime.scene.node_count>0);
	float origin[3]={512.25f,.25f,1}, direction[3]={0,0,-1}; rc_scene_hit_t hit;
	assert (RCScene_TraceReference (&runtime.scene,origin,direction,0,2,NULL,NULL,&hit) && hit.instance==255);
	origin[0]=514.25f;
	assert (!RCScene_TraceReference (&runtime.scene,origin,direction,0,2,NULL,NULL,&hit));
	assert (RCSceneRuntime_Update (&runtime,candidates,300,camera,1,material_id,NULL,NULL,&stats) && !stats.rebuilt);
	const rc_scene_node_t *old_nodes = runtime.scene.nodes;
	entities[10].origin[1]=3;
	assert (RCSceneRuntime_Update (&runtime,candidates,300,camera,1,material_id,NULL,NULL,&stats));
	assert (stats.refit && stats.geometry_changed && !stats.rebuilt && runtime.scene.nodes==old_nodes);
	float moved[3]={22.25f,3.25f,1};
	assert (RCScene_TraceReference (&runtime.scene,moved,direction,0,2,NULL,NULL,&hit) && hit.instance==10);
	float moved_min[3]={22,3,0}, moved_max[3]={23,4,0};
	assert (RCScene_AnyGeometryAABB (&runtime.scene,moved_min,moved_max));
	entities[299].origin[0]=0.5f;
	assert (RCSceneRuntime_Update (&runtime,candidates,300,camera,1,material_id,NULL,NULL,&stats) && stats.refit && stats.geometry_changed);
	origin[0]=.75f;
	assert (RCScene_TraceReference (&runtime.scene,origin,direction,0,2,NULL,NULL,&hit));
	assert (RCSceneRuntime_Update (&runtime,candidates,300,camera,2,material_id,NULL,NULL,&stats) && !stats.refit && !stats.geometry_changed);
	entities[299].alpha=128;
	assert (RCSceneRuntime_Update (&runtime,candidates,300,camera,2,material_id,NULL,NULL,&stats));
	assert (stats.omitted_transparent==1 && stats.selected==256 && stats.omitted_limit==43);
	RCSceneRuntime_Clear (&runtime);

	/* Exclude known unsupported aliases before applying the selection budget.
	 * An unexpected later extraction failure must retain the prior scene. */
	RCSceneRuntime_Init (&runtime);
	RCSceneRuntime_SetAliasSupport (&runtime, alias_supported);
	assert (RCSceneRuntime_SetWorld (&runtime,&world,material_id,NULL));
	qmodel_t alias_model = {0}; alias_model.type=mod_alias;
	entities[0].model=entities[1].model=&alias_model;
	entities[0].frame=entities[1].frame=1; entities[299].alpha=ENTALPHA_DEFAULT;
	assert (RCSceneRuntime_Update (&runtime,candidates,300,camera,3,material_id,alias_append,NULL,&stats));
	assert (stats.considered==300 && stats.selected==256 && stats.omitted_unsupported==2 && stats.omitted_limit==42);
	assert (runtime.scene.triangle_count==514);
	RCSceneRuntime_Clear (&runtime);
	RCSceneRuntime_Init (&runtime);
	RCSceneRuntime_SetAliasSupport (&runtime, alias_supported);
	assert (RCSceneRuntime_SetWorld (&runtime,&world,material_id,NULL));
	entity_t pair[2] = {0}; entity_t *pair_ptr[2] = {&pair[0], &pair[1]};
	pair[0].model=&prop; pair[1].model=&alias_model;
	pair[0].netstate.scale=pair[1].netstate.scale=ENTSCALE_DEFAULT;
	pair[1].frame=1;
	assert (RCSceneRuntime_Update (&runtime,pair_ptr,2,camera,3,material_id,alias_append,NULL,&stats));
	assert (stats.considered==2 && stats.selected==1 && stats.omitted_unsupported==1 && runtime.scene.triangle_count==4);
	pair[1].frame=2;
	assert (!RCSceneRuntime_Update (&runtime,pair_ptr,2,camera,3,material_id,alias_append,NULL,&stats));
	assert (runtime.scene.triangle_count==4);
	pair[1].frame=0;
	assert (RCSceneRuntime_Update (&runtime,pair_ptr,2,camera,3,material_id,alias_append,NULL,&stats));
	assert (stats.selected==2 && !stats.omitted_unsupported && runtime.scene.triangle_count==5);
	RCSceneRuntime_Clear (&runtime);
	/* Per-entity cache budgets interpolation changes while preserving urgent
	 * identity/position changes and removals. Every continuously dirty entry
	 * eventually receives an update, and extraction failures are transactional. */
	RCSceneRuntime_Init (&runtime);
	RCSceneRuntime_SetAliasRevision (&runtime, alias_revision);
	RCSceneRuntime_SetAliasBudget (&runtime, 1);
	assert (RCSceneRuntime_SetWorld (&runtime,&world,material_id,NULL));
	entity_t animated[4] = {0}; entity_t *animated_ptr[4];
	for (int i=0; i<4; ++i) { animated_ptr[i]=&animated[i]; animated[i].model=&alias_model; animated[i].origin[0]=i*10; animated[i].netstate.scale=ENTSCALE_DEFAULT; }
	alias_material=3;
	assert (RCSceneRuntime_Update (&runtime,animated_ptr,4,camera,1,material_id,alias_append,NULL,&stats));
	assert (stats.updated==4 && !stats.deferred && runtime.scene.triangle_count==6);
	/* Removed material sources must not remain in the atlas: stale texture
	 * pointers can have been freed or recycled. The assembled scene, static
	 * world and retained per-entity geometry must agree after compaction. */
	radiance_quake_materials_t materials = {0};
	texture_t obsolete[3] = {0};
	materials.entries = calloc (5, sizeof (*materials.entries)); assert (materials.entries);
	materials.count = materials.capacity = 5;
	for (int i = 0; i < 3; ++i) materials.entries[i].brush = &obsolete[i];
	materials.entries[4].brush = &texture;
	bool compacted = false;
	assert (RCSceneRuntime_CompactMaterials (&runtime, &materials, &compacted) && compacted);
	assert (materials.count == 2 && materials.entries[1].brush == &texture);
	assert (runtime.static_scene.triangles[0].metadata[0] == 1 && runtime.scene.triangles[0].metadata[0] == 1);
	for (int i = 0; i < 4; ++i) assert (runtime.scene.triangles[2+i].metadata[0] == 0);
	alias_material=0; /* Future extractions use the compacted material ID. */
	assert (RCSceneRuntime_CompactMaterials (&runtime, &materials, &compacted) && !compacted);
	/* A corrupt cached ID fails without mutating either scene or registry. */
	runtime.static_scene.triangles[0].metadata[0] = 3;
	assert (!RCSceneRuntime_CompactMaterials (&runtime, &materials, &compacted));
	assert (!compacted && materials.count == 2 && runtime.scene.triangles[0].metadata[0] == 1);
	runtime.static_scene.triangles[0].metadata[0] = 1;
	for (int i=0; i<4; ++i) animated[i].syncbase=1;
	for (int frame=0; frame<4; ++frame)
	{
		assert (RCSceneRuntime_Update (&runtime,animated_ptr,4,camera,1,material_id,alias_append,NULL,&stats));
		assert (stats.updated==1 && stats.deferred==(uint32_t)(3-frame) && !stats.materials_changed);
	}
	for (int i=0; i<4; ++i) assert (runtime.scene.triangles[2+i].position[0][0] == i*10+1);
	for (int i=0; i<4; ++i) assert (runtime.scene.triangles[2+i].metadata[0] == 0);
	/* A protocol slot can be removed and reused between two rendered frames.
	 * Its pointer, model and transform can all match the old occupant. The
	 * generation must force immediate extraction even with a zero pose budget. */
	RCSceneRuntime_SetAliasBudget (&runtime, 0);
	++animated[0].render_generation; animated[0].syncbase=9;
	assert (RCSceneRuntime_Update (&runtime,animated_ptr,4,camera,1,material_id,alias_append,NULL,&stats));
	assert (stats.updated==1 && !stats.deferred && runtime.scene.triangles[2].position[0][0]==9);
	/* Moving/reusing a slot is immediate even when its pose budget is zero. */
	animated[0].syncbase=1;
	animated[0].origin[0]=42; animated[1].syncbase=2;
	assert (RCSceneRuntime_Update (&runtime,animated_ptr,4,camera,1,material_id,alias_append,NULL,&stats));
	assert (stats.updated==1 && stats.deferred==1 && runtime.scene.triangles[2].position[0][0]==43);
	animated_ptr[0]=NULL;
	assert (RCSceneRuntime_Update (&runtime,animated_ptr,4,camera,1,material_id,alias_append,NULL,&stats));
	assert (runtime.scene.triangle_count==5 && stats.selected==3);
	animated[0].origin[0]=80; animated_ptr[0]=&animated[0];
	assert (RCSceneRuntime_Update (&runtime,animated_ptr,4,camera,1,material_id,alias_append,NULL,&stats));
	assert (stats.updated==1 && runtime.scene.triangles[2].position[0][0]==81);
	free (materials.entries);
	RCSceneRuntime_Clear (&runtime);
	puts ("PASS: static cache, bounded nearest selection, dynamic rebuild and transparent omission");
	return 0;
}
