#include "quakedef.h"
#include "radiance_scene_bsp.h"

static void RCScene_BrushVertex (const float *v, const float *matrix, float out[4])
{
	if (matrix)
	{
		for (int a = 0; a < 3; ++a)
			out[a] = matrix[a] * v[0] + matrix[4 + a] * v[1] + matrix[8 + a] * v[2] + matrix[12 + a];
	}
	else for (int a = 0; a < 3; ++a) out[a] = v[a];
	out[3] = 1.0f;
}

bool RCScene_AppendBrush (rc_scene_t *scene, const qmodel_t *model, const float matrix[16],
	uint32_t instance, bool dynamic, rc_scene_material_fn material, void *user)
{
	if (!scene || !model || model->type != mod_brush || model->firstmodelsurface < 0 ||
		model->nummodelsurfaces < 0 || model->firstmodelsurface > model->numsurfaces ||
		model->nummodelsurfaces > model->numsurfaces - model->firstmodelsurface) return false;
	uint32_t original = scene->triangle_count;
	for (int i = 0; i < model->nummodelsurfaces; ++i)
	{
		const int surface_index = model->firstmodelsurface + i;
		const msurface_t *surface = &model->surfaces[surface_index];
		if (!surface->texinfo || !surface->polys) goto failed;
		const texture_t *texture = surface->texinfo->texture;
		uint32_t material_id = material ? material (texture, user) : (uint32_t)surface->texinfo->tex_idx;
		if (material_id == UINT32_MAX) goto failed;
		uint32_t flags = dynamic ? RC_SCENE_DYNAMIC : 0;
		if ((surface->flags & SURF_DRAWFENCE) || (texture && texture->type == TEXTYPE_CUTOUT)) flags |= RC_SCENE_CUTOUT;
		if (surface->flags & SURF_DRAWSKY) flags |= RC_SCENE_SKY;
		if ((surface->flags & SURF_DRAWTURB) || (texture && TEXTYPE_ISLIQUID (texture->type))) flags |= RC_SCENE_LIQUID;
		if (texture && texture->fullbright) flags |= RC_SCENE_EMISSIVE;
		for (const glpoly_t *poly = surface->polys; poly; poly = poly->next)
		{
			if (poly->numverts < 3) continue;
			const float *verts = &poly->verts[0][0];
			for (int j = 1; j < poly->numverts - 1; ++j)
			{
				const float *a = verts, *b = verts + (size_t)j * VERTEXSIZE, *c = verts + (size_t)(j + 1) * VERTEXSIZE;
				uint32_t triangle_flags = flags;
				if (surface->plane)
				{
					/* BSP surfedges commonly wind opposite the visible face. Orient
					 * real brush faces using their plane, not this incidental winding. */
					vec3_t ab, ac, cross;
					VectorSubtract (b, a, ab); VectorSubtract (c, a, ac);
					cross[0] = ab[1] * ac[2] - ab[2] * ac[1];
					cross[1] = ab[2] * ac[0] - ab[0] * ac[2];
					cross[2] = ab[0] * ac[1] - ab[1] * ac[0];
					float facing = DotProduct (cross, surface->plane->normal);
					if (surface->flags & SURF_PLANEBACK) facing = -facing;
					if (isfinite (facing) && fabsf (facing) > 1e-9f)
					{
						/* Reflections reverse transformed winding. */
						if (matrix)
						{
							float det = matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6]) -
								matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2]) +
								matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
							if (!isfinite (det) || det == 0.0f) facing = 0.0f;
							else if (det < 0.0f) facing = -facing;
						}
						if (facing != 0.0f)
						{
							if (facing < 0.0f) { const float *tmp = b; b = c; c = tmp; }
							triangle_flags |= RC_SCENE_ORIENTED;
						}
					}
				}
				rc_scene_triangle_t tri = {0};
				RCScene_BrushVertex (a, matrix, tri.position[0]);
				RCScene_BrushVertex (b, matrix, tri.position[1]);
				RCScene_BrushVertex (c, matrix, tri.position[2]);
				tri.uv01[0] = a[3]; tri.uv01[1] = a[4]; tri.uv01[2] = b[3]; tri.uv01[3] = b[4];
				tri.uv2[0] = c[3]; tri.uv2[1] = c[4];
				tri.metadata[0] = material_id; tri.metadata[1] = triangle_flags;
				tri.metadata[2] = (uint32_t)surface_index; tri.metadata[3] = instance;
				if (!RCScene_AppendTriangle (scene, &tri)) goto failed;
			}
		}
	}
	return true;
failed:
	/* The scene cannot be used until rebuilt after this rollback. */
	scene->triangle_count = original;
	return false;
}
