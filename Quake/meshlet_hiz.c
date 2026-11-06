/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "quakedef.h"
#include "meshlet_hiz.h"
#include "../Shaders/shaders.h"

// The map cache keeps large faces in different orientations in each
// spatial cell. Missing candidates can reduce culling, never hide geometry.
enum
{
	CELL_SIZE = 512,
	DIRECTIONS = 6,
	PER_DIRECTION = 8,
	MAX_VERTS = 12,
	MAX_SELECTED = 256
};
typedef struct
{
	float plane[4];
	float verts[MAX_VERTS][4]; // first vertex w holds the count
} polygon_t;
typedef struct
{
	const msurface_t *surface;
	int				  first, count; // vertex 0, then count-1 consecutive fan vertices
	float			  area, center[3], radius;
} candidate_t;
typedef struct
{
	int			coord[3];
	bool		occupied;
	candidate_t faces[DIRECTIONS][PER_DIRECTION];
} cell_t;
typedef struct
{
	float			   score;
	const candidate_t *face;
} ranked_t;
typedef struct
{
	VkBuffer	   handle;
	VkDeviceMemory memory;
	void		  *mapped;
} buffer_t;
typedef struct
{
	VkDescriptorSet set;
	buffer_t		depth, input, readback;
	bool			pending;
	bool			shortlist_valid, drew_world;
	vec3_t			eye, forward;
	uint32_t		shortlist_count;
} slot_t;
static const qmodel_t		*map;
static cell_t				*cells;
static uint32_t				 num_cells, baked, selected, last_counts[ML_HIZ_COUNTERS];
static bool					 have_counts, counting;
static VkDescriptorSetLayout layout;
static VkDescriptorPool		 pool;
static vulkan_pipeline_t	 raster, reduce;
static slot_t				 slots[DOUBLE_BUFFERED];
static bool					 available;

VkDescriptorSetLayout MLHiZ_Layout (void)
{
	const VkPhysicalDeviceLimits *limits = &vulkan_globals.device_properties.limits;
	if (!layout && limits->maxBoundDescriptorSets >= 2 && limits->maxPerStageDescriptorStorageBuffers >= 8 && limits->maxDescriptorSetStorageBuffers >= 8 &&
		limits->maxPerStageResources >= 8)
	{
		VkDescriptorSetLayoutBinding bindings[2] = {
			{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
		VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, bindings};
		if (vkCreateDescriptorSetLayout (vulkan_globals.device, &info, NULL, &layout) != VK_SUCCESS)
			layout = VK_NULL_HANDLE;
	}
	return layout;
}

static void FreeBuffer (buffer_t *b)
{
	VkDevice device = vulkan_globals.device;
	if (b->mapped)
		vkUnmapMemory (device, b->memory);
	vkDestroyBuffer (device, b->handle, NULL);
	vkFreeMemory (device, b->memory, NULL);
	memset (b, 0, sizeof (*b));
}

// Optional resources must not make a normally playable map fail to load.
static bool TryBuffer (buffer_t *b, VkDeviceSize bytes, VkBufferUsageFlags usage, bool host)
{
	VkDevice		   device = vulkan_globals.device;
	VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = usage};
	if (vkCreateBuffer (device, &info, NULL, &b->handle) != VK_SUCCESS)
	{
		b->handle = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements (device, b->handle, &req);
	VkMemoryPropertyFlags required = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	const VkPhysicalDeviceMemoryProperties *memory = &vulkan_globals.memory_properties;
	for (uint32_t i = 0; i < memory->memoryTypeCount; ++i)
	{
		if (!(req.memoryTypeBits & (1u << i)) || (memory->memoryTypes[i].propertyFlags & required) != required)
			continue;
		VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, i};
		if (vkAllocateMemory (device, &alloc, NULL, &b->memory) != VK_SUCCESS)
		{
			b->memory = VK_NULL_HANDLE;
			continue;
		}
		if (vkBindBufferMemory (device, b->handle, b->memory, 0) != VK_SUCCESS)
		{
			vkFreeMemory (device, b->memory, NULL);
			b->memory = VK_NULL_HANDLE;
			continue;
		}
		if (host && vkMapMemory (device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS)
		{
			b->mapped = NULL;
			return false;
		}
		return true;
	}
	return false;
}

static bool TryPipeline (vulkan_pipeline_t *p, const unsigned char *spv, size_t bytes)
{
	VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = (const uint32_t *)spv};
	VkShaderModule			 module = VK_NULL_HANDLE;
	VkDevice				 device = vulkan_globals.device;
	if (vkCreateShaderModule (device, &shader, NULL, &module) != VK_SUCCESS)
		return false;
	VkComputePipelineCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.layout = p->layout.handle,
		.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}};
	VkResult result = vkCreateComputePipelines (device, VK_NULL_HANDLE, 1, &ci, NULL, &p->handle);
	vkDestroyShaderModule (device, module, NULL);
	return result == VK_SUCCESS;
}

static void DropResources (void)
{
	VkDevice device = vulkan_globals.device;
	for (int i = 0; i < DOUBLE_BUFFERED; ++i)
	{
		FreeBuffer (&slots[i].depth);
		FreeBuffer (&slots[i].input);
		FreeBuffer (&slots[i].readback);
	}
	vkDestroyDescriptorPool (device, pool, NULL);
	vkDestroyPipeline (device, raster.handle, NULL);
	vkDestroyPipeline (device, reduce.handle, NULL);
	vkDestroyPipelineLayout (device, raster.layout.handle, NULL);
	memset (slots, 0, sizeof (slots));
	memset (&raster, 0, sizeof (raster));
	memset (&reduce, 0, sizeof (reduce));
	pool = VK_NULL_HANDLE;
	available = false;
}

static bool TryResources (void)
{
	if (available)
		return true;
	if (!MLHiZ_Layout ())
		return false;
	VkDevice				   device = vulkan_globals.device;
	VkPushConstantRange		   range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
	VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &layout, 1, &range};
	if (vkCreatePipelineLayout (device, &li, NULL, &raster.layout.handle) != VK_SUCCESS)
		goto fail;
	raster.layout.push_constant_range = range;
	raster.layout.mboit_input_attachment_set = -1;
	reduce.layout = raster.layout;
	if (!TryPipeline (&raster, meshlet_hiz_raster_comp_spv, meshlet_hiz_raster_comp_spv_size) ||
		!TryPipeline (&reduce, meshlet_hiz_reduce_comp_spv, meshlet_hiz_reduce_comp_spv_size))
		goto fail;
	VkDescriptorPoolSize	   size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * DOUBLE_BUFFERED};
	VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, DOUBLE_BUFFERED, 1, &size};
	if (vkCreateDescriptorPool (device, &pi, NULL, &pool) != VK_SUCCESS)
		goto fail;
	for (int i = 0; i < DOUBLE_BUFFERED; ++i)
	{
		slot_t *s = &slots[i];
		if (!TryBuffer (
				&s->depth, (ML_HIZ_WORDS + ML_HIZ_COUNTERS) * sizeof (uint32_t),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
			!TryBuffer (&s->input, MAX_SELECTED * sizeof (polygon_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
			!TryBuffer (&s->readback, ML_HIZ_COUNTERS * sizeof (uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
			goto fail;
		VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &layout};
		if (vkAllocateDescriptorSets (device, &ai, &s->set) != VK_SUCCESS)
			goto fail;
		VkDescriptorBufferInfo db[2] = {{s->depth.handle, 0, VK_WHOLE_SIZE}, {s->input.handle, 0, VK_WHOLE_SIZE}};
		VkWriteDescriptorSet   writes[2] = {
			  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			   .dstSet = s->set,
			   .dstBinding = 0,
			   .descriptorCount = 1,
			   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			   .pBufferInfo = &db[0]},
			  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			   .dstSet = s->set,
			   .dstBinding = 1,
			   .descriptorCount = 1,
			   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			   .pBufferInfo = &db[1]}};
		vkUpdateDescriptorSets (device, 2, writes, 0, NULL);
		GL_SetObjectName ((uint64_t)s->depth.handle, VK_OBJECT_TYPE_BUFFER, "meshlet Hi-Z tiles");
	}
	available = true;
	return true;
fail:
	DropResources ();
	return false;
}

static bool CellCoord (const float p[3], int key[3])
{
	for (int k = 0; k < 3; ++k)
	{
		double d = floor ((double)p[k] / CELL_SIZE);
		if (!(d > -1.0e9 && d < 1.0e9))
			return false;
		key[k] = (int)d;
	}
	return true;
}

static cell_t *FindCell (const int key[3], bool insert)
{
	uint32_t h = (uint32_t)key[0] * 0x8da6b343u ^ (uint32_t)key[1] * 0xd8163841u ^ (uint32_t)key[2] * 0xcb1ab31fu;
	h ^= h >> 16;
	for (int probe = 0; probe < 16; ++probe)
	{
		cell_t *c = &cells[(h + probe) & (num_cells - 1)];
		if (!c->occupied)
		{
			if (!insert)
				return NULL;
			memcpy (c->coord, key, sizeof (c->coord));
			c->occupied = true;
			return c;
		}
		if (!memcmp (c->coord, key, sizeof (c->coord)))
			return c;
	}
	return NULL;
}

// For a planar convex face the union of the actual BSP fan triangles is
// the contour, including their internal diagonals. Only combine strictly
// coplanar, convex contours. Everything else retains the actual triangles.
static bool ConvexFace (const glpoly_t *poly, int count)
{
	if (count < 4 || count > MAX_VERTS)
		return false;
	const float *origin = poly->verts[0];
	double		 u[3], v[3], n[3];
	int			 axis = 0;
	bool		 nonzero = false;
	for (int j = 2; j < count && !nonzero; ++j)
	{
		for (int i = 0; i < 3; ++i)
		{
			u[i] = (double)poly->verts[j - 1][i] - origin[i];
			v[i] = (double)poly->verts[j][i] - origin[i];
		}
		for (int i = 0; i < 3; ++i)
			n[i] = u[(i + 1) % 3] * v[(i + 2) % 3] - u[(i + 2) % 3] * v[(i + 1) % 3];
		for (int i = 0; i < 3; ++i)
			if (fabs (n[i]) > fabs (n[axis]))
				axis = i;
		nonzero = fabs (n[axis]) > 0;
	}
	if (!nonzero)
		return false;
	int x = (axis + 1) % 3, y = (axis + 2) % 3;
	for (int i = 0; i < count; ++i)
	{
		const float *a = poly->verts[i], *b = poly->verts[(i + 1) % count];
		double		 d[3];
		for (int k = 0; k < 3; ++k)
			d[k] = (double)a[k] - origin[k];
		if (DotProduct (d, n) != 0)
			return false;
		// Checking every vertex also rejects self-crossing contours.
		for (int j = 0; j < count; ++j)
		{
			const float *c = poly->verts[j];
			double		 side = ((double)b[x] - a[x]) * ((double)c[y] - a[y]) - ((double)b[y] - a[y]) * ((double)c[x] - a[x]);
			if (side * n[axis] < 0)
				return false;
		}
	}
	return true;
}

static void AddFace (const msurface_t *s, int first, int count)
{
	const glpoly_t *p = s->polys;
	float			center[3] = {0}, area = 0;
	for (int j = 0; j < count; ++j)
	{
		const float *v = p->verts[j ? first + j - 1 : 0];
		VectorAdd (center, v, center);
		if (j >= 2)
		{
			float a[3], b[3], n[3];
			VectorSubtract (p->verts[first + j - 2], p->verts[0], a);
			VectorSubtract (v, p->verts[0], b);
			CrossProduct (a, b, n);
			area += sqrtf (DotProduct (n, n));
		}
	}
	if (!(area > 128.0f && isfinite (area)))
		return;
	VectorScale (center, 1.0f / count, center);
	int key[3];
	if (!CellCoord (center, key))
		return;
	cell_t *cell = FindCell (key, true);
	if (!cell)
		return;
	const float *n = s->plane->normal;
	int			 axis = fabsf (n[0]) >= fabsf (n[1]) ? 0 : 1;
	if (fabsf (n[2]) > fabsf (n[axis]))
		axis = 2;
	bool		 neg = (n[axis] < 0) != !!(s->flags & SURF_PLANEBACK);
	candidate_t *choices = cell->faces[axis * 2 + neg], *dst = choices;
	for (int k = 1; k < PER_DIRECTION; ++k)
		if (choices[k].area < dst->area)
			dst = &choices[k];
	if (dst->area >= area)
		return;
	if (!dst->area)
		++baked;
	dst->area = area;
	dst->surface = s;
	dst->first = first;
	dst->count = count;
	VectorCopy (center, dst->center);
	float max_d2 = 0;
	for (int j = 0; j < count; ++j)
	{
		float delta[3];
		VectorSubtract (p->verts[j ? first + j - 1 : 0], center, delta);
		max_d2 = q_max (max_d2, DotProduct (delta, delta));
	}
	dst->radius = sqrtf (max_d2);
}

void MLHiZ_Reset (void)
{
	free (cells);
	cells = NULL;
	map = NULL;
	num_cells = baked = selected = 0;
	have_counts = counting = false;
	for (int i = 0; i < DOUBLE_BUFFERED; ++i)
	{
		slots[i].pending = false;
		slots[i].shortlist_valid = false;
	}
}

void MLHiZ_Shutdown (void)
{
	MLHiZ_Reset ();
	if (vulkan_globals.device)
	{
		DropResources ();
		vkDestroyDescriptorSetLayout (vulkan_globals.device, layout, NULL);
	}
	layout = VK_NULL_HANDLE;
}

void MLHiZ_Bake (const qmodel_t *world)
{
	if (map == world)
		return;
	MLHiZ_Reset ();
	map = world;
	if (!world || !world->surfaces || world->firstmodelsurface || world->nummodelsurfaces <= 0 || world->nummodelsurfaces > world->numsurfaces ||
		!TryResources ())
		return;
	num_cells = 64;
	while (num_cells < 8192 && num_cells < (uint32_t)world->nummodelsurfaces / 2)
		num_cells *= 2;
	cells = calloc (num_cells, sizeof (*cells));
	if (!cells)
		return;
	for (int i = 0; i < world->nummodelsurfaces; ++i)
	{
		const msurface_t *s = &world->surfaces[i];
		const texture_t	 *t = s->texinfo ? s->texinfo->texture : NULL;
		if (!s->plane || !s->polys || !t || !t->gltexture || t->type != TEXTYPE_DEFAULT || t->anim_total || t->anim_next || t->alternate_anims ||
			(s->flags & ~(SURF_PLANEBACK | SURF_UNDERWATER)) || s->numedges < 3 || s->numedges != s->polys->numverts)
			continue;
		if (!isfinite (s->plane->normal[0]) || !isfinite (s->plane->normal[1]) || !isfinite (s->plane->normal[2]) || !isfinite (s->plane->dist))
			continue;
		if (ConvexFace (s->polys, s->numedges))
			AddFace (s, 1, s->numedges);
		else
			for (int k = 2; k < s->numedges; ++k)
				AddFace (s, k - 1, 3);
	}
}

static uint32_t Select (polygon_t *out)
{
	ranked_t heap[MAX_SELECTED]; // min-heap of highest projected-area estimates
	int		 eye_cell[3];
	if (!cells || !CellCoord (r_origin, eye_cell) || !r_drawworld_cheatsafe)
		return 0;
	uint32_t count = 0;
	for (int z = -2; z <= 2; ++z)
		for (int y = -2; y <= 2; ++y)
			for (int x = -2; x <= 2; ++x)
			{
				int			  key[3] = {eye_cell[0] + x, eye_cell[1] + y, eye_cell[2] + z};
				const cell_t *cell = FindCell (key, false);
				if (!cell)
					continue;
				for (int d = 0; d < DIRECTIONS; ++d)
					for (int k = 0; k < PER_DIRECTION; ++k)
					{
						const candidate_t *c = &cell->faces[d][k];
						if (!c->area)
							continue;
						const msurface_t *s = c->surface;
						float			  side = DotProduct (r_origin, s->plane->normal) - s->plane->dist;
						if ((s->flags & SURF_PLANEBACK ? -side : side) <= 0)
							continue;
						vec3_t delta;
						VectorSubtract (c->center, r_origin, delta);
						if (DotProduct (vpn, delta) + c->radius <= 0)
							continue;
						float score = c->area / q_max (4096.0f, DotProduct (delta, delta));
						if (count == MAX_SELECTED && score <= heap[0].score)
							continue;
						ranked_t item = {score, c};
						if (count < MAX_SELECTED)
						{
							uint32_t child = count++;
							while (child)
							{
								uint32_t parent = (child - 1) / 2;
								if (heap[parent].score <= score)
									break;
								heap[child] = heap[parent];
								child = parent;
							}
							heap[child] = item;
						}
						else
						{
							uint32_t parent = 0;
							for (uint32_t child = 1; child < count; child = parent * 2 + 1)
							{
								if (child + 1 < count && heap[child + 1].score < heap[child].score)
									++child;
								if (score <= heap[child].score)
									break;
								heap[parent] = heap[child];
								parent = child;
							}
							heap[parent] = item;
						}
					}
			}
	for (uint32_t i = 0; i < count; ++i)
	{
		const candidate_t *c = heap[i].face;
		const msurface_t  *s = c->surface;
		float			   sign = s->flags & SURF_PLANEBACK ? -1.0f : 1.0f;
		VectorScale (s->plane->normal, sign, out[i].plane);
		out[i].plane[3] = s->plane->dist * sign;
		for (int j = 0; j < c->count; ++j)
			VectorCopy (s->polys->verts[j ? c->first + j - 1 : 0], out[i].verts[j]);
		out[i].verts[0][3] = c->count;
	}
	return count;
}

static void Barrier (VkCommandBuffer cb, VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
	VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, src_access, dst_access};
	vkCmdPipelineBarrier (cb, src_stage, dst_stage, 0, 1, &b, 0, NULL, 0, NULL);
}

bool MLHiZ_Record (cb_context_t *cbx, bool build_depth, bool count_stats)
{
	counting = false;
	selected = 0;
	if (!available || !map || (!baked && build_depth) || r_scene_vrect.width <= 0 || r_scene_vrect.height <= 0)
		return false;
	slot_t *s = &slots[GL_RenderFrameSlot ()];
	// GL_BeginRenderingTask has already waited for the prior submission using
	// this slot. Never wait for a new result, even when printing diagnostics.
	if (s->pending)
	{
		memcpy (last_counts, s->readback.mapped, sizeof (last_counts));
		have_counts = true;
		s->pending = false;
	}
	counting = count_stats;
	vkCmdFillBuffer (cbx->cb, s->depth.handle, 0, VK_WHOLE_SIZE, 0);
	Barrier (
		cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	if (!build_depth)
		return true;
	// Reuse nearby candidates while the camera moves a short distance. Every
	// candidate is still projected and tested with this frame's view, so a
	// stale list can only make occlusion less effective, never hide geometry.
	vec3_t moved;
	VectorSubtract (r_origin, s->eye, moved);
	if (!s->shortlist_valid || s->drew_world != r_drawworld_cheatsafe || !(DotProduct (moved, moved) < 64.0f * 64.0f) ||
		!(DotProduct (s->forward, vpn) > 0.98f))
	{
		s->shortlist_count = Select (s->input.mapped);
		VectorCopy (r_origin, s->eye);
		VectorCopy (vpn, s->forward);
		s->drew_world = r_drawworld_cheatsafe;
		s->shortlist_valid = true;
	}
	selected = s->shortlist_count;
	if (!selected)
		return true; // all levels, including uncovered tiles, already clear
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, raster);
	vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, raster.layout.handle, 0, 1, &s->set, 0, NULL);
	struct
	{
		float mvp[16], eye[4], pixel[2];
	} pc;
	memcpy (pc.mvp, vulkan_globals.view_projection_matrix, sizeof (pc.mvp));
	VectorCopy (r_origin, pc.eye);
	pc.eye[3] = 0;
	pc.pixel[0] = 2.0f / r_scene_vrect.width;
	pc.pixel[1] = 2.0f / r_scene_vrect.height;
	R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (pc), &pc);
	vkCmdDispatch (cbx->cb, selected, 1, 1);

	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, reduce);
	for (uint32_t upper = 0; upper <= 1; ++upper)
	{
		Barrier (
			cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (upper), &upper);
		vkCmdDispatch (cbx->cb, upper ? 1 : 16, 1, 1);
	}
	Barrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	return true;
}

VkBuffer MLHiZ_BufferForSlot (uint32_t slot)
{
	return available && slot < DOUBLE_BUFFERED ? slots[slot].depth.handle : VK_NULL_HANDLE;
}

VkDescriptorSet MLHiZ_Set (void)
{
	return slots[GL_RenderFrameSlot ()].set;
}

void MLHiZ_FinishStats (cb_context_t *cbx)
{
	if (!counting)
		return;
	slot_t *s = &slots[GL_RenderFrameSlot ()];
	Barrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	VkBufferCopy copy = {ML_HIZ_WORDS * sizeof (uint32_t), 0, ML_HIZ_COUNTERS * sizeof (uint32_t)};
	vkCmdCopyBuffer (cbx->cb, s->depth.handle, s->readback.handle, 1, &copy);
	Barrier (cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	s->pending = true;
}

bool MLHiZ_GetStats (uint32_t counts[ML_HIZ_COUNTERS])
{
	if (!have_counts)
		return false;
	memcpy (counts, last_counts, sizeof (last_counts));
	return true;
}

uint32_t MLHiZ_OccluderCount (void)
{
	return baked;
}
uint32_t MLHiZ_ActiveOccluders (void)
{
	return selected;
}
