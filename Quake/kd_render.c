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
static cvar_t kd_noclip_bsp = {"kd_noclip_bsp", "0", CVAR_NONE};  // Skip BSP collision when kd is active
static cvar_t kd_norender_bsp = {"kd_norender_bsp", "0", CVAR_NONE};  // Skip BSP rendering when kd is active
static gltexture_t *kd_default_texture = NULL;
static wad_t *kd_wads = NULL;  // Loaded WAD files for kd textures

/*
=================
KD_FindWorldTexture

Find a texture from the world model by name (partial match)
=================
*/
static gltexture_t *KD_FindWorldTexture (const char *partial_name)
{
	if (!cl.worldmodel || !cl.worldmodel->textures)
		return NULL;

	for (int i = 0; i < cl.worldmodel->numtextures; i++)
	{
		texture_t *tex = cl.worldmodel->textures[i];
		if (tex && tex->gltexture && strstr(tex->name, partial_name))
			return tex->gltexture;
	}
	return NULL;
}

/*
=================
KD_LoadTextureFromWad

Load a texture directly from WAD files
=================
*/
static qboolean kd_wad_load_attempted = false;

static gltexture_t *KD_LoadTextureFromWad (const char *name)
{
	lumpinfo_t *info;
	wad_t      *wad = NULL;
	miptex_t    mt;
	byte       *data;
	int         pixels;
	gltexture_t *glt;
	char        texname[64];

	// Load WADs if not already loaded (only try once)
	if (!kd_wads && !kd_wad_load_attempted)
	{
		kd_wad_load_attempted = true;
		Con_Printf ("KD: Loading WAD files for textures...\n");
		// Try common Quake WAD files
		kd_wads = W_LoadWadList ("gfx/wiz.wad;gfx/medieval.wad;gfx/metal.wad;gfx/base.wad;gfx/rune.wad");
		if (!kd_wads)
		{
			Con_Printf ("KD: No WAD files found, textures will use fallback\n");
			return NULL;
		}
		Con_Printf ("KD: WAD files loaded successfully\n");
	}

	if (!kd_wads)
		return NULL;

	// Look for the texture in loaded WADs
	info = W_GetLumpinfoList (kd_wads, name, &wad);
	if (!info || !wad)
	{
		Con_DPrintf ("KD_LoadTextureFromWad: texture '%s' not found in WADs\n", name);
		return NULL;
	}

	if (info->type != TYP_MIPTEX)
	{
		Con_DPrintf ("KD_LoadTextureFromWad: '%s' is not a miptex (type %d)\n", name, info->type);
		return NULL;
	}

	// Read miptex header
	FS_fseek (&wad->fh, info->filepos, SEEK_SET);
	if (FS_fread (&mt, 1, sizeof(miptex_t), &wad->fh) != sizeof(miptex_t))
	{
		Con_Warning ("KD_LoadTextureFromWad: failed to read miptex header for '%s'\n", name);
		return NULL;
	}

	mt.width = LittleLong (mt.width);
	mt.height = LittleLong (mt.height);
	for (int i = 0; i < MIPLEVELS; i++)
		mt.offsets[i] = LittleLong (mt.offsets[i]);

	if (mt.width == 0 || mt.height == 0 || mt.width > 4096 || mt.height > 4096)
	{
		Con_Warning ("KD_LoadTextureFromWad: invalid texture size '%s' (%dx%d)\n", name, mt.width, mt.height);
		return NULL;
	}

	if (mt.offsets[0] == 0)
	{
		Con_Warning ("KD_LoadTextureFromWad: invalid mip offset for '%s'\n", name);
		return NULL;
	}

	// Read pixel data (just mip level 0)
	pixels = mt.width * mt.height;
	data = (byte *)Mem_Alloc (pixels);
	if (!data)
	{
		Con_Warning ("KD_LoadTextureFromWad: failed to allocate %d bytes for '%s'\n", pixels, name);
		return NULL;
	}

	FS_fseek (&wad->fh, info->filepos + mt.offsets[0], SEEK_SET);
	if (FS_fread (data, 1, pixels, &wad->fh) != (size_t)pixels)
	{
		Con_Warning ("KD_LoadTextureFromWad: failed to read pixel data for '%s'\n", name);
		Mem_Free (data);
		return NULL;
	}

	// Create GL texture
	q_snprintf (texname, sizeof(texname), "kd:%s", name);
	glt = TexMgr_LoadImage (NULL, texname, mt.width, mt.height, SRC_INDEXED, data,
	                        "", (src_offset_t)0, TEXPREF_MIPMAP | TEXPREF_PERSIST);

	Mem_Free (data);

	if (glt)
		Con_Printf ("KD: Loaded texture '%s' (%dx%d)\n", name, mt.width, mt.height);

	return glt;
}

// World vertex format: position (vec3), texcoord1 (vec2), texcoord2 (vec2) = 28 bytes
typedef struct
{
	float position[3];
	float texcoord[2];   // Diffuse texture coords
	float lmcoord[2];    // Lightmap coords
} worldvertex_t;

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

Draw kd-tree surfaces using the world pipeline for proper textured rendering
=================
*/
void KD_DrawDebugSurfaces (cb_context_t *cbx)
{
	if (!kd_debug_model || !kd_draw.value)
		return;

	kdmodel_t *model = kd_debug_model;

	// Calculate total vertices needed (triangulated polygons)
	// Double for both front and back faces (no backface culling via geometry)
	// Skip sky surfaces - they're rendered by the skybox system
	int total_verts = 0;
	for (int i = 0; i < model->num_surfaces; i++)
	{
		mkdsurface_t *surf = &model->surfaces[i];
		if (surf->num_vertices < 3)
			continue;
		// Skip sky textures
		if (surf->gltexture && !strncmp(surf->gltexture->name, "kd:sky", 6))
			continue;
		total_verts += (surf->num_vertices - 2) * 3 * 2; // Triangle fan * 2 for both sides
	}

	if (total_verts == 0)
		return;

	// Allocate vertex buffer from dynamic buffer (world vertex format = 28 bytes)
	VkBuffer buffer;
	VkDeviceSize buffer_offset;
	worldvertex_t *vertices = (worldvertex_t *)R_VertexAllocate (
		total_verts * sizeof(worldvertex_t), &buffer, &buffer_offset);

	if (!vertices)
		return;

	// Use world pipeline index 0 (no fullbright, no alpha test, no alpha blend, no palettize)
	int pipeline_index = 0;
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipelines[pipeline_index]);

	// Bind greylightmap for set 1 (lightmap) - we don't have real lightmaps yet
	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle,
		1, 1, &greylightmap->descriptor_set, 0, NULL);

	// Bind nulltexture for set 2 (fullbright)
	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle,
		2, 1, &nulltexture->descriptor_set, 0, NULL);

	// Try to find a world texture to use if we don't have a cached one
	if (!kd_default_texture)
	{
		// First try from worldmodel textures
		kd_default_texture = KD_FindWorldTexture("wiz1_1");
		if (!kd_default_texture)
			kd_default_texture = KD_FindWorldTexture("wiz");
		if (!kd_default_texture)
			kd_default_texture = KD_FindWorldTexture("brick");
		if (!kd_default_texture)
			kd_default_texture = KD_FindWorldTexture("wall");

		// If no worldmodel textures, try loading directly from WAD
		if (!kd_default_texture)
			kd_default_texture = KD_LoadTextureFromWad("wiz1_1");
		if (!kd_default_texture)
			kd_default_texture = KD_LoadTextureFromWad("metal1_1");
		if (!kd_default_texture)
			kd_default_texture = KD_LoadTextureFromWad("rock1_2");
	}

	// Group surfaces by texture for efficient rendering
	gltexture_t *current_texture = NULL;
	int vert_idx = 0;
	int batch_start = 0;

	for (int i = 0; i < model->num_surfaces; i++)
	{
		mkdsurface_t *surf = &model->surfaces[i];
		if (surf->num_vertices < 3)
			continue;

		// Skip sky surfaces - they're handled by the skybox renderer
		gltexture_t *gltex = surf->gltexture;
		if (gltex && !strncmp(gltex->name, "kd:sky", 6))
			continue;

		// Get texture for this surface (fallback if none)
		if (!gltex)
			gltex = kd_default_texture ? kd_default_texture : greytexture;

		// If texture changed, flush batch and bind new texture
		if (gltex != current_texture && vert_idx > batch_start)
		{
			// Bind vertex buffer and draw previous batch
			vkCmdBindVertexBuffers (cbx->cb, 0, 1, &buffer, &buffer_offset);
			vkCmdDraw (cbx->cb, vert_idx - batch_start, 1, batch_start, 0);
			batch_start = vert_idx;
		}

		if (gltex != current_texture)
		{
			current_texture = gltex;
			// Bind diffuse texture to set 0
			vulkan_globals.vk_cmd_bind_descriptor_sets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle,
				0, 1, &gltex->descriptor_set, 0, NULL);
		}

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
				vertices[vert_idx].lmcoord[0] = 0.5f;  // Center of lightmap (neutral)
				vertices[vert_idx].lmcoord[1] = 0.5f;
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v1][0];
				vertices[vert_idx].position[1] = model->vertices[v1][1];
				vertices[vert_idx].position[2] = model->vertices[v1][2];
				KD_CalcTexCoord(model->vertices[v1], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				vertices[vert_idx].lmcoord[0] = 0.5f;
				vertices[vert_idx].lmcoord[1] = 0.5f;
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v2][0];
				vertices[vert_idx].position[1] = model->vertices[v2][1];
				vertices[vert_idx].position[2] = model->vertices[v2][2];
				KD_CalcTexCoord(model->vertices[v2], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				vertices[vert_idx].lmcoord[0] = 0.5f;
				vertices[vert_idx].lmcoord[1] = 0.5f;
				vert_idx++;

				// Back face (CW) - reversed winding
				vertices[vert_idx].position[0] = model->vertices[v0][0];
				vertices[vert_idx].position[1] = model->vertices[v0][1];
				vertices[vert_idx].position[2] = model->vertices[v0][2];
				KD_CalcTexCoord(model->vertices[v0], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				vertices[vert_idx].lmcoord[0] = 0.5f;
				vertices[vert_idx].lmcoord[1] = 0.5f;
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v2][0];
				vertices[vert_idx].position[1] = model->vertices[v2][1];
				vertices[vert_idx].position[2] = model->vertices[v2][2];
				KD_CalcTexCoord(model->vertices[v2], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				vertices[vert_idx].lmcoord[0] = 0.5f;
				vertices[vert_idx].lmcoord[1] = 0.5f;
				vert_idx++;

				vertices[vert_idx].position[0] = model->vertices[v1][0];
				vertices[vert_idx].position[1] = model->vertices[v1][1];
				vertices[vert_idx].position[2] = model->vertices[v1][2];
				KD_CalcTexCoord(model->vertices[v1], surf->texinfo_vecs, gltex, &s, &t);
				vertices[vert_idx].texcoord[0] = s;
				vertices[vert_idx].texcoord[1] = t;
				vertices[vert_idx].lmcoord[0] = 0.5f;
				vertices[vert_idx].lmcoord[1] = 0.5f;
				vert_idx++;
			}
		}
	}

	// Draw final batch
	if (vert_idx > batch_start)
	{
		vkCmdBindVertexBuffers (cbx->cb, 0, 1, &buffer, &buffer_offset);
		vkCmdDraw (cbx->cb, vert_idx - batch_start, 1, batch_start, 0);
	}
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

	const char *filename = Cmd_Argv (1);
	kdmodel_t *model = KD_LoadModel (filename);

	// This will free any existing model and set the new one, including loading sky
	KD_SetDebugModel (model);

	if (kd_debug_model)
	{
		Con_Printf ("Loaded %s for debug rendering\n", filename);
		Con_Printf ("  Surfaces: %d, Vertices: %d\n",
		            kd_debug_model->num_surfaces, kd_debug_model->num_vertices);

		// Print first few surfaces for debugging
		for (int i = 0; i < kd_debug_model->num_surfaces && i < 6; i++)
		{
			mkdsurface_t *s = &kd_debug_model->surfaces[i];
			Con_Printf ("  Surf %d: first_vert=%d, num_verts=%d, tex=%s\n",
			            i, s->first_vertex, s->num_vertices,
			            s->gltexture ? s->gltexture->name : "(none)");
		}
	}
	else
		Con_Printf ("Failed to load %s\n", filename);
}

/*
=================
KD_ParseSkyFromEntities

Parse the "sky" key from worldspawn entity and load the skybox
=================
*/
static void KD_ParseSkyFromEntities (const char *entities)
{
	const char *data;
	char        key[128];
	char        value[1024];

	if (!entities)
	{
		Con_DPrintf ("KD_ParseSkyFromEntities: no entities\n");
		return;
	}

	Con_DPrintf ("KD_ParseSkyFromEntities: parsing entities:\n%.200s\n", entities);

	data = entities;

	// Parse first entity (should be worldspawn)
	data = COM_Parse (data);
	if (!data || com_token[0] != '{')
		return;

	while (1)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] == '}')
			break;

		// Handle keys with leading underscore
		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));

		// Remove trailing spaces
		while (key[0] && key[strlen (key) - 1] == ' ')
			key[strlen (key) - 1] = 0;

		// Parse value
		data = COM_Parse (data);
		if (!data)
			break;
		q_strlcpy (value, com_token, sizeof (value));

		// Check for sky keys
		if (!strcmp ("sky", key) || !strcmp ("skyname", key) || !strcmp ("qlsky", key))
		{
			Con_Printf ("KD: Loading skybox '%s'\n", value);
			Sky_LoadSkyBox (value);
			// Note: If this fails (no skybox files), the sky will render as solid color
			// which is fine - the stencil still works
			return;
		}
	}
}

/*
=================
KD_SetDebugModel

Set the debug model directly (for use by kd_compile)
=================
*/
void KD_SetDebugModel (kdmodel_t *model)
{
	if (kd_debug_model && kd_debug_model != model)
	{
		KD_FreeModel (kd_debug_model);
	}
	kd_debug_model = model;

	// Load sky from entities if present
	if (model && model->entities)
	{
		KD_ParseSkyFromEntities (model->entities);
	}
}

/*
=================
KD_RenderInit
=================
*/
void KD_RenderInit (void)
{
	Cvar_RegisterVariable (&kd_draw);
	Cvar_RegisterVariable (&kd_noclip_bsp);
	Cvar_RegisterVariable (&kd_norender_bsp);
	Cmd_AddCommand ("kd_load", KD_LoadDebug_f);
}

/*
=================
KD_ShouldSkipBSPCollision

Returns true if BSP collision should be skipped (kd_noclip_bsp is set and kd model is active)
=================
*/
qboolean KD_ShouldSkipBSPCollision (void)
{
	return kd_noclip_bsp.value && kd_debug_model != NULL;
}

/*
=================
KD_ShouldSkipBSPRendering

Returns true if BSP rendering should be skipped (kd_norender_bsp is set and kd model is active)
=================
*/
qboolean KD_ShouldSkipBSPRendering (void)
{
	return kd_norender_bsp.value && kd_debug_model != NULL;
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

/*
=================
KD_DrawSkySurfaces

Process KD sky surfaces for the sky rendering system.
This draws the sky polygons to the stencil buffer so the skybox
can be rendered where they are visible.
=================
*/
void KD_DrawSkySurfaces (cb_context_t *cbx, float color[3])
{
	if (!kd_debug_model || !kd_draw.value)
		return;

	kdmodel_t *model = kd_debug_model;
	int sky_count = 0;

	// Process each sky surface
	for (int i = 0; i < model->num_surfaces; i++)
	{
		mkdsurface_t *surf = &model->surfaces[i];
		if (surf->num_vertices < 3)
			continue;

		// Only process sky textures
		gltexture_t *gltex = surf->gltexture;
		if (!gltex || strncmp(gltex->name, "kd:sky", 6) != 0)
			continue;

		// Create a temporary glpoly_t for Sky_ProcessPoly
		int numverts = surf->num_vertices;
		size_t poly_size = sizeof(glpoly_t) + (numverts - 4) * VERTEXSIZE * sizeof(float);
		glpoly_t *p = (glpoly_t *)alloca(poly_size);
		memset(p, 0, poly_size);
		p->numverts = numverts;

		// Copy vertex positions into the poly
		for (int v = 0; v < numverts; v++)
		{
			int vidx = surf->first_vertex + v;
			if (vidx < model->num_vertices)
			{
				p->verts[v][0] = model->vertices[vidx][0];
				p->verts[v][1] = model->vertices[vidx][1];
				p->verts[v][2] = model->vertices[vidx][2];
				// Texcoords not needed for sky stencil
				p->verts[v][3] = 0;
				p->verts[v][4] = 0;
			}
		}

		// Process the polygon through the sky system
		Sky_ProcessPoly(cbx, p, color);
		sky_count++;
	}

	if (sky_count > 0)
		Con_DPrintf("KD_DrawSkySurfaces: processed %d sky polys\n", sky_count);
}
