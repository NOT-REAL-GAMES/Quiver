#include "../Quake/quakedef.h"
#include "../Quake/radiance_scene_alias.h"
#include "../Quake/radiance_alias.h"
#include <assert.h>
#include <stdio.h>

client_state_t cl;
viddef_t vid;
cvar_t gl_fullbrights = {.name = "gl_fullbrights", .string = "0", .flags = CVAR_NONE};
cvar_t gl_nocolors = {.name = "gl_nocolors", .string = "0", .flags = CVAR_NONE};
gltexture_t *playertextures[MAX_SCOREBOARD];
static qmodel_t *compiled_test_model;
qmodel_t *Mod_ForName(const char *name,qboolean crash)
{ (void)name;(void)crash;return compiled_test_model; }
void *Mod_Extradata_CheckSkin (qmodel_t *model, int skin)
{
	(void)skin; return model->extradata[PV_MD5] ? model->extradata[PV_MD5] : model->extradata[PV_QUAKE1];
}
void *Mod_Extradata_Exact (qmodel_t *model, int skin)
{ return Mod_Extradata_CheckSkin(model,skin); }
void R_SetupAliasFrame (const entity_t *entity, aliashdr_t *hdr, lerpdata_t *lerp)
{
	(void)entity; lerp->pose1=0;
	const bool skeletal = hdr->poseverttype == PV_MD5 || hdr->poseverttype == PV_MD5_8;
	lerp->pose2=skeletal ? 0 : 1; lerp->blend=skeletal ? 0 : .5f;
}
void R_GetEntityLerpedTransform (const entity_t *entity, vec3_t origin, vec3_t angles)
{
	(void)entity; origin[0]=30; origin[1]=40; origin[2]=50; angles[0]=angles[1]=angles[2]=0;
}
void IdentityMatrix (float m[16]) { memset (m,0,16*sizeof(*m)); m[0]=m[5]=m[10]=m[15]=1; }
void R_RotateForEntityScale (float m[16], vec3_t origin, vec3_t angles, float scale)
{
	(void)angles;for(unsigned a=0;a<3;++a)m[a*5]*=scale;
	m[12]=origin[0]; m[13]=origin[1]; m[14]=origin[2];
}

static uint32_t material_for_skin (const entity_t *entity, uint32_t surface,
	const struct gltexture_s *texture, const struct gltexture_s *fullbright, void *user)
{
	(void)entity; (void)texture; (void)fullbright;
	return surface == *(const uint32_t *)user ? UINT32_MAX : 100 + surface;
}

static rc_alias_mesh_t *static_md5_mesh_count (unsigned surface, bool eight_weights, unsigned triangles)
{
	md5vert_t vertices[3] = {0}; md5vert8_t vertices8[3] = {0};
	for (unsigned i=0; i<3; ++i)
	{
		/* Bind positions are deliberately different from the rendered pose. */
		vertices[i].xyz[0]=vertices8[i].xyz[0]=5000;
		vertices[i].joint_weights[0]=vertices8[i].joint_weights[0]=255;
		vertices[i].st[0]=vertices8[i].st[0]=i==1;
		vertices[i].st[1]=vertices8[i].st[1]=i==2;
		vertices[i].joint_position_x[0]=vertices8[i].joint_position_x[0]=i==1;
		vertices[i].joint_position_y[0]=vertices8[i].joint_position_y[0]=i==2;
	}
	float joints[12]={1,0,0,2,0,1,0,3,0,0,1,4+(float)surface};
	uint16_t *indices=malloc((size_t)triangles*3*sizeof(*indices));assert(indices);
	for(unsigned i=0;i<triangles*3;++i)indices[i]=i%3;
	rc_alias_mesh_t *mesh=RCAlias_CreateSkinnedStatic (3,triangles*3,eight_weights ? (void *)vertices8 : (void *)vertices,
		eight_weights ? sizeof (*vertices8) : sizeof (*vertices), eight_weights ? 8 : 4,1,joints,indices);
	free(indices);return mesh;
}

static rc_alias_mesh_t *static_md5_mesh (unsigned surface, bool eight_weights)
{ return static_md5_mesh_count(surface,eight_weights,1); }

static void test_static_md5_surfaces (gltexture_t *texture)
{
	/* Match the Sponza export's 26-surface chain; both skeletal layouts must
	 * enter ordinary per-entity extraction without using the ECS batch path. */
	aliashdr_t surfaces[26]={0};
	for (unsigned i=0; i<countof(surfaces); ++i)
	{
		aliashdr_t *hdr=&surfaces[i];
		hdr->poseverttype=i&1 ? PV_MD5_8 : PV_MD5;
		hdr->numframes=hdr->numposes=hdr->numjoints=hdr->numskins=1;
		if (i+1<countof(surfaces)) hdr->nextsurface=&surfaces[i+1];
		for (unsigned j=0; j<MAX_FRAMEGROUPS; ++j) hdr->gltextures[0][j]=texture;
		hdr->radiance_mesh=static_md5_mesh(i,i&1); assert(hdr->radiance_mesh);
	}
	qmodel_t model={0}; model.type=mod_alias; model.extradata[PV_MD5]=(byte *)surfaces;
	entity_t entity={0}; entity.model=&model;entity.netstate.scale=ENTSCALE_DEFAULT;
	rc_scene_t scene; RCScene_Init(&scene); uint32_t no_failure=UINT32_MAX;
	assert(RCScene_AliasSupported(&entity));
	assert(!RCScene_AliasPersistent(&entity));
	qsm_instance_t compiled_instance={0};entity.compiled_instance=&compiled_instance;
	assert(RCScene_AliasPersistent(&entity));
	surfaces[0].poseverttype=PV_QUAKE1;surfaces[1].poseverttype=PV_QUAKE3;
	assert(RCScene_AliasPersistent(&entity));
	surfaces[1].numposes=2;assert(!RCScene_AliasPersistent(&entity));surfaces[1].numposes=1;
	surfaces[0].poseverttype=PV_MD5;surfaces[1].poseverttype=PV_MD5_8;entity.compiled_instance=NULL;
	/* Compiled placements are world geometry rather than bounded dynamic
	 * candidates. Retain more than256 complete multi-surface instances. */
	qsm_instance_t *instances=calloc(300,sizeof(*instances));assert(instances);
	for(unsigned i=0;i<300;++i) {instances[i].origin[0]=(float)i*8;instances[i].modelscale=1.375f;instances[i].triangle_count=26;}
	qsm_data_t data={.instances=instances,.instance_count=300,.triangle_count=7800};
	qmodel_t world={0};world.type=mod_brush;world.static_models=&data;compiled_test_model=&model;
	rc_scene_t compiled;RCScene_Init(&compiled);
	assert(RCScene_AppendCompiledWorld(&compiled,&world,material_for_skin,&no_failure));
	assert(compiled.triangle_count==7800 && compiled.triangles[7799].metadata[3]==300);
	for(unsigned i=0;i<compiled.triangle_count;++i)assert(!(compiled.triangles[i].metadata[1]&RC_SCENE_DYNAMIC));
	assert(RCScene_Build(&compiled));
	float test_origin[3]={299*8+3.0f,4.375f,100},test_direction[3]={0,0,-1};rc_scene_hit_t test_hit;
	assert(RCScene_TraceReference(&compiled,test_origin,test_direction,0,100,NULL,NULL,&test_hit));
	assert(test_hit.instance==300 && test_hit.surface==25 && fabsf(test_hit.distance-60.125f)<1e-4f);
	instances[1].modelscale=0;
	assert(!RCScene_AppendCompiledWorld(&compiled,&world,material_for_skin,&no_failure) && compiled.triangle_count==7800);
	RCScene_Clear(&compiled);free(instances);compiled_test_model=NULL;
	assert(RCScene_AppendAliasLerped(&scene,&entity,73,material_for_skin,&no_failure));
	assert(scene.triangle_count==countof(surfaces));
	for (unsigned i=0; i<countof(surfaces); ++i)
	{
		const rc_scene_triangle_t *tri=&scene.triangles[i];
		assert(tri->metadata[0]==100+i && tri->metadata[2]==i && tri->metadata[3]==73);
		assert(tri->position[0][0]==32 && tri->position[0][1]==43 && tri->position[0][2]==54+(float)i);
	}
	assert(RCScene_Build(&scene));
	float origin[3]={32.25f,43.25f,100}, direction[3]={0,0,-1}; rc_scene_hit_t hit;
	assert(RCScene_TraceReference(&scene,origin,direction,0,100,NULL,NULL,&hit));
	assert(hit.surface==25 && hit.distance==21);
	float matrix[16]; IdentityMatrix(matrix);
	uint32_t fail_on_last=25;
	assert(!RCScene_AppendAlias(&scene,&entity,matrix,0,0,0,0,73,material_for_skin,&fail_on_last));
	assert(scene.triangle_count==countof(surfaces));
	/* Any unsupported/missing trailing surface rejects the entire model. */
	surfaces[25].numframes=2;
	assert(!RCScene_AliasSupported(&entity));
	assert(!RCScene_AppendAliasLerped(&scene,&entity,73,material_for_skin,&no_failure));
	assert(scene.triangle_count==countof(surfaces)); surfaces[25].numframes=1;
	rc_alias_mesh_t *saved=surfaces[25].radiance_mesh; surfaces[25].radiance_mesh=NULL;
	assert(!RCScene_AliasSupported(&entity));
	assert(!RCScene_AppendAlias(&scene,&entity,matrix,0,0,0,0,73,material_for_skin,&no_failure));
	assert(scene.triangle_count==countof(surfaces)); surfaces[25].radiance_mesh=saved;
	/* A new retained allocation must invalidate cached geometry even when
	 * the entity, pose and material pointers are otherwise unchanged. */
	uint64_t revision=RCScene_AliasRevision(&entity);
	surfaces[25].radiance_mesh=static_md5_mesh(26,true); assert(surfaces[25].radiance_mesh);
	assert(revision!=RCScene_AliasRevision(&entity)); RCAlias_Destroy(saved);
	/* Threshold uses retained triangle counts across all surfaces, rather
	 * than trusting mutable draw-header counts or dropping small casters. */
	RCAlias_Destroy(surfaces[0].radiance_mesh);
	surfaces[0].radiance_mesh=static_md5_mesh_count(0,false,4070);assert(surfaces[0].radiance_mesh);
	assert(RCAlias_TriangleCount(surfaces[0].radiance_mesh)==4070 && !RCScene_AliasPersistent(&entity));
	RCAlias_Destroy(surfaces[0].radiance_mesh);
	surfaces[0].radiance_mesh=static_md5_mesh_count(0,false,4071);assert(surfaces[0].radiance_mesh);
	assert(RCScene_AliasPersistent(&entity));
	surfaces[25].numframes=2;assert(!RCScene_AliasPersistent(&entity));surfaces[25].numframes=1;
	surfaces[25].poseverttype=PV_QUAKE1;assert(!RCScene_AliasPersistent(&entity));surfaces[25].poseverttype=PV_MD5_8;
	RCScene_Clear(&scene); RCScene_DetachAliasModel(&model); RCScene_DetachAliasModel(&model);
	assert(RCAlias_TotalBytes()==0);
}

int main (void)
{
	struct vertex { uint16_t xyz[4]; unsigned char normal[4]; } xyz[2][3] = {0};
	xyz[0][1].xyz[0]=257; xyz[0][2].xyz[1]=257;
	for (int i=0;i<3;++i) { xyz[1][i]=xyz[0][i]; xyz[1][i].xyz[2]=514; }
	float uv[3][2]={{0,0},{1,0},{0,1}}, unit[3]={1,1,1}, zero[3]={0,0,0};
	uint16_t indices[3]={0,1,2};
	aliashdr_t first = {0}, second = {0};
	first.radiance_mesh=RCAlias_Create (RC_ALIAS_MDL,3,2,3,xyz,sizeof (xyz[0][0]),uv,sizeof (uv[0]),indices,unit,zero);
	second.radiance_mesh=RCAlias_Create (RC_ALIAS_MDL,3,2,3,xyz,sizeof (xyz[0][0]),uv,sizeof (uv[0]),indices,unit,zero);
	assert (first.radiance_mesh && second.radiance_mesh);
	first.poseverttype=second.poseverttype=PV_QUAKE1; first.numskins=second.numskins=1; first.nextsurface=&second;
	qmodel_t model = {0}; model.type=mod_alias; model.flags=MF_HOLEY; model.extradata[PV_QUAKE1]=(byte *)&first;
	entity_t entity = {0}; entity.model=&model;entity.netstate.scale=ENTSCALE_DEFAULT;
	uint8_t source[4]={1,2,3,255};
	gltexture_t texture={0}; texture.source_width=texture.source_height=1; texture.source_format=SRC_RGBA;
	texture.source_offset=(src_offset_t)(uintptr_t)source;
	first.gltextures[0][0]=second.gltextures[0][0]=&texture;
	uint64_t unchanged_revision = RCScene_AliasRevision (&entity);
	assert (unchanged_revision == RCScene_AliasRevision (&entity));
	cl.time=.1; assert (unchanged_revision != RCScene_AliasRevision (&entity)); cl.time=0;
	gl_fullbrights.value=1; assert (unchanged_revision != RCScene_AliasRevision (&entity)); gl_fullbrights.value=0;
	byte translated_color=1; entity.colormap=&translated_color;
	uint64_t translated_revision = RCScene_AliasRevision (&entity);
	gl_nocolors.value=1; assert (translated_revision != RCScene_AliasRevision (&entity));
	gl_nocolors.value=0; entity.colormap=NULL;
	assert (RCScene_AliasSupported (&entity));
	texture.source_format=SRC_LIGHTMAP; assert (!RCScene_AliasSupported (&entity));
	texture.source_format=SRC_RGBA;
	texture.source_offset=0; assert (!RCScene_AliasSupported (&entity));
	texture.source_offset=(src_offset_t)(uintptr_t)source;
	gltexture_t broken_emission={0}; second.fbtextures[0][0]=&broken_emission;
	gl_fullbrights.value=1; assert (!RCScene_AliasSupported (&entity));
	gl_fullbrights.value=0; assert (RCScene_AliasSupported (&entity));
	second.fbtextures[0][0]=NULL;
	first.poseverttype=PV_MD5_8; assert (!RCScene_AliasSupported (&entity));
	first.poseverttype=PV_QUAKE1;
	rc_alias_mesh_t *saved_mesh=first.radiance_mesh;
	first.radiance_mesh=NULL; assert (!RCScene_AliasSupported (&entity));
	first.radiance_mesh=saved_mesh; saved_mesh=second.radiance_mesh; second.radiance_mesh=NULL;
	assert (!RCScene_AliasSupported (&entity));
	second.radiance_mesh=saved_mesh;
	float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1};
	rc_scene_t scene; RCScene_Init (&scene); uint32_t no_failure=UINT32_MAX;
	assert (RCScene_AppendAlias (&scene,&entity,matrix,0,0,1,0,42,material_for_skin,&no_failure));
	assert (scene.triangle_count==2 && scene.triangles[0].metadata[0]==100 && scene.triangles[1].metadata[0]==101);
	assert ((scene.triangles[0].metadata[1] & RC_SCENE_CUTOUT) && scene.triangles[1].metadata[3]==42);
	assert (RCScene_AppendAliasLerped (&scene,&entity,43,material_for_skin,&no_failure));
	assert (scene.triangle_count==4 && scene.triangles[2].position[0][0]==30 && scene.triangles[2].position[0][1]==40 &&
		scene.triangles[2].position[0][2]==51 && scene.triangles[2].metadata[3]==43);
	assert (RCScene_Build (&scene));
	uint32_t fail_on_second=1;
	assert (!RCScene_AppendAlias (&scene,&entity,matrix,0,0,1,0,42,material_for_skin,&fail_on_second));
	assert (scene.triangle_count==4);
	second.poseverttype=PV_MD5;
	assert (!RCScene_AliasSupported (&entity));
	assert (!RCScene_AppendAlias (&scene,&entity,matrix,0,0,1,0,42,material_for_skin,&no_failure));
	assert (scene.triangle_count==4);
	RCScene_Clear (&scene);
	/* A full stress batch shares one local-space mesh and keeps every
	 * translation; malformed entries never publish a partial snapshot. */
	first.nextsurface=NULL; first.numposes=1; first.numframes=1; model.flags=0;
	const uint32_t batch_count=32768;
	entity_t *entities=calloc(batch_count,sizeof(*entities)); assert(entities);
	for(uint32_t i=0;i<batch_count;++i) { entities[i].model=&model; entities[i].netstate.scale=ENTSCALE_DEFAULT;
		entities[i].origin[0]=(float)i; entities[i].origin[1]=-(float)i; entities[i].origin[2]=4; }
	rc_scene_ecs_batch_t batch; RCScene_ECSBatchInit(&batch); rc_scene_rt_batch_t snapshot;
	assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot));
	assert(snapshot.count==batch_count && snapshot.triangle_count==1 && snapshot.mesh_revision);
	assert(snapshot.origins[32767][0]==32767 && snapshot.origins[32767][1]==-32767);
	assert(snapshot.triangles[0].position[0][0]==0 && snapshot.triangles[0].position[0][2]==0);
	uint64_t revision=snapshot.mesh_revision;
	entities[4].origin[2]=7; assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot));
	assert(snapshot.mesh_revision==revision && snapshot.origins[4][2]==7);
	/* Each ineligible instance leaves alone; the rest of the batch stays. */
	entities[4].angles[0]=1; assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot));
	assert(snapshot.count==batch_count-1 && snapshot.origins[4][0]==5 && snapshot.mesh_revision==revision); entities[4].angles[0]=0;
	entities[5].origin[0]=NAN; assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot) && snapshot.count==batch_count-1);
	for(uint32_t i=0;i<snapshot.count;++i) assert(isfinite(snapshot.origins[i][0]));
	entities[5].origin[0]=5; entities[6].netstate.scale=32;
	assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot) && snapshot.count==batch_count-1);
	entities[6].netstate.scale=ENTSCALE_DEFAULT; entities[7].alpha=ENTALPHA_ENCODE(.5f);
	assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot) && snapshot.count==batch_count-1);
	entities[7].alpha=ENTALPHA_DEFAULT;
	/* A translucent first instance must not supply the shared mesh or block the rest. */
	entities[0].alpha=ENTALPHA_ENCODE(.5f);
	assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot) && snapshot.count==batch_count-1);
	entities[0].alpha=ENTALPHA_DEFAULT;
	/* No eligible instance leaves nothing to batch. */
	for(uint32_t i=0;i<batch_count;++i) entities[i].angles[0]=1;
	assert(!RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot) && !snapshot.count && !snapshot.triangles);
	for(uint32_t i=0;i<batch_count;++i) entities[i].angles[0]=0;
	rc_alias_mesh_t *old_mesh=first.radiance_mesh; uint64_t old_identity=RCAlias_Revision(old_mesh);
	first.radiance_mesh=RCAlias_Create (RC_ALIAS_MDL,3,2,3,xyz,sizeof (xyz[0][0]),uv,sizeof (uv[0]),indices,unit,zero);
	assert(first.radiance_mesh && RCAlias_Revision(first.radiance_mesh)!=old_identity);
	assert(RCScene_ECSBatchPrepare(&batch,entities,batch_count,&snapshot) && snapshot.mesh_revision!=revision);
	RCAlias_Destroy(old_mesh);
	RCScene_ECSBatchClear(&batch); free(entities); first.nextsurface=&second;
	uint64_t second_bytes = RCAlias_Bytes (second.radiance_mesh);
	RCScene_DetachAliasHeader (&first);
	assert (!first.radiance_mesh && RCAlias_TotalBytes()==second_bytes);
	RCScene_DetachAliasHeader (&first); /* GPU deletion followed by model teardown */
	RCScene_DetachAliasModel (&model);
	RCScene_DetachAliasModel (&model); /* repeated reload/reset is safe */
	assert (!first.radiance_mesh && !second.radiance_mesh);
	assert (RCAlias_TotalBytes()==0);
	test_static_md5_surfaces(&texture);
	puts ("PASS: alias materials, cutout flags, static MD5 surfaces, animated fallback and repeated cleanup");
	return 0;
}
