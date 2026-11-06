#include "quakedef.h"
#include "radiance_scene_alias.h"
#include "radiance_alias.h"

extern cvar_t gl_fullbrights;
extern gltexture_t *playertextures[MAX_SCOREBOARD];

static bool RCScene_AliasHeaderSupported (const aliashdr_t *hdr)
{
	if (!hdr || !hdr->radiance_mesh) return false;
	if (hdr->poseverttype == PV_QUAKE1 || hdr->poseverttype == PV_QUAKE3) return true;
	/* Skeletal aliases retain their exact skinned frame only when immutable.
	 * Animated MD5 models need pose-aware extraction before they can cast. */
	return (hdr->poseverttype == PV_MD5 || hdr->poseverttype == PV_MD5_8) &&
		hdr->numframes == 1 && hdr->numposes == 1 && hdr->numjoints > 0;
}

static uint64_t RCScene_AliasHash (uint64_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	while (size--) { hash ^= *bytes++; hash *= UINT64_C (1099511628211); }
	return hash;
}

uint64_t RCScene_AliasRevision (const entity_t *entity)
{
	uint64_t hash = UINT64_C (1469598103934665603);
	if (!entity || !entity->model || entity->model->type != mod_alias) return 0;
	aliashdr_t *model = entity->compiled_instance ? Mod_Extradata_Exact (entity->model, entity->skinnum) : Mod_Extradata_CheckSkin (entity->model, entity->skinnum);
	if (!model) return 0;
	lerpdata_t pose = {0};
	R_SetupAliasFrame (entity, model, &pose);
	R_GetEntityLerpedTransform (entity, pose.origin, pose.angles);
	/* Toggling fullbrights changes the material atlas and supported emission
	 * sources. Schedule each cached alias to refresh along with that change. */
	const bool fullbright_enabled = gl_fullbrights.value != 0;
	hash = RCScene_AliasHash (hash, &fullbright_enabled, sizeof (fullbright_enabled));
	hash = RCScene_AliasHash (hash, &pose.pose1, sizeof (pose.pose1));
	hash = RCScene_AliasHash (hash, &pose.pose2, sizeof (pose.pose2));
	hash = RCScene_AliasHash (hash, &pose.blend, sizeof (pose.blend));
	hash = RCScene_AliasHash (hash, pose.origin, sizeof (pose.origin));
	hash = RCScene_AliasHash (hash, pose.angles, sizeof (pose.angles));
	const unsigned animation = entity->compiled_instance ? 0 : (unsigned)((int)(cl.time * 10) & (MAX_FRAMEGROUPS - 1));
	for (const aliashdr_t *surface = model; surface; surface = surface->nextsurface)
	{
		const uint64_t retained_revision = RCAlias_Revision (surface->radiance_mesh);
		hash = RCScene_AliasHash (hash, &retained_revision, sizeof (retained_revision));
		int skin = entity->skinnum;
		if (skin < 0 || skin >= surface->numskins) skin = 0;
		if (surface->numskins < 1) continue;
		const gltexture_t *base = surface->gltextures[skin][animation];
		const gltexture_t *emission = surface->fbtextures[skin][animation];
		hash = RCScene_AliasHash (hash, &base, sizeof (base));
		hash = RCScene_AliasHash (hash, &emission, sizeof (emission));
	}
	/* Match the player color substitution performed by the material callback. */
	const bool translated = entity->colormap != vid.colormap && !gl_nocolors.value;
	hash = RCScene_AliasHash (hash, &translated, sizeof (translated));
	if (translated && cl.entities && cl.maxclients > 0)
	{
		uintptr_t address = (uintptr_t)entity, first = (uintptr_t)&cl.entities[1], last = (uintptr_t)&cl.entities[cl.maxclients];
		if (address >= first && address <= last && (address - first) % sizeof (*entity) == 0)
		{
			size_t player = (address - first) / sizeof (*entity);
			if (player < MAX_SCOREBOARD) hash = RCScene_AliasHash (hash, &playertextures[player], sizeof (playertextures[player]));
		}
	}
	return hash;
}

static bool RCScene_AliasSourceAvailable (const gltexture_t *texture)
{
	return texture && texture->source_width && texture->source_height &&
		(texture->source_file[0] || texture->source_offset) &&
		(texture->source_format == SRC_INDEXED || texture->source_format == SRC_INDEXED_PALETTE || texture->source_format == SRC_RGBA);
}

bool RCScene_AliasSupported (const entity_t *entity)
{
	if (!entity || !entity->model || entity->model->type != mod_alias) return false;
	aliashdr_t *hdr = entity->compiled_instance ? Mod_Extradata_Exact (entity->model, entity->skinnum) : Mod_Extradata_CheckSkin (entity->model, entity->skinnum);
	if (!hdr) return false;
	unsigned animation = entity->compiled_instance ? 0 : (unsigned)((int)(cl.time * 10) & 3);
	for (; hdr; hdr = hdr->nextsurface)
	{
		if (!RCScene_AliasHeaderSupported (hdr) || hdr->numskins < 1) return false;
		int skin = entity->skinnum;
		if (skin < 0 || skin >= hdr->numskins) skin = 0;
		const gltexture_t *base = hdr->gltextures[skin][animation];
		const gltexture_t *emission = hdr->fbtextures[skin][animation];
		if (!RCScene_AliasSourceAvailable (base) || (gl_fullbrights.value && emission && !RCScene_AliasSourceAvailable (emission))) return false;
	}
	return true;
}

bool RCScene_AliasPersistent (const entity_t *entity)
{
	if (!RCScene_AliasSupported (entity)) return false;
	const bool compiled = entity->compiled_instance != NULL;
	const aliashdr_t *hdr = entity->compiled_instance ? Mod_Extradata_Exact (entity->model, entity->skinnum) : Mod_Extradata_CheckSkin (entity->model, entity->skinnum);
	uint64_t triangles = 0;
	for (; hdr; hdr = hdr->nextsurface)
	{
		if (hdr->numframes != 1 || hdr->numposes != 1) return false;
		if (!compiled && ((hdr->poseverttype != PV_MD5 && hdr->poseverttype != PV_MD5_8) || hdr->numjoints < 1)) return false;
		triangles += RCAlias_TriangleCount (hdr->radiance_mesh);
	}
	/* Keep small moving replacements/projectiles in the mutable partition.
	 * Transform changes still invalidate a persistent model's cached base. */
	return compiled || triangles >= 4096;
}

bool RCScene_AppendCompiledWorld (rc_scene_t *scene, const qmodel_t *world,
	rc_scene_alias_material_fn material, void *user)
{
	if (!scene || !world || !material) return false;
	const qsm_data_t *data = world->static_models;
	if (!data) return true;
	const uint32_t original = scene->triangle_count;
	for (uint32_t i=0;i<data->instance_count;++i)
	{
		const qsm_instance_t *instance = &data->instances[i];
		if (!isfinite(instance->modelscale) || instance->modelscale<=0 || instance->modelscale>1024) goto failed;
		qmodel_t *model = Mod_ForName(instance->model,false);
		aliashdr_t *header = model && model->type==mod_alias ? Mod_Extradata_Exact(model,(int)instance->skin) : NULL;
		if (!header) goto failed;
		for (const aliashdr_t *surface=header;surface;surface=surface->nextsurface)
		{
			if (!RCScene_AliasHeaderSupported(surface) || surface->numframes!=1 || surface->numposes!=1 || surface->numskins<1 ||
				instance->skin>=(uint32_t)surface->numskins ||
				!RCScene_AliasSourceAvailable(surface->gltextures[instance->skin][0])) goto failed;
			const gltexture_t *fullbright=surface->fbtextures[instance->skin][0];
			if (gl_fullbrights.value && fullbright && !RCScene_AliasSourceAvailable(fullbright)) goto failed;
		}
		entity_t entity={0};entity.model=model;entity.skinnum=(int)instance->skin;
		entity.colormap=vid.colormap;entity.compiled_instance=instance;entity.modelscale_override=instance->modelscale;
		entity.netstate.scale=ENTSCALE_DEFAULT;
		float matrix[16];IdentityMatrix(matrix);
		vec3_t origin,angles;VectorCopy(instance->origin,origin);VectorCopy(instance->angles,angles);
		R_RotateForEntityScale(matrix,origin,angles,instance->modelscale);
		uint32_t start=scene->triangle_count;
		if (!RCScene_AppendAlias(scene,&entity,matrix,0,0,0,0,i+1,material,user)) goto failed;
		if (scene->triangle_count-start != instance->triangle_count) goto failed;
		for(uint32_t t=start;t<scene->triangle_count;++t)scene->triangles[t].metadata[1]&=~RC_SCENE_DYNAMIC;
	}
	return true;
failed:
	scene->triangle_count=original;
	return false;
}

void RCScene_DetachAliasHeader (aliashdr_t *hdr)
{
	if (!hdr) return;
	RCAlias_Destroy (hdr->radiance_mesh);
	hdr->radiance_mesh = NULL;
}

void RCScene_DetachAliasModel (qmodel_t *model)
{
	if (!model || model->type != mod_alias) return;
	for (int type = 0; type < PV_SIZE; ++type)
		for (aliashdr_t *hdr = (aliashdr_t *)model->extradata[type]; hdr; hdr = hdr->nextsurface)
			RCScene_DetachAliasHeader (hdr);
}

bool RCScene_AppendAlias (rc_scene_t *scene, const entity_t *entity, const float matrix[16],
	uint32_t pose1, uint32_t pose2, float blend, unsigned animation, uint32_t instance,
	rc_scene_alias_material_fn material, void *user)
{
	if (!scene || !entity || !entity->model || entity->model->type != mod_alias || !matrix || !material) return false;
	aliashdr_t *mainhdr = entity->compiled_instance ? Mod_Extradata_Exact (entity->model, entity->skinnum) : Mod_Extradata_CheckSkin (entity->model, entity->skinnum);
	if (!mainhdr) return false;
	/* Preflight every surface so unsupported enhanced models are rejected as a
	 * whole rather than leaving a partial occluder in the scene. */
	for (aliashdr_t *hdr = mainhdr; hdr; hdr = hdr->nextsurface)
		if (!RCScene_AliasHeaderSupported (hdr)) return false;
	uint32_t original = scene->triangle_count, surface = 0;
	for (aliashdr_t *hdr = mainhdr; hdr; hdr = hdr->nextsurface, ++surface)
	{
		int skin = entity->skinnum;
		if (skin < 0 || skin >= hdr->numskins) skin = 0;
		if (hdr->numskins < 1) goto failed;
		const gltexture_t *texture = hdr->gltextures[skin][animation % MAX_FRAMEGROUPS];
		const gltexture_t *fullbright = hdr->fbtextures[skin][animation % MAX_FRAMEGROUPS];
		uint32_t material_id = material (entity, surface, texture, fullbright, user);
		if (material_id == UINT32_MAX) goto failed;
		uint32_t flags = RC_SCENE_DYNAMIC;
		if ((entity->model->flags & MF_HOLEY) || (texture && (texture->flags & TEXPREF_ALPHAPIXELS))) flags |= RC_SCENE_CUTOUT;
		if (fullbright) flags |= RC_SCENE_EMISSIVE;
		if (!RCAlias_Append (scene, hdr->radiance_mesh, pose1, pose2, blend, matrix, material_id, flags, surface, instance)) goto failed;
	}
	return true;
failed:
	scene->triangle_count = original;
	return false;
}

bool RCScene_AppendAliasLerped (rc_scene_t *scene, const entity_t *entity, uint32_t instance,
	rc_scene_alias_material_fn material, void *user)
{
	if (!entity || !entity->model || entity->model->type != mod_alias) return false;
	aliashdr_t *hdr = entity->compiled_instance ? Mod_Extradata_Exact (entity->model, entity->skinnum) : Mod_Extradata_CheckSkin (entity->model, entity->skinnum);
	if (!RCScene_AliasHeaderSupported (hdr)) return false;
	lerpdata_t lerp = {0};
	R_SetupAliasFrame (entity, hdr, &lerp);
	R_GetEntityLerpedTransform (entity, lerp.origin, lerp.angles);
	if (lerp.pose1 < 0 || lerp.pose2 < 0) return false;
	float matrix[16]; IdentityMatrix (matrix);
	R_RotateForEntityScale (matrix, lerp.origin, lerp.angles, R_EntityModelScale (entity));
	return RCScene_AppendAlias (scene, entity, matrix, (uint32_t)lerp.pose1, (uint32_t)lerp.pose2,
		lerp.blend, (unsigned)((int)(cl.time * 10) & 3), instance, material, user);
}

/* This snapshot is restricted to the renderer's local static-model ECS fast
 * path. Preserve the ordinary bounded scene for material and software rays. */
static uint32_t RCScene_ECSBatchMaterial (const entity_t *entity, uint32_t surface,
    const gltexture_t *texture, const gltexture_t *fullbright, void *user)
{
    (void)entity; (void)surface; (void)texture; (void)fullbright; (void)user;
    return 0; /* shadow-only geometry has no material lookup */
}

void RCScene_ECSBatchInit (rc_scene_ecs_batch_t *batch)
{
    if (batch) memset (batch, 0, sizeof (*batch));
}

void RCScene_ECSBatchClear (rc_scene_ecs_batch_t *batch)
{
    if (!batch) return;
    RCScene_Clear (&batch->mesh);
    free (batch->origins);
    memset (batch, 0, sizeof (*batch));
}

bool RCScene_ECSBatchPrepare (rc_scene_ecs_batch_t *batch, const entity_t *entities,
    uint32_t count, rc_scene_rt_batch_t *snapshot)
{
    if (snapshot) memset (snapshot, 0, sizeof (*snapshot));
    if (!batch || !snapshot || !entities || !count) return false;
    const qmodel_t *model = entities[0].model;
    if (!model || model->needload || model->type != mod_alias || (model->flags & MF_HOLEY)) return false;
    aliashdr_t *hdr = Mod_Extradata_CheckSkin ((qmodel_t *)model, 0);
    if (!hdr || hdr->poseverttype != PV_QUAKE1 || hdr->numposes != 1 || hdr->numframes != 1 ||
        hdr->nextsurface || hdr->numskins < 1 || !hdr->radiance_mesh) return false;
    for (unsigned i = 0; i < MAX_FRAMEGROUPS; ++i)
        if (hdr->gltextures[0][i] && (hdr->gltextures[0][i]->flags & TEXPREF_ALPHAPIXELS)) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const entity_t *e = &entities[i];
        if (e->model != model || e->skinnum != 0 || e->frame != 0 ||
            e->netstate.scale != ENTSCALE_DEFAULT ||
            (e->alpha != ENTALPHA_DEFAULT && ENTALPHA_DECODE (e->alpha) < 1.0f)) return false;
        for (int axis = 0; axis < 3; ++axis)
            if (!isfinite (e->origin[axis]) || e->angles[axis] != 0.0f) return false;
    }
    if (batch->model != model || batch->header != hdr || batch->mesh_identity != hdr->radiance_mesh || batch->retained_revision != RCAlias_Revision (hdr->radiance_mesh) || !batch->mesh.triangle_count)
    {
        rc_scene_t mesh; RCScene_Init (&mesh);
        float matrix[16]; IdentityMatrix (matrix);
        if (!RCScene_AppendAlias (&mesh, &entities[0], matrix, 0, 0, 0, 0, 0, RCScene_ECSBatchMaterial, NULL) ||
            !mesh.triangle_count) { RCScene_Clear (&mesh); return false; }
        RCScene_Clear (&batch->mesh); batch->mesh = mesh;
        batch->model = model; batch->header = hdr; batch->mesh_identity = hdr->radiance_mesh;
        batch->retained_revision = RCAlias_Revision (hdr->radiance_mesh);
        if (++batch->mesh_revision == 0) ++batch->mesh_revision;
    }
    if (count > batch->capacity)
    {
        if ((uint64_t)count * sizeof (*batch->origins) > SIZE_MAX) return false;
        void *storage = realloc (batch->origins, (size_t)count * sizeof (*batch->origins));
        if (!storage) return false;
        batch->origins = storage; batch->capacity = count;
    }
    for (uint32_t i = 0; i < count; ++i) memcpy (batch->origins[i], entities[i].origin, sizeof (batch->origins[i]));
    *snapshot = (rc_scene_rt_batch_t){batch->mesh.triangles, batch->mesh.triangle_count,
        (const float (*)[3])batch->origins, count, batch->mesh_revision};
    return true;
}
