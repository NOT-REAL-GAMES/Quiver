/*
Copyright (C) 2024 Quiver Contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// kd_render.c -- Kd-tree map rendering

#include "quakedef.h"
#include "kd_model.h"

static kdmodel_t *kd_debug_model = NULL;
static cvar_t kd_draw = {"kd_draw", "1", CVAR_NONE};

/*
=================
KD_CalcTexCoord

Calculate texture coordinate for a vertex using texinfo_vecs
=================
*/
static void KD_CalcTexCoord (float *pos, float texinfo_vecs[2][4], gltexture_t *gltex, float *s, float *t)
{
	// Standard Quake texcoord calculation: s = pos . vec[0] + offset[0]
	*s = pos[0] * texinfo_vecs[0][0] + pos[1] * texinfo_vecs[0][1] + pos[2] * texinfo_vecs[0][2] + texinfo_vecs[0][3];
	*t = pos[0] * texinfo_vecs[1][0] + pos[1] * texinfo_vecs[1][1] + pos[2] * texinfo_vecs[1][2] + texinfo_vecs[1][3];

	// Normalize by texture size
	if (gltex)
	{
		*s /= gltex->width;
		*t /= gltex->height;
	}
	else
	{
		*s /= 64.0f;  // Default texture size
		*t /= 64.0f;
	}
}

/*
=================
KD_DrawDebugSurfaces

Draw kd-tree surfaces using basic_blend pipeline with textures
=================
*/
void KD_DrawDebugSurfaces (cb_context_t *cbx)
{
	if (!kd_debug_model || !kd_draw.value)
		return;

	kdmodel_t *model = kd_debug_model;

	// Calculate total vertices needed (triangulated quads = 6 verts per 4-vert surface)
	// Double for both front and back faces (no backface culling via geometry)
	int total_verts = 0;
	for (int i = 0; i < model->num_surfaces; i++)
	{
		mkdsurface_t *surf = &model->surfaces[i];
		if (surf->num_vertices >= 3)
			total_verts += (surf->num_vertices - 2) * 3 * 2; // Triangle fan * 2 for both sides
	}

	if (total_verts == 0)
		return;

	// Allocate vertex buffer from dynamic buffer
	VkBuffer buffer;
	VkDeviceSize buffer_offset;
	basicvertex_t *vertices = (basicvertex_t *)R_VertexAllocate (
		total_verts * sizeof(basicvertex_t), &buffer, &buffer_offset);

	if (!vertices)
		return;

	// Bind pipeline
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.basic_blend_pipeline[cbx->render_pass_index]);

	// Use greytexture as default - bind it once
	vkCmdBindDescriptorSets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.basic_pipeline_layout.handle, 0, 1, &greytexture->descriptor_set, 0, NULL);

	// Fill vertex buffer
	int vert_idx = 0;
	for (int i = 0; i < model->num_surfaces; i++)
	{
		mkdsurface_t *surf = &model->surfaces[i];
		if (surf->num_vertices < 3)
			continue;

		// Get texture for this surface - use greytexture if not available
		gltexture_t *gltex = surf->gltexture;
		if (!gltex)
			gltex = greytexture;

		// White color - let texture provide color
		byte color[4] = {255, 255, 255, 255};

		// Triangle fan - emit both windings for double-sided rendering
		for (int j = 2; j < surf->num_vertices; j++)
		{
			int v0 = surf->first_vertex;
			int v1 = surf->first_vertex + j - 1;
			int v2 = surf->first_vertex + j;

			if (v0 < model->num_vertices && v1 < model->num_vertices && v2 < model->num_vertices)
			{
				float s, t;

				// Front face (CCW)
				vertices[vert_idx].position[0] = model->vertices[v0][0];
				vertices[vert_idx].position[1] = model->vertices[v0][1];
				vertices[vert_idx].position[2] = model->vertices[v0][2];
				KD_CalcTexCoord(model->vertices[v0], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				memcpy(vertices[vert_idx].color, color, 4);
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v1][0];
				vertices[vert_idx].position[1] = model->vertices[v1][1];
				vertices[vert_idx].position[2] = model->vertices[v1][2];
				KD_CalcTexCoord(model->vertices[v1], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				memcpy(vertices[vert_idx].color, color, 4);
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v2][0];
				vertices[vert_idx].position[1] = model->vertices[v2][1];
				vertices[vert_idx].position[2] = model->vertices[v2][2];
				KD_CalcTexCoord(model->vertices[v2], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				memcpy(vertices[vert_idx].color, color, 4);
				vert_idx++;

				// Back face (CW) - reversed winding
				vertices[vert_idx].position[0] = model->vertices[v0][0];
				vertices[vert_idx].position[1] = model->vertices[v0][1];
				vertices[vert_idx].position[2] = model->vertices[v0][2];
				KD_CalcTexCoord(model->vertices[v0], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				memcpy(vertices[vert_idx].color, color, 4);
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v2][0];
				vertices[vert_idx].position[1] = model->vertices[v2][1];
				vertices[vert_idx].position[2] = model->vertices[v2][2];
				KD_CalcTexCoord(model->vertices[v2], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				memcpy(vertices[vert_idx].color, color, 4);
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v1][0];
				vertices[vert_idx].position[1] = model->vertices[v1][1];
				vertices[vert_idx].position[2] = model->vertices[v1][2];
				KD_CalcTexCoord(model->vertices[v1], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				memcpy(vertices[vert_idx].color, color, 4);
				vert_idx++;
			}
		}
	}

	if (vert_idx == 0)
		return;

	// Bind vertex buffer and draw
	vkCmdBindVertexBuffers (cbx->cb, 0, 1, &buffer, &buffer_offset);
	vkCmdDraw (cbx->cb, vert_idx, 1, 0, 0);
}

/*
=================
KD_LoadDebug_f
=================
*/
static void KD_LoadDebug_f (void)
{
	if (Cmd_Argc () < 2)
	{
		Con_Printf ("Usage: kd_load <filename.qkd>\n");
		if (kd_debug_model)
			Con_Printf ("Current: %s\n", kd_debug_model->name);
		return;
	}

	// Free existing
	if (kd_debug_model)
	{
		KD_FreeModel (kd_debug_model);
		kd_debug_model = NULL;
	}

	const char *filename = Cmd_Argv (1);
	kd_debug_model = KD_LoadModel (filename);

	if (kd_debug_model)
	{
		Con_Printf ("Loaded %s for debug rendering\n", filename);
		Con_Printf ("  Surfaces: %d, Vertices: %d\n",
		            kd_debug_model->num_surfaces, kd_debug_model->num_vertices);

		// Print first few vertices for debugging
		for (int i = 0; i < kd_debug_model->num_surfaces && i < 6; i++)
		{
			mkdsurface_t *s = &kd_debug_model->surfaces[i];
			Con_Printf ("  Surf %d: first_vert=%d, num_verts=%d\n",
			            i, s->first_vertex, s->num_vertices);
		}
		for (int i = 0; i < kd_debug_model->num_vertices && i < 8; i++)
		{
			Con_Printf ("  Vert %d: (%.1f, %.1f, %.1f)\n",
			            i, kd_debug_model->vertices[i][0],
			            kd_debug_model->vertices[i][1],
			            kd_debug_model->vertices[i][2]);
		}
	}
	else
		Con_Printf ("Failed to load %s\n", filename);
}

/*
=================
KD_RenderInit
=================
*/
void KD_RenderInit (void)
{
	Cvar_RegisterVariable (&kd_draw);
	Cmd_AddCommand ("kd_load", KD_LoadDebug_f);
}

/*
=================
KD_GetDebugModel
=================
*/
kdmodel_t *KD_GetDebugModel (void)
{
	return kd_debug_model;
}
