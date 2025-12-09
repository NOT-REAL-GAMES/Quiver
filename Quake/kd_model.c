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
// kd_model.c -- Kd-tree map loading and management

#include "quakedef.h"
#include "kd_model.h"

//=============================================================================
// Local Variables
//=============================================================================

static byte *kd_mod_base; // Base pointer for lump access during loading

//=============================================================================
// Lump Loading Functions
//=============================================================================

/*
=================
KD_LoadPlanes
=================
*/
static void KD_LoadPlanes (kdmodel_t *model, dkdlump_t *lump)
{
	dkdplane_t *in;
	mplane_t   *out;
	int         i;

	in = reinterpret_cast<dkdplane_t *>(kd_mod_base + lump->offset);
	if (lump->length % sizeof (dkdplane_t))
		Sys_Error ("KD_LoadPlanes: invalid lump size");

	model->num_planes = lump->length / sizeof (dkdplane_t);
	model->planes = static_cast<mplane_t *>(Mem_Alloc (model->num_planes * sizeof (mplane_t)));
	out = model->planes;

	for (i = 0; i < model->num_planes; i++, in++, out++)
	{
		out->normal[0] = LittleFloat (in->normal[0]);
		out->normal[1] = LittleFloat (in->normal[1]);
		out->normal[2] = LittleFloat (in->normal[2]);
		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);

		// Compute signbits
		out->signbits = 0;
		for (int j = 0; j < 3; j++)
		{
			if (out->normal[j] < 0)
				out->signbits |= (1 << j);
		}
	}
}

/*
=================
KD_LoadVertices
=================
*/
static void KD_LoadVertices (kdmodel_t *model, dkdlump_t *lump)
{
	dkdvertex_t *in;
	vec3_t      *out;
	int          i;

	in = reinterpret_cast<dkdvertex_t *>(kd_mod_base + lump->offset);
	if (lump->length % sizeof (dkdvertex_t))
		Sys_Error ("KD_LoadVertices: invalid lump size");

	model->num_vertices = lump->length / sizeof (dkdvertex_t);
	model->vertices = static_cast<vec3_t *>(Mem_Alloc (model->num_vertices * sizeof (vec3_t)));
	out = model->vertices;

	for (i = 0; i < model->num_vertices; i++, in++, out++)
	{
		(*out)[0] = LittleFloat (in->position[0]);
		(*out)[1] = LittleFloat (in->position[1]);
		(*out)[2] = LittleFloat (in->position[2]);
	}
}

/*
=================
KD_LoadLeaves
=================
*/
static void KD_LoadLeaves (kdmodel_t *model, dkdlump_t *lump)
{
	dkdleaf_t *in;
	mkdleaf_t *out;
	int        i;

	in = reinterpret_cast<dkdleaf_t *>(kd_mod_base + lump->offset);
	if (lump->length % sizeof (dkdleaf_t))
		Sys_Error ("KD_LoadLeaves: invalid lump size");

	model->num_leaves = lump->length / sizeof (dkdleaf_t);
	model->leaves = static_cast<mkdleaf_t *>(Mem_Alloc (model->num_leaves * sizeof (mkdleaf_t)));
	out = model->leaves;

	for (i = 0; i < model->num_leaves; i++, in++, out++)
	{
		out->contents = LittleLong (in->contents);
		out->zone = LittleLong (in->zone);
		out->first_surface = LittleLong (in->first_surface);
		out->num_surfaces = LittleLong (in->num_surfaces);
		out->mins[0] = LittleFloat (in->mins[0]);
		out->mins[1] = LittleFloat (in->mins[1]);
		out->mins[2] = LittleFloat (in->mins[2]);
		out->maxs[0] = LittleFloat (in->maxs[0]);
		out->maxs[1] = LittleFloat (in->maxs[1]);
		out->maxs[2] = LittleFloat (in->maxs[2]);
		out->visframe = -1;
		out->surfaces = NULL; // Will be set up after surfaces are loaded
	}
}

/*
=================
KD_LoadNodes
=================
*/
static void KD_LoadNodes (kdmodel_t *model, dkdlump_t *lump)
{
	dkdnode_t *in;
	mkdnode_t *out;
	int        i;

	in = reinterpret_cast<dkdnode_t *>(kd_mod_base + lump->offset);
	if (lump->length % sizeof (dkdnode_t))
		Sys_Error ("KD_LoadNodes: invalid lump size");

	model->num_nodes = lump->length / sizeof (dkdnode_t);
	model->nodes = static_cast<mkdnode_t *>(Mem_Alloc (model->num_nodes * sizeof (mkdnode_t)));
	out = model->nodes;

	for (i = 0; i < model->num_nodes; i++, in++, out++)
	{
		out->axis = in->axis;
		out->split_dist = LittleFloat (in->split_dist);

		out->mins[0] = LittleFloat (in->mins[0]);
		out->mins[1] = LittleFloat (in->mins[1]);
		out->mins[2] = LittleFloat (in->mins[2]);
		out->maxs[0] = LittleFloat (in->maxs[0]);
		out->maxs[1] = LittleFloat (in->maxs[1]);
		out->maxs[2] = LittleFloat (in->maxs[2]);

		// Store child indices directly (negative = leaf index via ~)
		for (int j = 0; j < 2; j++)
		{
			out->children[j] = LittleLong (in->children[j]);
		}
	}

	// Set head node to first node
	if (model->num_nodes > 0)
		model->headnode = model->nodes;
	else
		model->headnode = NULL;
}

/*
=================
KD_LoadSurfaces
=================
*/
static void KD_LoadSurfaces (kdmodel_t *model, dkdlump_t *lump)
{
	dkdsurface_t *in;
	mkdsurface_t *out;
	int           i, j;

	in = reinterpret_cast<dkdsurface_t *>(kd_mod_base + lump->offset);
	if (lump->length % sizeof (dkdsurface_t))
		Sys_Error ("KD_LoadSurfaces: invalid lump size");

	model->num_surfaces = lump->length / sizeof (dkdsurface_t);
	model->surfaces = static_cast<mkdsurface_t *>(Mem_Alloc (model->num_surfaces * sizeof (mkdsurface_t)));
	out = model->surfaces;

	for (i = 0; i < model->num_surfaces; i++, in++, out++)
	{
		int plane_num = LittleLong (in->plane_num);
		if (plane_num >= 0 && plane_num < model->num_planes)
			out->plane = model->planes + plane_num;
		else
			out->plane = NULL;

		out->flags = LittleLong (in->flags);

		int tex_num = LittleLong (in->texture_num);
		if (tex_num >= 0 && tex_num < model->num_textures && model->textures)
			out->texture = model->textures[tex_num];
		else
			out->texture = NULL;

		out->first_vertex = LittleLong (in->first_vertex);
		out->num_vertices = LittleLong (in->num_vertices);

		out->light_s = LittleLong (in->lightmap_s);
		out->light_t = LittleLong (in->lightmap_t);
		out->light_w = LittleLong (in->lightmap_w);
		out->light_h = LittleLong (in->lightmap_h);

		// Copy texture vectors
		for (j = 0; j < 2; j++)
		{
			out->texinfo_vecs[j][0] = LittleFloat (in->texinfo_vecs[j][0]);
			out->texinfo_vecs[j][1] = LittleFloat (in->texinfo_vecs[j][1]);
			out->texinfo_vecs[j][2] = LittleFloat (in->texinfo_vecs[j][2]);
			out->texinfo_vecs[j][3] = LittleFloat (in->texinfo_vecs[j][3]);
		}

		out->visframe = -1;
		out->texturechain = NULL;
		out->vbo_firstvert = -1;
		out->indirect_idx = -1;
		out->lightmap_num = -1;
		out->samples = NULL;
	}
}

/*
=================
KD_LoadZones
=================
*/
static void KD_LoadZones (kdmodel_t *model, dkdlump_t *lump)
{
	dkdzone_t *in;
	mkdzone_t *out;
	int        i;

	if (lump->length == 0)
	{
		model->num_zones = 0;
		model->zones = NULL;
		return;
	}

	in = reinterpret_cast<dkdzone_t *>(kd_mod_base + lump->offset);
	if (lump->length % sizeof (dkdzone_t))
		Sys_Error ("KD_LoadZones: invalid lump size");

	model->num_zones = lump->length / sizeof (dkdzone_t);
	model->zones = static_cast<mkdzone_t *>(Mem_Alloc (model->num_zones * sizeof (mkdzone_t)));
	out = model->zones;

	for (i = 0; i < model->num_zones; i++, in++, out++)
	{
		out->mins[0] = LittleFloat (in->mins[0]);
		out->mins[1] = LittleFloat (in->mins[1]);
		out->mins[2] = LittleFloat (in->mins[2]);
		out->maxs[0] = LittleFloat (in->maxs[0]);
		out->maxs[1] = LittleFloat (in->maxs[1]);
		out->maxs[2] = LittleFloat (in->maxs[2]);
		out->vis_size = LittleLong (in->vis_size);
		out->first_leaf = LittleLong (in->first_leaf);
		out->num_leaves = LittleLong (in->num_leaves);

		// Visibility data will be loaded separately if present
		out->vis = NULL;
	}
}

/*
=================
KD_LoadSubmodels
=================
*/
static void KD_LoadSubmodels (kdmodel_t *model, dkdlump_t *lump)
{
	dkdmodel_t    *in;
	mkdsubmodel_t *out;
	int            i;

	if (lump->length == 0)
	{
		model->num_submodels = 0;
		model->submodels = NULL;
		return;
	}

	in = reinterpret_cast<dkdmodel_t *>(kd_mod_base + lump->offset);
	if (lump->length % sizeof (dkdmodel_t))
		Sys_Error ("KD_LoadSubmodels: invalid lump size");

	model->num_submodels = lump->length / sizeof (dkdmodel_t);
	model->submodels = static_cast<mkdsubmodel_t *>(Mem_Alloc (model->num_submodels * sizeof (mkdsubmodel_t)));
	out = model->submodels;

	for (i = 0; i < model->num_submodels; i++, in++, out++)
	{
		out->mins[0] = LittleFloat (in->mins[0]);
		out->mins[1] = LittleFloat (in->mins[1]);
		out->mins[2] = LittleFloat (in->mins[2]);
		out->maxs[0] = LittleFloat (in->maxs[0]);
		out->maxs[1] = LittleFloat (in->maxs[1]);
		out->maxs[2] = LittleFloat (in->maxs[2]);
		out->origin[0] = LittleFloat (in->origin[0]);
		out->origin[1] = LittleFloat (in->origin[1]);
		out->origin[2] = LittleFloat (in->origin[2]);

		int headnode_idx = LittleLong (in->headnode);
		if (headnode_idx >= 0 && headnode_idx < model->num_nodes)
			out->headnode = model->nodes + headnode_idx;
		else
			out->headnode = model->headnode; // Fallback to main tree

		out->first_surface = LittleLong (in->first_surface);
		out->num_surfaces = LittleLong (in->num_surfaces);
	}
}

/*
=================
KD_LoadVisibility
=================
*/
static void KD_LoadVisibility (kdmodel_t *model, dkdlump_t *lump)
{
	if (lump->length == 0)
	{
		model->visdata = NULL;
		model->visdata_size = 0;
		return;
	}

	model->visdata_size = lump->length;
	model->visdata = static_cast<byte *>(Mem_Alloc (lump->length));
	memcpy (model->visdata, kd_mod_base + lump->offset, lump->length);
}

/*
=================
KD_LoadLighting
=================
*/
static void KD_LoadLighting (kdmodel_t *model, dkdlump_t *lump)
{
	if (lump->length == 0)
	{
		model->lightdata = NULL;
		model->lightdata_size = 0;
		return;
	}

	model->lightdata_size = lump->length;
	model->lightdata = static_cast<byte *>(Mem_Alloc (lump->length));
	memcpy (model->lightdata, kd_mod_base + lump->offset, lump->length);
}

/*
=================
KD_LoadEntities
=================
*/
static void KD_LoadEntities (kdmodel_t *model, dkdlump_t *lump)
{
	if (lump->length == 0)
	{
		model->entities = NULL;
		return;
	}

	model->entities = static_cast<char *>(Mem_Alloc (lump->length + 1));
	memcpy (model->entities, kd_mod_base + lump->offset, lump->length);
	model->entities[lump->length] = '\0';
}

//=============================================================================
// Main Loading Functions
//=============================================================================

/*
=================
KD_LoadModel
=================
*/
kdmodel_t *KD_LoadModel (const char *name)
{
	byte        *buf;
	dkdheader_t *header;
	kdmodel_t   *model;
	int          i;

	// Load file
	buf = COM_LoadFile (name, NULL);
	if (!buf)
	{
		Con_Warning ("KD_LoadModel: couldn't load %s\n", name);
		return NULL;
	}
	(void)com_filesize; // Could be used for validation in future

	// Check header
	header = (dkdheader_t *)buf;
	if (LittleLong (header->magic) != KDFORMAT_MAGIC)
	{
		Mem_Free (buf);
		Con_Warning ("KD_LoadModel: %s is not a kd-tree map\n", name);
		return NULL;
	}

	int version = LittleLong (header->version);
	if (version != KDFORMAT_VERSION)
	{
		Mem_Free (buf);
		Con_Warning ("KD_LoadModel: %s has unsupported version %d\n", name, version);
		return NULL;
	}

	// Allocate model
	model = static_cast<kdmodel_t *>(Mem_Alloc (sizeof (kdmodel_t)));
	memset (model, 0, sizeof (kdmodel_t));
	q_strlcpy (model->name, name, sizeof (model->name));
	model->version = version;

	// Set base for lump loading
	kd_mod_base = buf;

	// Byte-swap lump offsets and lengths
	dkdlump_t lumps[KD_LUMP_NUM];
	for (i = 0; i < KD_LUMP_NUM; i++)
	{
		lumps[i].offset = LittleLong (header->lumps[i].offset);
		lumps[i].length = LittleLong (header->lumps[i].length);
	}

	// Load lumps in dependency order
	KD_LoadEntities (model, &lumps[KD_LUMP_ENTITIES]);
	KD_LoadPlanes (model, &lumps[KD_LUMP_PLANES]);
	KD_LoadVertices (model, &lumps[KD_LUMP_VERTICES]);
	// TODO: KD_LoadTextures (model, &lumps[KD_LUMP_TEXTURES]);
	KD_LoadLeaves (model, &lumps[KD_LUMP_KDLEAVES]);
	KD_LoadNodes (model, &lumps[KD_LUMP_KDNODES]);
	KD_LoadSurfaces (model, &lumps[KD_LUMP_SURFACES]);
	KD_LoadZones (model, &lumps[KD_LUMP_ZONES]);
	KD_LoadSubmodels (model, &lumps[KD_LUMP_MODELS]);
	KD_LoadVisibility (model, &lumps[KD_LUMP_VISIBILITY]);
	KD_LoadLighting (model, &lumps[KD_LUMP_LIGHTMAPS]);

	// Compute overall bounds from headnode or submodel 0
	if (model->num_submodels > 0)
	{
		VectorCopy (model->submodels[0].mins, model->mins);
		VectorCopy (model->submodels[0].maxs, model->maxs);
	}
	else if (model->headnode)
	{
		VectorCopy (model->headnode->mins, model->mins);
		VectorCopy (model->headnode->maxs, model->maxs);
	}

	Mem_Free (buf);

	Con_Printf ("Loaded kd-tree map: %s (%d nodes, %d leaves, %d surfaces)\n",
	            name, model->num_nodes, model->num_leaves, model->num_surfaces);

	return model;
}

/*
=================
KD_FreeModel
=================
*/
void KD_FreeModel (kdmodel_t *model)
{
	if (!model)
		return;

	Mem_Free (model->planes);
	Mem_Free (model->vertices);
	Mem_Free (model->leaves);
	Mem_Free (model->nodes);
	Mem_Free (model->surfaces);
	Mem_Free (model->zones);
	Mem_Free (model->submodels);
	Mem_Free (model->visdata);
	Mem_Free (model->lightdata);
	Mem_Free (model->entities);
	Mem_Free (model->brushes);

	// TODO: Free GPU resources

	Mem_Free (model);
}

//=============================================================================
// Tree Traversal Functions
//=============================================================================

/*
=================
KD_PointInLeaf
=================
*/
mkdleaf_t *KD_PointInLeaf (kdmodel_t *model, vec3_t point)
{
	if (!model || model->num_nodes == 0)
		return model->num_leaves > 0 ? &model->leaves[0] : NULL;

	int node_idx = 0;

	// Special case: if the root node is a leaf marker, return first leaf
	if (model->nodes[0].axis == KD_AXIS_LEAF)
	{
		int leaf_idx = model->nodes[0].children[0];
		if (leaf_idx < 0)
			leaf_idx = ~leaf_idx;
		if (leaf_idx < model->num_leaves)
			return &model->leaves[leaf_idx];
		return model->num_leaves > 0 ? &model->leaves[0] : NULL;
	}

	while (1)
	{
		mkdnode_t *node = &model->nodes[node_idx];

		// Compare point against split plane
		float dist = point[node->axis] - node->split_dist;
		int child = (dist > 0) ? node->children[0] : node->children[1];

		// Check if child is a leaf (negative index)
		if (child < 0)
		{
			int leaf_idx = ~child;
			if (leaf_idx < model->num_leaves)
				return &model->leaves[leaf_idx];
			return model->num_leaves > 0 ? &model->leaves[0] : NULL;
		}

		// Check if child node is a leaf marker
		if (model->nodes[child].axis == KD_AXIS_LEAF)
		{
			int leaf_idx = model->nodes[child].children[0];
			if (leaf_idx < 0)
				leaf_idx = ~leaf_idx;
			if (leaf_idx < model->num_leaves)
				return &model->leaves[leaf_idx];
			return model->num_leaves > 0 ? &model->leaves[0] : NULL;
		}

		node_idx = child;
	}
}

/*
=================
KD_BoxLeaves_r
=================
*/
static int KD_BoxLeaves_r (kdmodel_t *model, int node_idx, vec3_t mins, vec3_t maxs, mkdleaf_t **list, int listsize, int count)
{
	// Check for leaf
	if (node_idx < 0)
	{
		int leaf_idx = ~node_idx;
		if (leaf_idx < model->num_leaves && count < listsize)
			list[count] = &model->leaves[leaf_idx];
		return count + 1;
	}

	mkdnode_t *node = &model->nodes[node_idx];

	if (node->axis == KD_AXIS_LEAF)
	{
		int leaf_idx = node->children[0];
		if (leaf_idx < 0)
			leaf_idx = ~leaf_idx;
		if (leaf_idx < model->num_leaves && count < listsize)
			list[count] = &model->leaves[leaf_idx];
		return count + 1;
	}

	// Check which side(s) the box is on
	float split = node->split_dist;
	int   axis = node->axis;

	if (mins[axis] > split)
	{
		// Entirely on front side
		return KD_BoxLeaves_r (model, node->children[0], mins, maxs, list, listsize, count);
	}
	else if (maxs[axis] < split)
	{
		// Entirely on back side
		return KD_BoxLeaves_r (model, node->children[1], mins, maxs, list, listsize, count);
	}
	else
	{
		// Spans both sides
		count = KD_BoxLeaves_r (model, node->children[0], mins, maxs, list, listsize, count);
		return KD_BoxLeaves_r (model, node->children[1], mins, maxs, list, listsize, count);
	}
}

/*
=================
KD_BoxLeaves
=================
*/
int KD_BoxLeaves (kdmodel_t *model, vec3_t mins, vec3_t maxs, mkdleaf_t **list, int listsize)
{
	if (!model || model->num_nodes == 0)
		return 0;

	return KD_BoxLeaves_r (model, 0, mins, maxs, list, listsize, 0);
}

//=============================================================================
// Visibility Functions (stubs for now)
//=============================================================================

/*
=================
KD_LeafPVS
=================
*/
byte *KD_LeafPVS (kdmodel_t *model, mkdleaf_t *leaf)
{
	// TODO: Implement zone-based visibility lookup
	(void)model;
	(void)leaf;
	return NULL;
}

/*
=================
KD_LeafVisible
=================
*/
qboolean KD_LeafVisible (kdmodel_t *model, mkdleaf_t *from, mkdleaf_t *to)
{
	// TODO: Implement visibility check
	(void)model;
	(void)from;
	(void)to;
	return true; // For now, everything is visible
}

//=============================================================================
// Note: Collision functions (KD_PointContents, KD_TraceLine, etc.) are in kd_world.c
//=============================================================================

//=============================================================================
// Rendering Functions (stubs for now)
//=============================================================================

/*
=================
KD_BuildVertexBuffer
=================
*/
void KD_BuildVertexBuffer (kdmodel_t *model)
{
	// TODO: Build VBO from surfaces
	(void)model;
}

/*
=================
KD_MarkSurfaces
=================
*/
void KD_MarkSurfaces (kdmodel_t *model, mkdleaf_t *viewleaf)
{
	// TODO: Mark visible surfaces using zone visibility
	(void)model;
	(void)viewleaf;
}

//=============================================================================
// Console Commands
//=============================================================================

/*
=================
KD_Test_f
=================
*/
static void KD_Test_f (void)
{
	if (Cmd_Argc () < 2)
	{
		Con_Printf ("Usage: kd_test <filename.qkd>\n");
		return;
	}

	const char *filename = Cmd_Argv (1);
	Con_Printf ("Testing kd-tree load: %s\n", filename);

	kdmodel_t *model = KD_LoadModel (filename);
	if (!model)
	{
		Con_Printf ("Failed to load %s\n", filename);
		return;
	}

	Con_Printf ("Successfully loaded %s:\n", filename);
	Con_Printf ("  Nodes: %d\n", model->num_nodes);
	Con_Printf ("  Leaves: %d\n", model->num_leaves);
	Con_Printf ("  Surfaces: %d\n", model->num_surfaces);
	Con_Printf ("  Planes: %d\n", model->num_planes);
	Con_Printf ("  Vertices: %d\n", model->num_vertices);
	Con_Printf ("  Zones: %d\n", model->num_zones);
	Con_Printf ("  Submodels: %d\n", model->num_submodels);
	Con_Printf ("  Bounds: (%.1f, %.1f, %.1f) - (%.1f, %.1f, %.1f)\n",
	            model->mins[0], model->mins[1], model->mins[2],
	            model->maxs[0], model->maxs[1], model->maxs[2]);

	if (model->entities)
		Con_Printf ("  Entities: %zu bytes\n", strlen (model->entities));

	// Test point-in-leaf
	vec3_t test_point = {0, 0, 0};
	mkdleaf_t *leaf = KD_PointInLeaf (model, test_point);
	if (leaf)
	{
		Con_Printf ("  Point (0,0,0) is in leaf with contents %d\n", leaf->contents);
	}

	KD_FreeModel (model);
	Con_Printf ("Model freed successfully\n");
}

/*
=================
KD_Init
=================
*/
void KD_Init (void)
{
	Cmd_AddCommand ("kd_test", KD_Test_f);
}
