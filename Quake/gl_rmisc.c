/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

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
// r_misc.c

#include "quakedef.h"
#include "r_ssao.h"
#include "gl_heap.h"
#include "meshlet_hiz.h"
#include "radiance_runtime.h"
#include "radiance_graphics.h"
#include "radiance_receiver_live.h"
#include "radiance_receiver_experimental.h"
#include "radiance_receiver_diagnostics.h"
#include "vulkan_shader_transaction.h"
#include <float.h>

cvar_t r_lodbias = {"r_lodbias", "1", CVAR_ARCHIVE};
cvar_t gl_lodbias = {"gl_lodbias", "0", CVAR_ARCHIVE};

// johnfitz -- new cvars
extern cvar_t r_clearcolor;
extern cvar_t r_fastclear;
extern cvar_t r_flatlightstyles;
extern cvar_t r_lerplightstyles;
extern cvar_t r_entdlightscale;
extern cvar_t gl_fullbrights;
extern cvar_t gl_farclip;
extern cvar_t r_waterquality;
extern cvar_t r_waterwarp;
extern cvar_t r_waterwarpcompute;
extern cvar_t r_oldskyleaf;
extern cvar_t r_drawworld;
extern cvar_t r_showtris;
extern cvar_t r_ecs_batch;
extern cvar_t r_ecs_gpu_cull;
extern cvar_t r_showskel;
extern cvar_t r_showbboxes;
extern cvar_t r_showbboxes_think;
extern cvar_t r_showbboxes_health;
extern cvar_t r_showbboxes_links;
extern cvar_t r_showbboxes_targets;
extern cvar_t r_showfields;
extern cvar_t r_showfields_align;
extern cvar_t r_lerpmodels;
extern cvar_t r_lerpmove;
extern cvar_t r_lerpturn;
extern cvar_t r_nolerp_list;
extern cvar_t r_oit;
// johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix
extern cvar_t r_alphasort;

extern cvar_t r_gpulightmapupdate;
extern cvar_t r_rtshadows;
extern cvar_t r_indirect;
extern cvar_t r_meshlets, r_meshlet_occlusion, r_meshlet_occlusion_stats, r_meshlet_task;
extern cvar_t r_tasks;
extern cvar_t r_parallelmark;

#if defined(USE_SIMD)
extern cvar_t r_simd;
#endif
extern gltexture_t *playertextures[MAX_SCOREBOARD]; // johnfitz

vulkanglobals_t vulkan_globals;

atomic_uint32_t num_vulkan_tex_allocations;
atomic_uint32_t num_vulkan_bmodel_allocations;
atomic_uint32_t num_vulkan_mesh_allocations;
atomic_uint32_t num_vulkan_misc_allocations;
atomic_uint32_t num_vulkan_dynbuf_allocations;
atomic_uint32_t num_vulkan_samplers;
atomic_uint32_t num_vulkan_combined_image_samplers;
atomic_uint32_t num_vulkan_ubos_dynamic;
atomic_uint32_t num_vulkan_ubos;
atomic_uint32_t num_vulkan_storage_buffers;
atomic_uint32_t num_vulkan_input_attachments;
atomic_uint32_t num_vulkan_storage_images;
atomic_uint32_t num_vulkan_sampled_images;
atomic_uint32_t num_acceleration_structures;
atomic_uint64_t total_device_vulkan_allocation_size;
atomic_uint64_t total_host_vulkan_allocation_size;

qboolean   use_simd;
oit_mode_t frame_oit_mode;

static SDL_Mutex *vertex_allocate_mutex;
static SDL_Mutex *index_allocate_mutex;
static SDL_Mutex *uniform_allocate_mutex;
static SDL_Mutex *storage_allocate_mutex;
static SDL_Mutex *garbage_mutex;

extern SDL_Mutex *draw_qcvm_mutex;

qboolean R_UseAlphaSort (void)
{
	return r_alphasort.value && !R_UseOIT ();
}

qboolean R_UseIndirectTransparentWater (void)
{
	return R_UseOIT ();
}

/*
================
Staging
================
*/
#define NUM_STAGING_BUFFERS 2

typedef struct
{
	VkBuffer		buffer;
	VkCommandBuffer command_buffer;
	VkFence			fence;
	int				current_offset;
	qboolean		submitted;
	unsigned char  *data;
} stagingbuffer_t;

static VkCommandPool   staging_command_pool;
static vulkan_memory_t staging_memory;
static stagingbuffer_t staging_buffers[NUM_STAGING_BUFFERS];
static int			   current_staging_buffer = 0;
static int			   num_stagings_in_flight = 0;
static SDL_Mutex	  *staging_mutex;
static SDL_Condition  *staging_cond;
/*
================
Dynamic vertex/index & uniform buffer
================
*/
#define INITIAL_DYNAMIC_VERTEX_BUFFER_SIZE_KB  256
#define INITIAL_DYNAMIC_INDEX_BUFFER_SIZE_KB   1024
#define INITIAL_DYNAMIC_UNIFORM_BUFFER_SIZE_KB 256
#define NUM_DYNAMIC_BUFFERS					   2
#define GARBAGE_FRAME_COUNT					   3
#define MAX_UNIFORM_ALLOC					   2048

static uint32_t		   current_dyn_vertex_buffer_size = INITIAL_DYNAMIC_VERTEX_BUFFER_SIZE_KB * 1024;
static uint32_t		   current_dyn_index_buffer_size = INITIAL_DYNAMIC_INDEX_BUFFER_SIZE_KB * 1024;
static uint32_t		   current_dyn_uniform_buffer_size = INITIAL_DYNAMIC_UNIFORM_BUFFER_SIZE_KB * 1024;
static uint32_t		   current_dyn_storage_buffer_size = 0; // Only used for RT so allocate lazily
static vulkan_memory_t dyn_vertex_buffer_memory;
static vulkan_memory_t dyn_index_buffer_memory;
static vulkan_memory_t dyn_uniform_buffer_memory;
static vulkan_memory_t dyn_storage_buffer_memory;
extern vulkan_memory_t frame_upload_buffers_memory;
static dynbuffer_t	   dyn_vertex_buffers[NUM_DYNAMIC_BUFFERS];
static dynbuffer_t	   dyn_index_buffers[NUM_DYNAMIC_BUFFERS];
static dynbuffer_t	   dyn_uniform_buffers[NUM_DYNAMIC_BUFFERS];
static dynbuffer_t	   dyn_storage_buffers[NUM_DYNAMIC_BUFFERS];
static int			   current_dyn_buffer_index = 0;
int R_ECSGPUFrameIndex (void) { return current_dyn_buffer_index; }
static VkDescriptorSet ubo_descriptor_sets[2];

static int				current_garbage_index = 0;
static int				num_device_memory_garbage[GARBAGE_FRAME_COUNT];
static int				num_buffer_garbage[GARBAGE_FRAME_COUNT];
static int				num_desc_set_garbage[GARBAGE_FRAME_COUNT];
static vulkan_memory_t *device_memory_garbage[GARBAGE_FRAME_COUNT];
static VkDescriptorSet *descriptor_set_garbage[GARBAGE_FRAME_COUNT];
static VkBuffer		   *buffer_garbage[GARBAGE_FRAME_COUNT];

void R_VulkanMemStats_f (void);

/*
================
GL_MemoryTypeFromProperties
================
*/
int GL_TryMemoryTypeFromProperties (uint32_t type_bits, VkFlags requirements_mask, VkFlags preferred_mask)
{
	uint32_t current_type_bits = type_bits;
	uint32_t i;

	for (i = 0; i < VK_MAX_MEMORY_TYPES; i++)
	{
		if ((current_type_bits & 1) == 1)
		{
			if ((vulkan_globals.memory_properties.memoryTypes[i].propertyFlags & (requirements_mask | preferred_mask)) == (requirements_mask | preferred_mask))
				return i;
		}
		current_type_bits >>= 1;
	}

	current_type_bits = type_bits;
	for (i = 0; i < VK_MAX_MEMORY_TYPES; i++)
	{
		if ((current_type_bits & 1) == 1)
		{
			if ((vulkan_globals.memory_properties.memoryTypes[i].propertyFlags & requirements_mask) == requirements_mask)
				return i;
		}
		current_type_bits >>= 1;
	}

	return -1;
}

int GL_MemoryTypeFromProperties (uint32_t type_bits, VkFlags requirements_mask, VkFlags preferred_mask)
{
	int type = GL_TryMemoryTypeFromProperties (type_bits, requirements_mask, preferred_mask);
	if (type < 0) Sys_Error ("Could not find memory type");
	return type;
}

/*
====================
R_ShowbboxesFilter_f
====================
*/
static void R_ShowbboxesFilter_f (void)
{
	extern char	   *r_showbboxes_filter_strings;
	extern qboolean r_showbboxes_filter_byindex;

	if (Cmd_Argc () >= 2)
	{
		// reset
		SAFE_FREE (r_showbboxes_filter_strings);
		r_showbboxes_filter_byindex = false;

		// Concat all arguments adding a ' ' (space) separator
		for (int i = 1; i < Cmd_Argc (); i++)
		{
			const char *arg = Cmd_Argv (i);
			if (!*arg)
				continue;

			r_showbboxes_filter_strings = q_strcatf (r_showbboxes_filter_strings, "%s ", arg);

			r_showbboxes_filter_byindex |= (arg[0] == '#');
		}

		// Re-split r_showbboxes_filter_strings by ' ' (space) on-place, effectively splitting it by '\0'
		if (r_showbboxes_filter_strings)
			q_strsplit (r_showbboxes_filter_strings, " ", NULL);
	}
	else
	{
		const char *p = r_showbboxes_filter_strings;

		Con_SafePrintf ("\"r_showbboxes_filter\" is");
		if (!p)
			Con_SafePrintf (" \"\"");
		else
			do
			{
				Con_SafePrintf (" \"%s\"", p);
				p += strlen (p) + 1;
			} while (*p);
		Con_SafePrintf ("\n");
	}
}

/*
====================
R_ShowbboxesFilter_Completion_f -- tab completion for r_showbboxes_filter
====================
*/
static void R_ShowbboxesFilter_Completion_f (const char *partial)
{
	extern edict_t	*sv_player;
	extern edict_t **bbox_linked;
	edict_t			*ed;
	int				 i;

	if (!sv.active)
		return;

	SDL_LockMutex (draw_qcvm_mutex);
	PR_SwitchQCVM (&sv.qcvm);

	if (*partial == '#')
	{
		for (i = 0; i < (int)VEC_SIZE (bbox_linked); i++)
		{
			const char *name;
			ed = bbox_linked[i];
			if (ed->free || !ed->v.classname)
				continue;
			name = PR_GetString (ed->v.classname);
			Con_AddToTabList (va ("#%d", NUM_FOR_EDICT (ed)), partial, name);
		}
	}
	else
	{
		for (i = 1, ed = NEXT_EDICT (qcvm->edicts); i < qcvm->num_edicts; i++, ed = NEXT_EDICT (ed))
		{
			const char *name;
			if (ed == sv_player || ed->free || !ed->v.classname)
				continue;
			name = PR_GetString (ed->v.classname);
			if (*name)
				Con_AddToTabList (name, partial, "#");
		}
	}

	PR_SwitchQCVM (NULL);
	SDL_UnlockMutex (draw_qcvm_mutex);
}

/*
====================
R_ShowbboxesFilterClear_f
====================
*/
static void R_ShowbboxesFilterClear_f (void)
{
	extern char	   *r_showbboxes_filter_strings;
	extern qboolean r_showbboxes_filter_byindex;

	SDL_LockMutex (draw_qcvm_mutex);

	SAFE_FREE (r_showbboxes_filter_strings);
	r_showbboxes_filter_byindex = false;

	SDL_UnlockMutex (draw_qcvm_mutex);
}

/*
====================
GL_Fullbrights_f -- johnfitz
====================
*/
static void GL_Fullbrights_f (cvar_t *var)
{
	TexMgr_ReloadNobrightImages ();
}

/*
====================
SetClearColor
====================
*/
static void SetClearColor ()
{
	byte *rgb;
	int	  s;

	if (r_fastclear.value != 0.0f)
	{
		// Set to black so fast clear works properly on modern GPUs
		vulkan_globals.color_clear_value.color.float32[0] = 0.0f;
		vulkan_globals.color_clear_value.color.float32[1] = 0.0f;
		vulkan_globals.color_clear_value.color.float32[2] = 0.0f;
		vulkan_globals.color_clear_value.color.float32[3] = 0.0f;
	}
	else
	{
		s = (int)r_clearcolor.value & 0xFF;
		rgb = (byte *)(d_8to24table + s);
		vulkan_globals.color_clear_value.color.float32[0] = rgb[0] / 255.0f;
		vulkan_globals.color_clear_value.color.float32[1] = rgb[1] / 255.0f;
		vulkan_globals.color_clear_value.color.float32[2] = rgb[2] / 255.0f;
		vulkan_globals.color_clear_value.color.float32[3] = 0.0f;
	}
}

/*
====================
R_SetClearColor_f -- johnfitz
====================
*/
static void R_SetClearColor_f (cvar_t *var)
{
	if (r_fastclear.value != 0.0f)
		Con_Warning ("Black clear color forced by r_fastclear\n");

	SetClearColor ();
}

/*
====================
R_SetFastClear_f -- johnfitz
====================
*/
static void R_SetFastClear_f (cvar_t *var)
{
	SetClearColor ();
}

/*
===============
R_Model_ExtraFlags_List_f -- johnfitz -- called when r_nolerp_list cvar changes
===============
*/
static void R_Model_ExtraFlags_List_f (cvar_t *var)
{
	int i;
	for (i = 0; i < MAX_MODELS; i++)
		Mod_SetExtraFlags (cl.model_precache[i]);
}

/*
====================
R_SetWateralpha_f -- ericw
====================
*/
static void R_SetWateralpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWWATER) && var->value < 1)
		Con_Warning ("Map does not appear to be water-vised\n");
	map_wateralpha = var->value;
	map_fallbackalpha = var->value;
}

#if defined(USE_SIMD)
/*
====================
R_SIMD_f
====================
*/
static void R_SIMD_f (cvar_t *var)
{
#if defined(USE_SSE2)
	use_simd = SDL_HasSSE () && SDL_HasSSE2 () && (var->value != 0.0f);
#elif defined(USE_NEON)
	// We only enable USE_NEON on AArch64 which always has support for it
	use_simd = var->value != 0.0f;
#else
#error not implemented
#endif
}
#endif

/*
====================
R_SetLavaalpha_f -- ericw
====================
*/
static void R_SetLavaalpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWLAVA) && var->value && var->value < 1)
		Con_Warning ("Map does not appear to be lava-vised\n");
	map_lavaalpha = var->value;
}

/*
====================
R_SetTelealpha_f -- ericw
====================
*/
static void R_SetTelealpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWTELE) && var->value && var->value < 1)
		Con_Warning ("Map does not appear to be tele-vised\n");
	map_telealpha = var->value;
}

/*
====================
R_SetSlimealpha_f -- ericw
====================
*/
static void R_SetSlimealpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWSLIME) && var->value && var->value < 1)
		Con_Warning ("Map does not appear to be slime-vised\n");
	map_slimealpha = var->value;
}

/*
====================
R_SetRTShadows_f
====================
*/
static void R_SetRTShadows_f (cvar_t *var)
{
	if (var->value > 0)
	{
		GL_BuildBModelAccelerationStructures ();
	}
	else
	{
		GL_DeleteBModelAccelerationStructures ();
		R_FreeAllEntityBLASes ();
		R_FreeASScratchBuffer ();
	}
	GL_UpdateLightmapDescriptorSets ();
}

/*
====================
GL_WaterAlphaForSurface -- ericw

Returns the map level alpha for the surface's liquid type. Only valid in
contexts where the entity alpha is known to be ENTALPHA_DEFAULT; entities
with an explicit alpha need GL_WaterAlphaForEntityTextureType instead.
====================
*/
float GL_WaterAlphaForSurface (msurface_t *fa)
{
	if (fa->flags & SURF_DRAWLAVA)
		return map_lavaalpha > 0 ? map_lavaalpha : map_fallbackalpha;
	else if (fa->flags & SURF_DRAWTELE)
		return map_telealpha > 0 ? map_telealpha : map_fallbackalpha;
	else if (fa->flags & SURF_DRAWSLIME)
		return map_slimealpha > 0 ? map_slimealpha : map_fallbackalpha;
	else
		return map_wateralpha;
}

float GL_WaterAlphaForTextureType (textype_t type)
{
	switch (type)
	{
	case TEXTYPE_LAVA:
		return map_lavaalpha > 0 ? map_lavaalpha : map_fallbackalpha;
	case TEXTYPE_SLIME:
		return map_slimealpha > 0 ? map_slimealpha : map_fallbackalpha;
	case TEXTYPE_TELE:
		return map_telealpha > 0 ? map_telealpha : map_fallbackalpha;
	case TEXTYPE_WATER:
		return map_wateralpha;
	default:
		return 1.0f;
	}
}

/*
===============
R_CreateStagingBuffers
===============
*/
static void R_CreateStagingBuffers ()
{
	int		 i;
	VkResult err;

	ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
	buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_create_info.size = vulkan_globals.staging_buffer_size;
	buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	for (i = 0; i < NUM_STAGING_BUFFERS; ++i)
	{
		staging_buffers[i].current_offset = 0;
		staging_buffers[i].submitted = false;

		err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, &staging_buffers[i].buffer);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateBuffer failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)staging_buffers[i].buffer, VK_OBJECT_TYPE_BUFFER, "Staging Buffer");
	}

	VkMemoryRequirements memory_requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, staging_buffers[0].buffer, &memory_requirements);
	const size_t aligned_size = q_align (memory_requirements.size, memory_requirements.alignment);

	ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.allocationSize = NUM_STAGING_BUFFERS * aligned_size;
	memory_allocate_info.memoryTypeIndex =
		GL_MemoryTypeFromProperties (memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

	R_AllocateVulkanMemory (&staging_memory, &memory_allocate_info, VULKAN_MEMORY_TYPE_HOST, &num_vulkan_misc_allocations);
	GL_SetObjectName ((uint64_t)staging_memory.handle, VK_OBJECT_TYPE_DEVICE_MEMORY, "Staging Buffers");

	for (i = 0; i < NUM_STAGING_BUFFERS; ++i)
	{
		err = vkBindBufferMemory (vulkan_globals.device, staging_buffers[i].buffer, staging_memory.handle, i * aligned_size);
		if (err != VK_SUCCESS)
			Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);
	}

	void *data;
	err = vkMapMemory (vulkan_globals.device, staging_memory.handle, 0, NUM_STAGING_BUFFERS * aligned_size, 0, &data);
	if (err != VK_SUCCESS)
		Sys_Error ("vkMapMemory failed with code %i", (int)err);

	for (i = 0; i < NUM_STAGING_BUFFERS; ++i)
		staging_buffers[i].data = (unsigned char *)data + (i * aligned_size);
}

/*
===============
R_DestroyStagingBuffers
===============
*/
static void R_DestroyStagingBuffers (void)
{
	int i;

	R_FreeVulkanMemory (&staging_memory, &num_vulkan_misc_allocations);
	for (i = 0; i < NUM_STAGING_BUFFERS; ++i)
	{
		vkDestroyBuffer (vulkan_globals.device, staging_buffers[i].buffer, NULL);
	}
}

/*
===============
R_InitStagingBuffers
===============
*/
void R_InitStagingBuffers (void)
{
	int		 i;
	VkResult err;

	Con_Printf ("Initializing staging\n");

	R_CreateStagingBuffers ();

	ZEROED_STRUCT (VkCommandPoolCreateInfo, command_pool_create_info);
	command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool_create_info.queueFamilyIndex = vulkan_globals.gfx_queue_family_index;

	err = vkCreateCommandPool (vulkan_globals.device, &command_pool_create_info, NULL, &staging_command_pool);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateCommandPool failed with code %i", (int)err);

	ZEROED_STRUCT (VkCommandBufferAllocateInfo, command_buffer_allocate_info);
	command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_buffer_allocate_info.commandPool = staging_command_pool;
	command_buffer_allocate_info.commandBufferCount = NUM_STAGING_BUFFERS;

	VkCommandBuffer command_buffers[NUM_STAGING_BUFFERS];
	err = vkAllocateCommandBuffers (vulkan_globals.device, &command_buffer_allocate_info, command_buffers);
	if (err != VK_SUCCESS)
		Sys_Error ("vkAllocateCommandBuffers failed with code %i", (int)err);

	ZEROED_STRUCT (VkFenceCreateInfo, fence_create_info);
	fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

	ZEROED_STRUCT (VkCommandBufferBeginInfo, command_buffer_begin_info);
	command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	for (i = 0; i < NUM_STAGING_BUFFERS; ++i)
	{
		err = vkCreateFence (vulkan_globals.device, &fence_create_info, NULL, &staging_buffers[i].fence);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateFence failed with code %i", (int)err);

		staging_buffers[i].command_buffer = command_buffers[i];

		err = vkBeginCommandBuffer (staging_buffers[i].command_buffer, &command_buffer_begin_info);
		if (err != VK_SUCCESS)
			Sys_Error ("vkBeginCommandBuffer failed with code %i", (int)err);
	}

	vertex_allocate_mutex = SDL_CreateMutex ();
	index_allocate_mutex = SDL_CreateMutex ();
	uniform_allocate_mutex = SDL_CreateMutex ();
	storage_allocate_mutex = SDL_CreateMutex ();
	garbage_mutex = SDL_CreateMutex ();
	staging_mutex = SDL_CreateMutex ();
	staging_cond = SDL_CreateCondition ();
}

/*
===============
R_SubmitStagingBuffer
===============
*/
static void R_SubmitStagingBuffer (int index)
{
	while (num_stagings_in_flight > 0)
		SDL_WaitCondition (staging_cond, staging_mutex);

	ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier (
		staging_buffers[index].command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

	vkEndCommandBuffer (staging_buffers[index].command_buffer);

	ZEROED_STRUCT (VkMappedMemoryRange, range);
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = staging_memory.handle;
	range.size = VK_WHOLE_SIZE;
	vkFlushMappedMemoryRanges (vulkan_globals.device, 1, &range);

	ZEROED_STRUCT (VkSubmitInfo, submit_info);
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &staging_buffers[index].command_buffer;

	SDL_LockMutex (vulkan_globals.queue_mutex);
	vkQueueSubmit (vulkan_globals.queue, 1, &submit_info, staging_buffers[index].fence);
	SDL_UnlockMutex (vulkan_globals.queue_mutex);

	staging_buffers[index].submitted = true;
	current_staging_buffer = (current_staging_buffer + 1) % NUM_STAGING_BUFFERS;
}

/*
===============
R_SubmitStagingBuffers
===============
*/
void R_SubmitStagingBuffers (void)
{
	SDL_LockMutex (staging_mutex);

	while (num_stagings_in_flight > 0)
		SDL_WaitCondition (staging_cond, staging_mutex);

	int i;
	for (i = 0; i < NUM_STAGING_BUFFERS; ++i)
	{
		if (!staging_buffers[i].submitted && staging_buffers[i].current_offset > 0)
			R_SubmitStagingBuffer (i);
	}

	SDL_UnlockMutex (staging_mutex);
}

/*
===============
R_FlushStagingCommandBuffer
===============
*/
static void R_FlushStagingCommandBuffer (stagingbuffer_t *staging_buffer)
{
	VkResult err;

	if (!staging_buffer->submitted)
		return;

	err = vkWaitForFences (vulkan_globals.device, 1, &staging_buffer->fence, VK_TRUE, UINT64_MAX);
	if (err != VK_SUCCESS)
		Sys_Error ("vkWaitForFences failed with code %i", (int)err);

	err = vkResetFences (vulkan_globals.device, 1, &staging_buffer->fence);
	if (err != VK_SUCCESS)
		Sys_Error ("vkResetFences failed with code %i", (int)err);

	staging_buffer->current_offset = 0;
	staging_buffer->submitted = false;

	ZEROED_STRUCT (VkCommandBufferBeginInfo, command_buffer_begin_info);
	command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	err = vkBeginCommandBuffer (staging_buffer->command_buffer, &command_buffer_begin_info);
	if (err != VK_SUCCESS)
		Sys_Error ("vkBeginCommandBuffer failed with code %i", (int)err);
}

/*
===============
R_StagingAllocate
===============
*/
byte *R_StagingAllocate (int size, int alignment, VkCommandBuffer *command_buffer, VkBuffer *buffer, int *buffer_offset)
{
	SDL_LockMutex (staging_mutex);

	while (num_stagings_in_flight > 0)
		SDL_WaitCondition (staging_cond, staging_mutex);

	vulkan_globals.device_idle = false;

	if (size > vulkan_globals.staging_buffer_size)
	{
		R_SubmitStagingBuffers ();

		for (int i = 0; i < NUM_STAGING_BUFFERS; ++i)
			R_FlushStagingCommandBuffer (&staging_buffers[i]);

		vulkan_globals.staging_buffer_size = size;

		R_DestroyStagingBuffers ();
		R_CreateStagingBuffers ();
	}

	stagingbuffer_t *staging_buffer = &staging_buffers[current_staging_buffer];
	assert (alignment == Q_nextPow2 (alignment));
	staging_buffer->current_offset = q_align (staging_buffer->current_offset, alignment);

	if ((staging_buffer->current_offset + size) >= vulkan_globals.staging_buffer_size && !staging_buffer->submitted)
		R_SubmitStagingBuffer (current_staging_buffer);

	staging_buffer = &staging_buffers[current_staging_buffer];
	R_FlushStagingCommandBuffer (staging_buffer);

	if (command_buffer)
		*command_buffer = staging_buffer->command_buffer;
	if (buffer)
		*buffer = staging_buffer->buffer;
	if (buffer_offset)
		*buffer_offset = staging_buffer->current_offset;

	unsigned char *data = staging_buffer->data + staging_buffer->current_offset;
	staging_buffer->current_offset += size;
	num_stagings_in_flight += 1;

	return data;
}

/*
===============
R_StagingBeginCopy
===============
*/
void R_StagingBeginCopy (void)
{
	SDL_UnlockMutex (staging_mutex);
}

/*
===============
R_StagingEndCopy
===============
*/
void R_StagingEndCopy (void)
{
	SDL_LockMutex (staging_mutex);
	num_stagings_in_flight -= 1;
	SDL_BroadcastCondition (staging_cond);
	SDL_UnlockMutex (staging_mutex);
}

/*
===============
R_StagingUploadBuffer
===============
*/
void R_StagingUploadBuffer (const VkBuffer buffer, const size_t size, const byte *data)
{
	size_t remaining_size = size;
	size_t copy_offset = 0;

	while (remaining_size > 0)
	{
		const int		size_to_copy = q_min (remaining_size, vulkan_globals.staging_buffer_size);
		VkBuffer		staging_buffer;
		VkCommandBuffer command_buffer;
		int				staging_offset;
		unsigned char  *staging_ptr = R_StagingAllocate (size_to_copy, 1, &command_buffer, &staging_buffer, &staging_offset);

		VkBufferCopy region;
		region.srcOffset = staging_offset;
		region.dstOffset = copy_offset;
		region.size = size_to_copy;
		vkCmdCopyBuffer (command_buffer, staging_buffer, buffer, 1, &region);

		R_StagingBeginCopy ();
		memcpy (staging_ptr, (byte *)data + copy_offset, size_to_copy);
		R_StagingEndCopy ();

		copy_offset += size_to_copy;
		remaining_size -= size_to_copy;
	}
}

/*
===============
R_InitDynamicBuffers
===============
*/
static void R_InitDynamicBuffers (
	dynbuffer_t *buffers, vulkan_memory_t *memory, uint32_t *current_size, VkBufferUsageFlags usage_flags, qboolean get_device_address, const char *name)
{
	int i;

	Sys_Printf ("Reallocating dynamic %ss (%u KB)\n", name, *current_size / 1024);

	VkResult err;

	if (get_device_address)
		usage_flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;

	ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
	buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_create_info.size = *current_size;
	buffer_create_info.usage = usage_flags;

	for (i = 0; i < NUM_DYNAMIC_BUFFERS; ++i)
	{
		buffers[i].current_offset = 0;

		err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, &buffers[i].buffer);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateBuffer failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)buffers[i].buffer, VK_OBJECT_TYPE_BUFFER, name);
	}

	VkMemoryRequirements memory_requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, buffers[0].buffer, &memory_requirements);

	const size_t aligned_size = q_align (memory_requirements.size, memory_requirements.alignment);

	ZEROED_STRUCT (VkMemoryAllocateFlagsInfo, memory_allocate_flags_info);
	if (get_device_address)
	{
		memory_allocate_flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR;
		memory_allocate_flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	}

	ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.pNext = get_device_address ? &memory_allocate_flags_info : NULL;
	memory_allocate_info.allocationSize = NUM_DYNAMIC_BUFFERS * aligned_size;
	memory_allocate_info.memoryTypeIndex =
		GL_MemoryTypeFromProperties (memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

	R_AllocateVulkanMemory (memory, &memory_allocate_info, VULKAN_MEMORY_TYPE_HOST, &num_vulkan_dynbuf_allocations);
	GL_SetObjectName ((uint64_t)memory->handle, VK_OBJECT_TYPE_DEVICE_MEMORY, name);

	for (i = 0; i < NUM_DYNAMIC_BUFFERS; ++i)
	{
		err = vkBindBufferMemory (vulkan_globals.device, buffers[i].buffer, memory->handle, i * aligned_size);
		if (err != VK_SUCCESS)
			Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);
	}

	void *data;
	err = vkMapMemory (vulkan_globals.device, memory->handle, 0, NUM_DYNAMIC_BUFFERS * aligned_size, 0, &data);
	if (err != VK_SUCCESS)
		Sys_Error ("vkMapMemory failed with code %i", (int)err);

	for (i = 0; i < NUM_DYNAMIC_BUFFERS; ++i)
	{
		buffers[i].data = (unsigned char *)data + (i * aligned_size);

		if (get_device_address)
		{
			VkBufferDeviceAddressInfoKHR buffer_device_address_info;
			memset (&buffer_device_address_info, 0, sizeof (buffer_device_address_info));
			buffer_device_address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR;
			buffer_device_address_info.buffer = buffers[i].buffer;
			VkDeviceAddress device_address = vulkan_globals.vk_get_buffer_device_address (vulkan_globals.device, &buffer_device_address_info);
			buffers[i].device_address = device_address;
		}
	}
}

/*
===============
R_InitDynamicVertexBuffers
===============
*/
static void R_InitDynamicVertexBuffers (void)
{
	R_InitDynamicBuffers (
		dyn_vertex_buffers, &dyn_vertex_buffer_memory, &current_dyn_vertex_buffer_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, "vertex buffer");
}

/*
===============
R_InitDynamicIndexBuffers
===============
*/
static void R_InitDynamicIndexBuffers (void)
{
	R_InitDynamicBuffers (dyn_index_buffers, &dyn_index_buffer_memory, &current_dyn_index_buffer_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false, "index buffer");
}

/*
===============
R_InitDynamicUniformBuffers
===============
*/
static void R_InitDynamicUniformBuffers (void)
{
	R_InitDynamicBuffers (
		dyn_uniform_buffers, &dyn_uniform_buffer_memory, &current_dyn_uniform_buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false, "uniform buffer");

	ZEROED_STRUCT (VkDescriptorSetAllocateInfo, descriptor_set_allocate_info);
	descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptor_set_allocate_info.descriptorPool = vulkan_globals.descriptor_pool;
	descriptor_set_allocate_info.descriptorSetCount = 1;
	descriptor_set_allocate_info.pSetLayouts = &vulkan_globals.ubo_set_layout.handle;

	ubo_descriptor_sets[0] = R_AllocateDescriptorSet (&vulkan_globals.ubo_set_layout);
	ubo_descriptor_sets[1] = R_AllocateDescriptorSet (&vulkan_globals.ubo_set_layout);

	ZEROED_STRUCT (VkDescriptorBufferInfo, buffer_info);
	buffer_info.offset = 0;
	buffer_info.range = MAX_UNIFORM_ALLOC;

	ZEROED_STRUCT (VkWriteDescriptorSet, ubo_write);
	ubo_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	ubo_write.dstBinding = 0;
	ubo_write.dstArrayElement = 0;
	ubo_write.descriptorCount = 1;
	ubo_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	ubo_write.pBufferInfo = &buffer_info;

	for (int i = 0; i < NUM_DYNAMIC_BUFFERS; ++i)
	{
		buffer_info.buffer = dyn_uniform_buffers[i].buffer;
		ubo_write.dstSet = ubo_descriptor_sets[i];
		vkUpdateDescriptorSets (vulkan_globals.device, 1, &ubo_write, 0, NULL);
	}
}

/*
===============
R_InitDynamicStorageBuffers
===============
*/
static void R_InitDynamicStorageBuffers (void)
{
	VkBufferUsageFlags usage_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	qboolean		   get_device_address = false;
	if (vulkan_globals.ray_query)
	{
		usage_flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
		get_device_address = true;
	}

	R_InitDynamicBuffers (dyn_storage_buffers, &dyn_storage_buffer_memory, &current_dyn_storage_buffer_size, usage_flags, get_device_address, "storage buffer");
}

/*
===============
R_InitFanIndexBuffer
===============
*/
static void R_InitFanIndexBuffer ()
{
	VkResult	   err;
	VkDeviceMemory memory;
	const int	   bufferSize = sizeof (uint16_t) * FAN_INDEX_BUFFER_SIZE;

	ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
	buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_create_info.size = bufferSize;
	buffer_create_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, &vulkan_globals.fan_index_buffer);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateBuffer failed with code %i", (int)err);

	GL_SetObjectName ((uint64_t)vulkan_globals.fan_index_buffer, VK_OBJECT_TYPE_BUFFER, "Quad Index Buffer");

	VkMemoryRequirements memory_requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, vulkan_globals.fan_index_buffer, &memory_requirements);

	ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.allocationSize = memory_requirements.size;
	memory_allocate_info.memoryTypeIndex = GL_MemoryTypeFromProperties (memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

	Atomic_IncrementUInt32 (&num_vulkan_dynbuf_allocations);
	Atomic_AddUInt64 (&total_device_vulkan_allocation_size, memory_requirements.size);
	err = vkAllocateMemory (vulkan_globals.device, &memory_allocate_info, NULL, &memory);
	if (err != VK_SUCCESS)
		Sys_Error ("vkAllocateMemory failed with code %i", (int)err);

	err = vkBindBufferMemory (vulkan_globals.device, vulkan_globals.fan_index_buffer, memory, 0);
	if (err != VK_SUCCESS)
		Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);

	{
		VkBuffer		staging_buffer;
		VkCommandBuffer command_buffer;
		int				staging_offset;
		int				current_index = 0;
		int				i;
		uint16_t	   *staging_mem = (uint16_t *)R_StagingAllocate (bufferSize, 1, &command_buffer, &staging_buffer, &staging_offset);

		VkBufferCopy region;
		region.srcOffset = staging_offset;
		region.dstOffset = 0;
		region.size = bufferSize;
		vkCmdCopyBuffer (command_buffer, staging_buffer, vulkan_globals.fan_index_buffer, 1, &region);

		R_StagingBeginCopy ();
		for (i = 0; i < FAN_INDEX_BUFFER_SIZE / 3; ++i)
		{
			staging_mem[current_index++] = 0;
			staging_mem[current_index++] = 1 + i;
			staging_mem[current_index++] = 2 + i;
		}
		R_StagingEndCopy ();
	}
}

/*
===============
R_SwapDynamicBuffers
===============
*/
void R_SwapDynamicBuffers (void)
{
	// Switch to the buffers for this frame and start writing from the beginning.
	// BeginRenderingTask has made sure the GPU is done with their old contents.
	current_dyn_buffer_index = (current_dyn_buffer_index + 1) % NUM_DYNAMIC_BUFFERS;
	dyn_vertex_buffers[current_dyn_buffer_index].current_offset = 0;
	dyn_index_buffers[current_dyn_buffer_index].current_offset = 0;
	dyn_uniform_buffers[current_dyn_buffer_index].current_offset = 0;
	dyn_storage_buffers[current_dyn_buffer_index].current_offset = 0;
}

/*
===============
R_FlushDynamicBuffers
===============
*/
void R_FlushDynamicBuffers (void)
{
	ZEROED_STRUCT_ARRAY (VkMappedMemoryRange, ranges, 5);
	int num_ranges = 0;
	ranges[num_ranges].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	ranges[num_ranges].memory = dyn_vertex_buffer_memory.handle;
	ranges[num_ranges++].size = VK_WHOLE_SIZE;
	ranges[num_ranges].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	ranges[num_ranges].memory = dyn_index_buffer_memory.handle;
	ranges[num_ranges++].size = VK_WHOLE_SIZE;
	ranges[num_ranges].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	ranges[num_ranges].memory = dyn_uniform_buffer_memory.handle;
	ranges[num_ranges++].size = VK_WHOLE_SIZE;
	ranges[num_ranges].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	ranges[num_ranges].memory = frame_upload_buffers_memory.handle;
	ranges[num_ranges++].size = VK_WHOLE_SIZE;
	if (dyn_storage_buffer_memory.handle != VK_NULL_HANDLE)
	{
		ranges[num_ranges].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		ranges[num_ranges].memory = dyn_storage_buffer_memory.handle;
		ranges[num_ranges++].size = VK_WHOLE_SIZE;
	}
	vkFlushMappedMemoryRanges (vulkan_globals.device, num_ranges, ranges);
}

/*
===============
R_AddDynamicBufferGarbage
===============
*/
void R_AddDynamicBufferGarbage (vulkan_memory_t memory, dynbuffer_t *buffers, int num_buffers, VkDescriptorSet *descriptor_sets)
{
	SDL_LockMutex (garbage_mutex);

	{
		int *num_garbage = &num_device_memory_garbage[current_garbage_index];
		int	 old_num_memory_garbage = *num_garbage;
		*num_garbage += 1;
		if (device_memory_garbage[current_garbage_index] == NULL)
			device_memory_garbage[current_garbage_index] = Mem_Alloc (sizeof (vulkan_memory_t) * (*num_garbage));
		else
			device_memory_garbage[current_garbage_index] =
				Mem_Realloc (device_memory_garbage[current_garbage_index], sizeof (vulkan_memory_t) * (*num_garbage));
		device_memory_garbage[current_garbage_index][old_num_memory_garbage] = memory;
	}

	{
		int *num_garbage = &num_buffer_garbage[current_garbage_index];
		int	 old_num_buffer_garbage = *num_garbage;
		*num_garbage += num_buffers;
		if (buffer_garbage[current_garbage_index] == NULL)
			buffer_garbage[current_garbage_index] = Mem_Alloc (sizeof (VkBuffer) * (*num_garbage));
		else
			buffer_garbage[current_garbage_index] = Mem_Realloc (buffer_garbage[current_garbage_index], sizeof (VkBuffer) * (*num_garbage));
		for (int i = 0; i < num_buffers; ++i)
			buffer_garbage[current_garbage_index][old_num_buffer_garbage + i] = buffers[i].buffer;
	}

	if (descriptor_sets)
	{
		int *num_garbage = &num_desc_set_garbage[current_garbage_index];
		int	 old_num_desc_set_garbage = *num_garbage;
		*num_garbage += 2;
		if (descriptor_set_garbage[current_garbage_index] == NULL)
			descriptor_set_garbage[current_garbage_index] = Mem_Alloc (sizeof (VkDescriptorSet) * (*num_garbage));
		else
			descriptor_set_garbage[current_garbage_index] =
				Mem_Realloc (descriptor_set_garbage[current_garbage_index], sizeof (VkDescriptorSet) * (*num_garbage));
		for (int i = 0; i < 2; ++i)
			descriptor_set_garbage[current_garbage_index][old_num_desc_set_garbage + i] = descriptor_sets[i];
	}

	SDL_UnlockMutex (garbage_mutex);
}

/*
===============
R_CollectDynamicBufferGarbage
===============
*/
void R_CollectDynamicBufferGarbage (void)
{
	current_garbage_index = (current_garbage_index + 1) % GARBAGE_FRAME_COUNT;
	const int collect_garbage_index = (current_garbage_index + 1) % GARBAGE_FRAME_COUNT;

	if (num_desc_set_garbage[collect_garbage_index] > 0)
	{
		for (int i = 0; i < num_desc_set_garbage[collect_garbage_index]; ++i)
			R_FreeDescriptorSet (descriptor_set_garbage[collect_garbage_index][i], &vulkan_globals.ubo_set_layout);
		Mem_Free (descriptor_set_garbage[collect_garbage_index]);
		descriptor_set_garbage[collect_garbage_index] = NULL;
		num_desc_set_garbage[collect_garbage_index] = 0;
	}

	if (num_buffer_garbage[collect_garbage_index] > 0)
	{
		for (int i = 0; i < num_buffer_garbage[collect_garbage_index]; ++i)
			vkDestroyBuffer (vulkan_globals.device, buffer_garbage[collect_garbage_index][i], NULL);
		Mem_Free (buffer_garbage[collect_garbage_index]);
		buffer_garbage[collect_garbage_index] = NULL;
		num_buffer_garbage[collect_garbage_index] = 0;
	}

	if (num_device_memory_garbage[collect_garbage_index] > 0)
	{
		for (int i = 0; i < num_device_memory_garbage[collect_garbage_index]; ++i)
			R_FreeVulkanMemory (&device_memory_garbage[collect_garbage_index][i], &num_vulkan_dynbuf_allocations);
		Mem_Free (device_memory_garbage[collect_garbage_index]);
		device_memory_garbage[collect_garbage_index] = NULL;
		num_device_memory_garbage[collect_garbage_index] = 0;
	}
}

/*
===============
R_DynBufferAllocate
===============
*/
byte *R_DynBufferAllocate (
	int size, int alignment, int min_tail_size, VkBuffer *buffer, VkDeviceSize *buffer_offset, VkDeviceAddress *device_address, SDL_Mutex **mutex,
	dynbuffer_t *dyn_buffers, vulkan_memory_t *memory, uint32_t *current_size, VkDescriptorSet *descriptor_set, VkDescriptorSet *descriptor_sets,
	void (*init_func) (void))
{
	SDL_LockMutex (*mutex);
	dynbuffer_t *dyn_buffer = &dyn_buffers[current_dyn_buffer_index];
	const int	 aligned_size = q_align (size, alignment);

	if ((dyn_buffer->current_offset + q_max (size, min_tail_size)) > *current_size)
	{
		R_AddDynamicBufferGarbage (*memory, dyn_buffers, NUM_DYNAMIC_BUFFERS, descriptor_sets);
		*current_size = q_max (*current_size * 2, (uint32_t)Q_nextPow2 (size));
		init_func ();
	}

	if (buffer)
		*buffer = dyn_buffer->buffer;
	if (buffer_offset)
		*buffer_offset = dyn_buffer->current_offset;
	if (device_address)
		*device_address = dyn_buffer->device_address + dyn_buffer->current_offset;

	unsigned char *data = dyn_buffer->data + dyn_buffer->current_offset;
	dyn_buffer->current_offset += aligned_size;

	if (descriptor_set)
		*descriptor_set = descriptor_sets[current_dyn_buffer_index];

	SDL_UnlockMutex (*mutex);
	return data;
}

/*
===============
R_VertexAllocate
Vertex buffers need to be aligned by the maximum format component size, i.e. sizeof(float)
===============
*/
byte *R_VertexAllocate (int size, VkBuffer *buffer, VkDeviceSize *buffer_offset)
{
	return R_DynBufferAllocate (
		size, sizeof (float), 0, buffer, buffer_offset, NULL, &vertex_allocate_mutex, dyn_vertex_buffers, &dyn_vertex_buffer_memory,
		&current_dyn_vertex_buffer_size, NULL, NULL, &R_InitDynamicVertexBuffers);
}

/*
===============
R_IndexAllocate
Align to 4 bytes because we allocate both uint16 and uint32
index buffers and alignment must match index size
===============
*/
byte *R_IndexAllocate (int size, VkBuffer *buffer, VkDeviceSize *buffer_offset)
{
	return R_DynBufferAllocate (
		size, sizeof (uint32_t), 0, buffer, buffer_offset, NULL, &index_allocate_mutex, dyn_index_buffers, &dyn_index_buffer_memory,
		&current_dyn_index_buffer_size, NULL, NULL, &R_InitDynamicIndexBuffers);
}

/*
===============
R_StorageAllocate
===============
*/
byte *R_StorageAllocate (int size, VkBuffer *buffer, VkDeviceSize *buffer_offset, VkDeviceAddress *device_address)
{
	return R_DynBufferAllocate (
		size, vulkan_globals.device_properties.limits.minStorageBufferOffsetAlignment, 0, buffer, buffer_offset, device_address, &storage_allocate_mutex,
		dyn_storage_buffers, &dyn_storage_buffer_memory, &current_dyn_storage_buffer_size, NULL, NULL, &R_InitDynamicStorageBuffers);
}

/*
===============
R_UniformAllocate
===============
*/
byte *R_UniformAllocate (int size, VkBuffer *buffer, uint32_t *buffer_offset, VkDescriptorSet *descriptor_set)
{
	if (size > MAX_UNIFORM_ALLOC)
		Sys_Error ("Increase MAX_UNIFORM_ALLOC");

	VkDeviceSize device_size_offset = 0;

	byte *data = R_DynBufferAllocate (
		size, vulkan_globals.device_properties.limits.minUniformBufferOffsetAlignment, MAX_UNIFORM_ALLOC, buffer, &device_size_offset, NULL,
		&uniform_allocate_mutex, dyn_uniform_buffers, &dyn_uniform_buffer_memory, &current_dyn_uniform_buffer_size, descriptor_set, ubo_descriptor_sets,
		&R_InitDynamicUniformBuffers);

	*buffer_offset = device_size_offset;
	return data;
}

/*
===============
R_InitGPUBuffers
===============
*/
void R_InitGPUBuffers (void)
{
	R_InitDynamicVertexBuffers ();
	R_InitDynamicIndexBuffers ();
	R_InitDynamicUniformBuffers ();
	R_InitFanIndexBuffer ();
}

/*
===============
R_CreateDescriptorSetLayouts
===============
*/
void R_CreateDescriptorSetLayouts ()
{
	Sys_Printf ("Creating descriptor set layouts\n");

	VkResult err;
	ZEROED_STRUCT (VkDescriptorSetLayoutCreateInfo, descriptor_set_layout_create_info);
	descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

	{
		ZEROED_STRUCT (VkDescriptorSetLayoutBinding, single_texture_layout_binding);
		single_texture_layout_binding.binding = 0;
		single_texture_layout_binding.descriptorCount = 1;
		single_texture_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		single_texture_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

		descriptor_set_layout_create_info.bindingCount = 1;
		descriptor_set_layout_create_info.pBindings = &single_texture_layout_binding;

		memset (&vulkan_globals.single_texture_set_layout, 0, sizeof (vulkan_globals.single_texture_set_layout));
		vulkan_globals.single_texture_set_layout.num_combined_image_samplers = 1;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.single_texture_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.single_texture_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "single texture");
	}

	{
		ZEROED_STRUCT (VkDescriptorSetLayoutBinding, gui_sampler_layout_binding);
		gui_sampler_layout_binding.binding = 0;
		gui_sampler_layout_binding.descriptorCount = 1;
		gui_sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		gui_sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		descriptor_set_layout_create_info.bindingCount = 1;
		descriptor_set_layout_create_info.pBindings = &gui_sampler_layout_binding;

		memset (&vulkan_globals.gui_sampler_set_layout, 0, sizeof (vulkan_globals.gui_sampler_set_layout));
		vulkan_globals.gui_sampler_set_layout.num_samplers = 1;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.gui_sampler_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.gui_sampler_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "GUI sampler");
	}

	{
		ZEROED_STRUCT (VkDescriptorSetLayoutBinding, ubo_layout_bindings);
		ubo_layout_bindings.binding = 0;
		ubo_layout_bindings.descriptorCount = 1;
		ubo_layout_bindings.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		ubo_layout_bindings.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		descriptor_set_layout_create_info.bindingCount = 1;
		descriptor_set_layout_create_info.pBindings = &ubo_layout_bindings;

		memset (&vulkan_globals.ubo_set_layout, 0, sizeof (vulkan_globals.ubo_set_layout));
		vulkan_globals.ubo_set_layout.num_ubos_dynamic = 1;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.ubo_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.ubo_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "single dynamic UBO");
	}

	{
		ZEROED_STRUCT (VkDescriptorSetLayoutBinding, joints_buffer_layout_bindings);
		joints_buffer_layout_bindings.binding = 0;
		joints_buffer_layout_bindings.descriptorCount = 1;
		joints_buffer_layout_bindings.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		joints_buffer_layout_bindings.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		descriptor_set_layout_create_info.bindingCount = 1;
		descriptor_set_layout_create_info.pBindings = &joints_buffer_layout_bindings;

		memset (&vulkan_globals.joints_buffer_set_layout, 0, sizeof (vulkan_globals.joints_buffer_set_layout));
		vulkan_globals.joints_buffer_set_layout.num_storage_buffers = 1;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.joints_buffer_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.joints_buffer_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "joints buffer");
	}

	{
		ZEROED_STRUCT (VkDescriptorSetLayoutBinding, input_attachment_layout_bindings);
		input_attachment_layout_bindings.binding = 0;
		input_attachment_layout_bindings.descriptorCount = 1;
		input_attachment_layout_bindings.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		input_attachment_layout_bindings.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		descriptor_set_layout_create_info.bindingCount = 1;
		descriptor_set_layout_create_info.pBindings = &input_attachment_layout_bindings;

		memset (&vulkan_globals.input_attachment_set_layout, 0, sizeof (vulkan_globals.input_attachment_set_layout));
		vulkan_globals.input_attachment_set_layout.num_input_attachments = 1;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.input_attachment_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.input_attachment_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "input attachment");
	}

	{
		ZEROED_STRUCT_ARRAY (VkDescriptorSetLayoutBinding, oit_input_attachment_layout_bindings, 2);
		for (int i = 0; i < 2; ++i)
		{
			oit_input_attachment_layout_bindings[i].binding = i;
			oit_input_attachment_layout_bindings[i].descriptorCount = 1;
			oit_input_attachment_layout_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			oit_input_attachment_layout_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		}

		descriptor_set_layout_create_info.bindingCount = countof (oit_input_attachment_layout_bindings);
		descriptor_set_layout_create_info.pBindings = oit_input_attachment_layout_bindings;

		memset (&vulkan_globals.oit_input_attachment_set_layout, 0, sizeof (vulkan_globals.oit_input_attachment_set_layout));
		vulkan_globals.oit_input_attachment_set_layout.num_input_attachments = 2;

		err = vkCreateDescriptorSetLayout (
			vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.oit_input_attachment_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.oit_input_attachment_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "oit input attachment");
	}

	{
		ZEROED_STRUCT_ARRAY (VkDescriptorSetLayoutBinding, mboit_input_attachment_layout_bindings, 3);
		for (int i = 0; i < 3; ++i)
		{
			mboit_input_attachment_layout_bindings[i].binding = i;
			mboit_input_attachment_layout_bindings[i].descriptorCount = 1;
			mboit_input_attachment_layout_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			mboit_input_attachment_layout_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		}

		descriptor_set_layout_create_info.bindingCount = countof (mboit_input_attachment_layout_bindings);
		descriptor_set_layout_create_info.pBindings = mboit_input_attachment_layout_bindings;

		memset (&vulkan_globals.mboit_input_attachment_set_layout, 0, sizeof (vulkan_globals.mboit_input_attachment_set_layout));
		vulkan_globals.mboit_input_attachment_set_layout.num_input_attachments = 3;

		err = vkCreateDescriptorSetLayout (
			vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.mboit_input_attachment_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.mboit_input_attachment_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "mboit input attachment");
	}

	{
		ZEROED_STRUCT_ARRAY (VkDescriptorSetLayoutBinding, screen_effects_layout_bindings, 5);
		screen_effects_layout_bindings[0].binding = 0;
		screen_effects_layout_bindings[0].descriptorCount = 1;
		screen_effects_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		screen_effects_layout_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		screen_effects_layout_bindings[1].binding = 1;
		screen_effects_layout_bindings[1].descriptorCount = 1;
		screen_effects_layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		screen_effects_layout_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		screen_effects_layout_bindings[2].binding = 2;
		screen_effects_layout_bindings[2].descriptorCount = 1;
		screen_effects_layout_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		screen_effects_layout_bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		screen_effects_layout_bindings[3].binding = 3;
		screen_effects_layout_bindings[3].descriptorCount = 1;
		screen_effects_layout_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		screen_effects_layout_bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		screen_effects_layout_bindings[4].binding = 4;
		screen_effects_layout_bindings[4].descriptorCount = 1;
		screen_effects_layout_bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		screen_effects_layout_bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		descriptor_set_layout_create_info.bindingCount = countof (screen_effects_layout_bindings);
		descriptor_set_layout_create_info.pBindings = screen_effects_layout_bindings;

		memset (&vulkan_globals.screen_effects_set_layout, 0, sizeof (vulkan_globals.screen_effects_set_layout));
		vulkan_globals.screen_effects_set_layout.num_combined_image_samplers = 2;
		vulkan_globals.screen_effects_set_layout.num_storage_images = 1;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.screen_effects_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.screen_effects_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "screen effects");
	}

	{
		ZEROED_STRUCT (VkDescriptorSetLayoutBinding, single_texture_cs_write_layout_binding);
		single_texture_cs_write_layout_binding.binding = 0;
		single_texture_cs_write_layout_binding.descriptorCount = 1;
		single_texture_cs_write_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		single_texture_cs_write_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		descriptor_set_layout_create_info.bindingCount = 1;
		descriptor_set_layout_create_info.pBindings = &single_texture_cs_write_layout_binding;

		memset (&vulkan_globals.single_texture_cs_write_set_layout, 0, sizeof (vulkan_globals.single_texture_cs_write_set_layout));
		vulkan_globals.single_texture_cs_write_set_layout.num_storage_images = 1;

		err = vkCreateDescriptorSetLayout (
			vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.single_texture_cs_write_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.single_texture_cs_write_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "single storage image");
	}

	{
		int num_descriptors = 0;
		ZEROED_STRUCT_ARRAY (VkDescriptorSetLayoutBinding, lightmap_compute_layout_bindings, 10);
		lightmap_compute_layout_bindings[0].binding = num_descriptors++;
		lightmap_compute_layout_bindings[0].descriptorCount = 1;
		lightmap_compute_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		lightmap_compute_layout_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[1].binding = num_descriptors++;
		lightmap_compute_layout_bindings[1].descriptorCount = 1;
		lightmap_compute_layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		lightmap_compute_layout_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[2].binding = num_descriptors++;
		lightmap_compute_layout_bindings[2].descriptorCount = MAXLIGHTMAPS * 3 / 4;
		lightmap_compute_layout_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		lightmap_compute_layout_bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[3].binding = num_descriptors++;
		lightmap_compute_layout_bindings[3].descriptorCount = 1;
		lightmap_compute_layout_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		lightmap_compute_layout_bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[4].binding = num_descriptors++;
		lightmap_compute_layout_bindings[4].descriptorCount = 1;
		lightmap_compute_layout_bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		lightmap_compute_layout_bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[5].binding = num_descriptors++;
		lightmap_compute_layout_bindings[5].descriptorCount = 1;
		lightmap_compute_layout_bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		lightmap_compute_layout_bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[6].binding = num_descriptors++;
		lightmap_compute_layout_bindings[6].descriptorCount = 1;
		lightmap_compute_layout_bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		lightmap_compute_layout_bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[7].binding = num_descriptors++;
		lightmap_compute_layout_bindings[7].descriptorCount = 1;
		lightmap_compute_layout_bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		lightmap_compute_layout_bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[8].binding = num_descriptors++;
		lightmap_compute_layout_bindings[8].descriptorCount = 1;
		lightmap_compute_layout_bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		lightmap_compute_layout_bindings[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		lightmap_compute_layout_bindings[9].binding = num_descriptors++;
		lightmap_compute_layout_bindings[9].descriptorCount = 1;
		lightmap_compute_layout_bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		lightmap_compute_layout_bindings[9].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		descriptor_set_layout_create_info.bindingCount = num_descriptors;
		descriptor_set_layout_create_info.pBindings = lightmap_compute_layout_bindings;

		memset (&vulkan_globals.lightmap_compute_set_layout, 0, sizeof (vulkan_globals.lightmap_compute_set_layout));
		vulkan_globals.lightmap_compute_set_layout.num_storage_images = 1;
		vulkan_globals.lightmap_compute_set_layout.num_sampled_images = 1 + MAXLIGHTMAPS * 3 / 4;
		vulkan_globals.lightmap_compute_set_layout.num_storage_buffers = 5;
		vulkan_globals.lightmap_compute_set_layout.num_ubos_dynamic = 2;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.lightmap_compute_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.lightmap_compute_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "lightmap compute");
	}

	{
		ZEROED_STRUCT_ARRAY (VkDescriptorSetLayoutBinding, indirect_compute_layout_bindings, 6);
		indirect_compute_layout_bindings[0].binding = 0;
		indirect_compute_layout_bindings[0].descriptorCount = 1;
		indirect_compute_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		indirect_compute_layout_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		indirect_compute_layout_bindings[1].binding = 1;
		indirect_compute_layout_bindings[1].descriptorCount = 1;
		indirect_compute_layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		indirect_compute_layout_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		indirect_compute_layout_bindings[2].binding = 2;
		indirect_compute_layout_bindings[2].descriptorCount = 1;
		indirect_compute_layout_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		indirect_compute_layout_bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		indirect_compute_layout_bindings[3].binding = 3;
		indirect_compute_layout_bindings[3].descriptorCount = 1;
		indirect_compute_layout_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		indirect_compute_layout_bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		indirect_compute_layout_bindings[4].binding = 4;
		indirect_compute_layout_bindings[4].descriptorCount = 1;
		indirect_compute_layout_bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		indirect_compute_layout_bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		indirect_compute_layout_bindings[5].binding = 5;
		indirect_compute_layout_bindings[5].descriptorCount = 1;
		indirect_compute_layout_bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		indirect_compute_layout_bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		descriptor_set_layout_create_info.bindingCount = countof (indirect_compute_layout_bindings);
		descriptor_set_layout_create_info.pBindings = indirect_compute_layout_bindings;

		memset (&vulkan_globals.indirect_compute_set_layout, 0, sizeof (vulkan_globals.indirect_compute_set_layout));
		vulkan_globals.indirect_compute_set_layout.num_storage_buffers = 6;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.indirect_compute_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.indirect_compute_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "indirect compute");
	}

	{
		VkDescriptorSetLayoutBinding bindings[3] = {0};
		for (uint32_t i = 0; i < countof (bindings); ++i)
		{
			bindings[i].binding = i;
			bindings[i].descriptorCount = 1;
			bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		}
		VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = countof (bindings), .pBindings = bindings};
		vulkan_globals.ecs_cull_set_layout.num_storage_buffers = countof (bindings);
		if (vkCreateDescriptorSetLayout (vulkan_globals.device, &info, NULL, &vulkan_globals.ecs_cull_set_layout.handle) != VK_SUCCESS)
			vulkan_globals.ecs_cull_set_layout.handle = VK_NULL_HANDLE;
	}

	{
		ZEROED_STRUCT_ARRAY (VkDescriptorSetLayoutBinding, bmodel_instances_layout_bindings, 2);
		bmodel_instances_layout_bindings[0].binding = 0;
		bmodel_instances_layout_bindings[0].descriptorCount = 1;
		bmodel_instances_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bmodel_instances_layout_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		bmodel_instances_layout_bindings[1].binding = 1;
		bmodel_instances_layout_bindings[1].descriptorCount = 1;
		bmodel_instances_layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bmodel_instances_layout_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

		descriptor_set_layout_create_info.bindingCount = countof (bmodel_instances_layout_bindings);
		descriptor_set_layout_create_info.pBindings = bmodel_instances_layout_bindings;

		memset (&vulkan_globals.bmodel_instances_set_layout, 0, sizeof (vulkan_globals.bmodel_instances_set_layout));
		vulkan_globals.bmodel_instances_set_layout.num_storage_buffers = 2;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.bmodel_instances_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.bmodel_instances_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "bmodel instances");
	}

#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
	if (vulkan_globals.mesh_shader)
	{
		VkDescriptorSetLayoutBinding bindings[4];
		for (uint32_t i = 0; i < countof (bindings); ++i)
			bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
				VK_SHADER_STAGE_MESH_BIT_EXT | (vulkan_globals.task_shader ? VK_SHADER_STAGE_TASK_BIT_EXT : 0), NULL};
		VkDescriptorSetLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = countof (bindings), .pBindings = bindings};
		vulkan_globals.world_mesh_set_layout.num_storage_buffers = countof (bindings);
		if (vkCreateDescriptorSetLayout (vulkan_globals.device, &info, NULL, &vulkan_globals.world_mesh_set_layout.handle) != VK_SUCCESS)
			vulkan_globals.world_mesh_set_layout.handle = VK_NULL_HANDLE;
	}
#endif

	if (vulkan_globals.ray_query)
	{
		ZEROED_STRUCT (VkDescriptorSetLayoutBinding, ray_query_push_layout_binding);
		ray_query_push_layout_binding.binding = 0;
		ray_query_push_layout_binding.descriptorCount = 1;
		ray_query_push_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		ray_query_push_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		descriptor_set_layout_create_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		descriptor_set_layout_create_info.bindingCount = 1;
		descriptor_set_layout_create_info.pBindings = &ray_query_push_layout_binding;

		memset (&vulkan_globals.ray_query_push_set_layout, 0, sizeof (vulkan_globals.ray_query_push_set_layout));

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.ray_query_push_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.ray_query_push_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "ray query push");

		descriptor_set_layout_create_info.flags = 0;
	}

#if defined(_DEBUG)
	if (vulkan_globals.ray_query)
	{
		ZEROED_STRUCT_ARRAY (VkDescriptorSetLayoutBinding, ray_debug_layout_bindings, 1);
		ray_debug_layout_bindings[0].binding = 0;
		ray_debug_layout_bindings[0].descriptorCount = 1;
		ray_debug_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		ray_debug_layout_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		descriptor_set_layout_create_info.bindingCount = countof (ray_debug_layout_bindings);
		descriptor_set_layout_create_info.pBindings = ray_debug_layout_bindings;

		memset (&vulkan_globals.ray_debug_set_layout, 0, sizeof (vulkan_globals.ray_debug_set_layout));
		vulkan_globals.ray_debug_set_layout.num_storage_images = 1;

		err = vkCreateDescriptorSetLayout (vulkan_globals.device, &descriptor_set_layout_create_info, NULL, &vulkan_globals.ray_debug_set_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateDescriptorSetLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.ray_debug_set_layout.handle, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "ray debug");
	}
#endif
}

/*
===============
R_CreateDescriptorPool
===============
*/
void R_CreateDescriptorPool ()
{
	/* Scene-descriptor updates create a replacement set before freeing the old
	 * one. Reserve space for its maximum temporary footprint (upscale,
	 * postprocess, one OIT mode, screen effects and debug ray output). */
	enum { SCENE_REPLACEMENT_SETS = 5, SCENE_REPLACEMENT_COMBINED = 3,
		SCENE_REPLACEMENT_STORAGE = 2, SCENE_REPLACEMENT_INPUT = 4 };
	ZEROED_STRUCT_ARRAY (VkDescriptorPoolSize, pool_sizes, 9);
	pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_sizes[0].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE + (MAX_SANITY_LIGHTMAPS * 2) + (MAX_GLTEXTURES + 1) + SCENE_REPLACEMENT_COMBINED;
	pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_sizes[1].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE + MAX_GLTEXTURES + MAX_SANITY_LIGHTMAPS + SCENE_REPLACEMENT_STORAGE;
	pool_sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
	pool_sizes[2].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE;
	pool_sizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_sizes[3].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE;
	pool_sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_sizes[4].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE + MAX_SANITY_LIGHTMAPS * 2;
	pool_sizes[5].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	pool_sizes[5].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE + (MAX_SANITY_LIGHTMAPS * 2);
	pool_sizes[6].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
	pool_sizes[6].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE + SCENE_REPLACEMENT_INPUT;
	pool_sizes[7].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	pool_sizes[7].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE + (1 + MAXLIGHTMAPS * 3 / 4) * MAX_SANITY_LIGHTMAPS;
	pool_sizes[8].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	pool_sizes[8].descriptorCount = MIN_NB_DESCRIPTORS_PER_TYPE;

	ZEROED_STRUCT (VkDescriptorPoolCreateInfo, descriptor_pool_create_info);
	descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptor_pool_create_info.maxSets = MAX_GLTEXTURES + MAX_SANITY_LIGHTMAPS + 128 + SCENE_REPLACEMENT_SETS;
	descriptor_pool_create_info.poolSizeCount = countof (pool_sizes);
	descriptor_pool_create_info.pPoolSizes = pool_sizes;
	descriptor_pool_create_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

	vkCreateDescriptorPool (vulkan_globals.device, &descriptor_pool_create_info, NULL, &vulkan_globals.descriptor_pool);
}

/*
===============
R_CreatePipelineLayouts
===============
*/
void R_CreatePipelineLayouts ()
{
	Sys_Printf ("Creating pipeline layouts\n");

	VkResult err;

	{
		// Basic
		VkDescriptorSetLayout basic_descriptor_set_layouts[2] = {
			vulkan_globals.single_texture_set_layout.handle, vulkan_globals.mboit_input_attachment_set_layout.handle};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 22 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = countof (basic_descriptor_set_layouts);
		pipeline_layout_create_info.pSetLayouts = basic_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.basic_pipeline_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.basic_pipeline_layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "basic_pipeline_layout");
		vulkan_globals.basic_pipeline_layout.push_constant_range = push_constant_range;
		vulkan_globals.basic_pipeline_layout.mboit_input_attachment_set = 1;

		basic_descriptor_set_layouts[1] = vulkan_globals.input_attachment_set_layout.handle;
		push_constant_range.size = 22 * sizeof (float);
		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.fte_particle_pipeline_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.fte_particle_pipeline_layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "fte_particle_pipeline_layout");
		vulkan_globals.fte_particle_pipeline_layout.push_constant_range = push_constant_range;
		vulkan_globals.fte_particle_pipeline_layout.mboit_input_attachment_set = -1;
	}

	{
		// GUI texture plus independently selected sampler
		VkDescriptorSetLayout gui_descriptor_set_layouts[2] = {vulkan_globals.single_texture_set_layout.handle, vulkan_globals.gui_sampler_set_layout.handle};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 22 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = countof (gui_descriptor_set_layouts);
		pipeline_layout_create_info.pSetLayouts = gui_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.gui_pipeline_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.gui_pipeline_layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "gui_pipeline_layout");
		vulkan_globals.gui_pipeline_layout.push_constant_range = push_constant_range;
		vulkan_globals.gui_pipeline_layout.mboit_input_attachment_set = -1;
	}

	{
		// World
		VkDescriptorSetLayout world_descriptor_set_layouts[5] = {
			vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle,
			vulkan_globals.mboit_input_attachment_set_layout.handle, vulkan_globals.bmodel_instances_set_layout.handle};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 22 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = countof (world_descriptor_set_layouts);
		pipeline_layout_create_info.pSetLayouts = world_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.world_pipeline_layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.world_pipeline_layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "world_pipeline_layout");
		vulkan_globals.world_pipeline_layout.push_constant_range = push_constant_range;
		vulkan_globals.world_pipeline_layout.mboit_input_attachment_set = 3;
	}

#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
	if (vulkan_globals.mesh_shader && vulkan_globals.world_mesh_set_layout.handle)
	{
		VkDescriptorSetLayout layouts[] = {
			vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle,
			vulkan_globals.mboit_input_attachment_set_layout.handle, vulkan_globals.world_mesh_set_layout.handle};
		VkPushConstantRange range = {VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_MESH_BIT_EXT, 0, 128};
		VkPipelineLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = countof (layouts), .pSetLayouts = layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
		if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &vulkan_globals.world_mesh_pipeline_layout.handle) == VK_SUCCESS)
		{
			vulkan_globals.world_mesh_pipeline_layout.push_constant_range = range;
			vulkan_globals.world_mesh_pipeline_layout.mboit_input_attachment_set = 3;
		}
#if defined(HAVE_WORLD_TASK_SHADER)
		if (vulkan_globals.task_shader)
		{
			range.stageFlags |= VK_SHADER_STAGE_TASK_BIT_EXT;
			if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &vulkan_globals.world_task_pipeline_layout.handle) == VK_SUCCESS)
			{
				vulkan_globals.world_task_pipeline_layout.push_constant_range = range;
				vulkan_globals.world_task_pipeline_layout.mboit_input_attachment_set = 3;
			}
		}
#endif
	}
#endif

	{
		// Alias
		VkDescriptorSetLayout alias_descriptor_set_layouts[4] = {
			vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.ubo_set_layout.handle,
			vulkan_globals.mboit_input_attachment_set_layout.handle};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 22 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = countof (alias_descriptor_set_layouts);
		pipeline_layout_create_info.pSetLayouts = alias_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (
			vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.alias_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName (
			(uint64_t)vulkan_globals.alias_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "alias_pipeline_layout");
		vulkan_globals.alias_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.push_constant_range = push_constant_range;
		vulkan_globals.alias_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.mboit_input_attachment_set = 3;
	}

	{
		// MD5
		VkDescriptorSetLayout md5_descriptor_set_layouts[5] = {
			vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.ubo_set_layout.handle,
			vulkan_globals.joints_buffer_set_layout.handle, vulkan_globals.mboit_input_attachment_set_layout.handle};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 22 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = countof (md5_descriptor_set_layouts);
		pipeline_layout_create_info.pSetLayouts = md5_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (
			vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.md5_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName (
			(uint64_t)vulkan_globals.md5_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "md5_pipeline_layout");
		vulkan_globals.md5_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.push_constant_range = push_constant_range;
		vulkan_globals.md5_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.mboit_input_attachment_set = 4;
	}

	{
		// Sky
		VkDescriptorSetLayout sky_layer_descriptor_set_layouts[2] = {
			vulkan_globals.single_texture_set_layout.handle,
			vulkan_globals.single_texture_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 27 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 1;
		pipeline_layout_create_info.pSetLayouts = sky_layer_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.sky_pipeline_layout[0].handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.sky_pipeline_layout[0].handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "sky_pipeline_layout");
		vulkan_globals.sky_pipeline_layout[0].push_constant_range = push_constant_range;

		push_constant_range.size = 25 * sizeof (float);
		pipeline_layout_create_info.setLayoutCount = 2;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.sky_pipeline_layout[1].handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.sky_pipeline_layout[1].handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "sky_layer_pipeline_layout");
		vulkan_globals.sky_pipeline_layout[1].push_constant_range = push_constant_range;
	}

	{
		// Postprocess
		VkDescriptorSetLayout postprocess_descriptor_set_layouts[1] = {
			vulkan_globals.input_attachment_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 2 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 1;
		pipeline_layout_create_info.pSetLayouts = postprocess_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.postprocess_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.postprocess_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "postprocess_pipeline_layout");
		vulkan_globals.postprocess_pipeline.layout.push_constant_range = push_constant_range;
	}

	{
		// Entity-only screen-space occlusion.
		VkDescriptorSetLayout layouts[] = {
			vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle,
			vulkan_globals.single_texture_set_layout.handle};
		VkPushConstantRange		   range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (ssao_constants_t)};
		VkPipelineLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = countof (layouts),
			.pSetLayouts = layouts,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &range,
		};
		if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &ssao_layout.handle) != VK_SUCCESS)
			Sys_Error ("Couldn't create entity SSAO layout");
		ssao_layout.push_constant_range = range;
		ssao_layout.mboit_input_attachment_set = -1;
		layouts[2] = vulkan_globals.single_texture_cs_write_set_layout.handle;
		layouts[3] = vulkan_globals.single_texture_cs_write_set_layout.handle;
		range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &ssao_compute_layout.handle) != VK_SUCCESS)
			Sys_Error ("Couldn't create entity GTAO compute layout");
		ssao_compute_layout.push_constant_range = range;
		ssao_compute_layout.mboit_input_attachment_set = -1;
		ssao_prepare_pipeline.layout = ssao_compute_layout;
		ssao_evaluate_pipelines[0].layout = ssao_compute_layout;
		ssao_filter_pipeline.layout = ssao_compute_layout;
		const VkDescriptorSetLayoutBinding lookup_bindings[] = {
			{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
			{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
		const VkDescriptorSetLayoutCreateInfo lookup_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = countof (lookup_bindings), .pBindings = lookup_bindings};
		ssao_lookup_set_layout.num_combined_image_samplers = 2;
		if (vkCreateDescriptorSetLayout (vulkan_globals.device, &lookup_info, NULL, &ssao_lookup_set_layout.handle) != VK_SUCCESS)
			Sys_Error ("Couldn't create GTAO lookup descriptor layout");
		layouts[1] = ssao_lookup_set_layout.handle;
		if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &ssao_evaluate_pipelines[0].layout.handle) != VK_SUCCESS)
			Sys_Error ("Couldn't create GTAO evaluator pipeline layout");
		for (int i = 1; i < countof (ssao_evaluate_pipelines); ++i)
			ssao_evaluate_pipelines[i].layout = ssao_evaluate_pipelines[0].layout;
		VkDescriptorSetLayoutBinding bindings[7];
		for (int i = 0; i < 7; ++i)
			bindings[i] = (VkDescriptorSetLayoutBinding){
				.binding = i,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
		const VkDescriptorSetLayoutCreateInfo set_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = countof (bindings), .pBindings = bindings};
		ssao_mip_set_layout.num_combined_image_samplers = 1;
		ssao_mip_set_layout.num_storage_images = 6;
		if (vkCreateDescriptorSetLayout (vulkan_globals.device, &set_info, NULL, &ssao_mip_set_layout.handle) != VK_SUCCESS)
			Sys_Error ("Couldn't create entity GTAO mip descriptor layout");
		info.setLayoutCount = 1;
		info.pSetLayouts = &ssao_mip_set_layout.handle;
		ssao_mip_pipeline.layout = ssao_compute_layout;
		if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &ssao_mip_pipeline.layout.handle) != VK_SUCCESS)
			Sys_Error ("Couldn't create entity GTAO mip pipeline layout");
	}

	{
		// WBOIT resolve
		VkDescriptorSetLayout wboit_resolve_descriptor_set_layouts[1] = {
			vulkan_globals.oit_input_attachment_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 1;
		pipeline_layout_create_info.pSetLayouts = wboit_resolve_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 0;
		pipeline_layout_create_info.pPushConstantRanges = NULL;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.wboit_resolve_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.wboit_resolve_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "wboit_resolve_pipeline_layout");
		vulkan_globals.wboit_resolve_pipeline.layout.push_constant_range = push_constant_range;
	}

	{
		// MBOIT resolve
		VkDescriptorSetLayout mboit_resolve_descriptor_set_layouts[1] = {
			vulkan_globals.mboit_input_attachment_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 1;
		pipeline_layout_create_info.pSetLayouts = mboit_resolve_descriptor_set_layouts;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.mboit_resolve_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.mboit_resolve_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "mboit_resolve_pipeline_layout");
		vulkan_globals.mboit_resolve_pipeline.layout.push_constant_range = push_constant_range;
		vulkan_globals.mboit_resolve_pipeline.layout.mboit_input_attachment_set = -1;
	}

	{
		// Screen effects
		VkDescriptorSetLayout screen_effects_descriptor_set_layouts[1] = {
			vulkan_globals.screen_effects_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 3 * sizeof (uint32_t) + 10 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 1;
		pipeline_layout_create_info.pSetLayouts = screen_effects_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.screen_effects_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.screen_effects_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "screen_effects_pipeline_layout");
		vulkan_globals.screen_effects_pipeline.layout.push_constant_range = push_constant_range;
	}

	{
		// Texture warp
		VkDescriptorSetLayout tex_warp_descriptor_set_layouts[2] = {
			vulkan_globals.single_texture_set_layout.handle,
			vulkan_globals.single_texture_cs_write_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 1 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 2;
		pipeline_layout_create_info.pSetLayouts = tex_warp_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.cs_tex_warp_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.cs_tex_warp_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "cs_tex_warp_pipeline_layout");
		vulkan_globals.cs_tex_warp_pipeline.layout.push_constant_range = push_constant_range;
	}

	{
		// Show triangles
		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 0;
		pipeline_layout_create_info.pushConstantRangeCount = 0;

		err = vkCreatePipelineLayout (
			vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.showtris_pipeline[MAIN_RENDER_PASS_STANDARD].layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName (
			(uint64_t)vulkan_globals.showtris_pipeline[MAIN_RENDER_PASS_STANDARD].layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "showtris_pipeline_layout");
		vulkan_globals.showtris_pipeline[MAIN_RENDER_PASS_STANDARD].layout.push_constant_range = push_constant_range;
	}

	{
		// Update lightmaps
		VkDescriptorSetLayout update_lightmap_descriptor_set_layouts[1] = {
			vulkan_globals.lightmap_compute_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 11 * sizeof (uint32_t);
		push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 1;
		pipeline_layout_create_info.pSetLayouts = update_lightmap_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.update_lightmap_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.update_lightmap_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "update_lightmap_pipeline_layout");
		vulkan_globals.update_lightmap_pipeline.layout.push_constant_range = push_constant_range;
	}

	if (vulkan_globals.ray_query)
	{
		// Update lightmaps RT
		VkDescriptorSetLayout update_lightmap_rt_descriptor_set_layouts[2] = {
			vulkan_globals.lightmap_compute_set_layout.handle,
			vulkan_globals.ray_query_push_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 12 * sizeof (uint32_t);
		push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = countof (update_lightmap_rt_descriptor_set_layouts);
		pipeline_layout_create_info.pSetLayouts = update_lightmap_rt_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.update_lightmap_rt_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName (
			(uint64_t)vulkan_globals.update_lightmap_rt_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "update_lightmap_rt_pipeline_layout");
		vulkan_globals.update_lightmap_rt_pipeline.layout.push_constant_range = push_constant_range;
	}

	{
		// Indirect draw
		VkDescriptorSetLayout indirect_draw_descriptor_set_layouts[1] = {
			vulkan_globals.indirect_compute_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 7 * sizeof (uint32_t);
		push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 1;
		pipeline_layout_create_info.pSetLayouts = indirect_draw_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.indirect_draw_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.indirect_draw_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "indirect_draw_pipeline_layout");
		vulkan_globals.indirect_draw_pipeline.layout.push_constant_range = push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.indirect_clear_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.indirect_clear_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "indirect_clear_pipeline_layout");
		vulkan_globals.indirect_clear_pipeline.layout.push_constant_range = push_constant_range;

		// Same storage-buffer layout, but cull whole meshlets against the view frustum.
		push_constant_range.size = 96;
		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.meshlet_cull_pipeline.layout.handle);
		if (err == VK_SUCCESS)
		{
			vulkan_globals.meshlet_cull_pipeline.layout.push_constant_range = push_constant_range;
			GL_SetObjectName ((uint64_t)vulkan_globals.meshlet_cull_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "meshlet_cull_layout");

			VkDescriptorSetLayout layouts[2] = {indirect_draw_descriptor_set_layouts[0], MLHiZ_Layout ()};
			if (layouts[1])
			{
				pipeline_layout_create_info.setLayoutCount = countof (layouts);
				pipeline_layout_create_info.pSetLayouts = layouts;
				err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.meshlet_hiz_pipeline.layout.handle);
				if (err == VK_SUCCESS)
				{
					vulkan_globals.meshlet_hiz_pipeline.layout.push_constant_range = push_constant_range;
					GL_SetObjectName ((uint64_t)vulkan_globals.meshlet_hiz_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "meshlet_hiz_layout");
				}
			}
		}
		else
			Sys_Printf ("Meshlet pipeline layout unavailable (%i); using stock BSP renderer\n", err);
	}

	if (vulkan_globals.ecs_cull_set_layout.handle)
	{
		VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 96};
		VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1, .pSetLayouts = &vulkan_globals.ecs_cull_set_layout.handle,
			.pushConstantRangeCount = 1, .pPushConstantRanges = &range};
		if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &vulkan_globals.ecs_cull_pipeline.layout.handle) == VK_SUCCESS)
			vulkan_globals.ecs_cull_pipeline.layout.push_constant_range = range;
	}

	if (vulkan_globals.ray_query)
	{
		// Mesh interpolate pipeline (MDL/MD3) - uses buffer device addresses via push constants
		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 40; // sizeof(mesh_interpolate_push_constants_t)
		push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = 0;
		pipeline_layout_create_info.pSetLayouts = NULL;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.mesh_interpolate_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.mesh_interpolate_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "mesh_interpolate_pipeline_layout");
		vulkan_globals.mesh_interpolate_pipeline.layout.push_constant_range = push_constant_range;

		// Skinning pipeline (MD5) - uses buffer device addresses via push constants
		push_constant_range.size = q_max (sizeof (skinning_push_constants_t), sizeof (mesh_interpolate_push_constants_t));

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.skinning_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.skinning_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "skinning_pipeline_layout");
		vulkan_globals.skinning_pipeline.layout.push_constant_range = push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.skinning_8_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.skinning_8_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "skinning_8_pipeline_layout");
		vulkan_globals.skinning_8_pipeline.layout.push_constant_range = push_constant_range;
	}

#if defined(_DEBUG)
	if (vulkan_globals.ray_query)
	{
		// Ray debug
		VkDescriptorSetLayout ray_debug_descriptor_set_layouts[2] = {
			vulkan_globals.ray_debug_set_layout.handle,
			vulkan_globals.ray_query_push_set_layout.handle,
		};

		ZEROED_STRUCT (VkPushConstantRange, push_constant_range);
		push_constant_range.offset = 0;
		push_constant_range.size = 15 * sizeof (float);
		push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

		ZEROED_STRUCT (VkPipelineLayoutCreateInfo, pipeline_layout_create_info);
		pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipeline_layout_create_info.setLayoutCount = countof (ray_debug_descriptor_set_layouts);
		pipeline_layout_create_info.pSetLayouts = ray_debug_descriptor_set_layouts;
		pipeline_layout_create_info.pushConstantRangeCount = 1;
		pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;

		err = vkCreatePipelineLayout (vulkan_globals.device, &pipeline_layout_create_info, NULL, &vulkan_globals.ray_debug_pipeline.layout.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreatePipelineLayout failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.ray_debug_pipeline.layout.handle, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "ray_debug_pipeline_layout");
		vulkan_globals.ray_debug_pipeline.layout.push_constant_range = push_constant_range;
	}
#endif
}

/*
===============
R_InitSamplers
===============
*/
void R_InitSamplers ()
{
	GL_WaitForDeviceIdle ();
	Sys_Printf ("Initializing samplers\n");

	VkResult err;

	if (vulkan_globals.point_sampler == VK_NULL_HANDLE)
	{
		ZEROED_STRUCT (VkSamplerCreateInfo, sampler_create_info);
		sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_create_info.magFilter = VK_FILTER_NEAREST;
		sampler_create_info.minFilter = VK_FILTER_NEAREST;
		sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.mipLodBias = 0.0f;
		sampler_create_info.maxAnisotropy = 1.0f;
		sampler_create_info.minLod = 0;
		sampler_create_info.maxLod = FLT_MAX;

		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.point_sampler);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.point_sampler, VK_OBJECT_TYPE_SAMPLER, "point");

		// GUI sampling is deliberately isolated from texture-manager sampler state.
		sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.gui_point_sampler);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.gui_point_sampler, VK_OBJECT_TYPE_SAMPLER, "GUI point");
		sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

		sampler_create_info.anisotropyEnable = VK_TRUE;
		sampler_create_info.maxAnisotropy = vulkan_globals.device_properties.limits.maxSamplerAnisotropy;
		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.point_aniso_sampler);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.point_aniso_sampler, VK_OBJECT_TYPE_SAMPLER, "point_aniso");

		sampler_create_info.magFilter = VK_FILTER_LINEAR;
		sampler_create_info.minFilter = VK_FILTER_LINEAR;
		sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler_create_info.anisotropyEnable = VK_FALSE;
		sampler_create_info.maxAnisotropy = 1.0f;

		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.linear_sampler);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.linear_sampler, VK_OBJECT_TYPE_SAMPLER, "linear");

		sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.gui_linear_sampler);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)vulkan_globals.gui_linear_sampler, VK_OBJECT_TYPE_SAMPLER, "GUI linear");
		sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

		sampler_create_info.anisotropyEnable = VK_TRUE;
		sampler_create_info.maxAnisotropy = vulkan_globals.device_properties.limits.maxSamplerAnisotropy;
		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.linear_aniso_sampler);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.linear_aniso_sampler, VK_OBJECT_TYPE_SAMPLER, "linear_aniso");
	}

	if (vulkan_globals.point_sampler_lod_bias != VK_NULL_HANDLE)
	{
		vkDestroySampler (vulkan_globals.device, vulkan_globals.point_sampler_lod_bias, NULL);
		vkDestroySampler (vulkan_globals.device, vulkan_globals.point_aniso_sampler_lod_bias, NULL);
		vkDestroySampler (vulkan_globals.device, vulkan_globals.linear_sampler_lod_bias, NULL);
		vkDestroySampler (vulkan_globals.device, vulkan_globals.linear_aniso_sampler_lod_bias, NULL);
	}

	{
		float lod_bias = 0.0f;
		if (r_lodbias.value)
		{
			if (vulkan_globals.supersampling)
			{
				switch (vulkan_globals.sample_count)
				{
				case VK_SAMPLE_COUNT_2_BIT:
					lod_bias -= 0.5f;
					break;
				case VK_SAMPLE_COUNT_4_BIT:
					lod_bias -= 1.0f;
					break;
				case VK_SAMPLE_COUNT_8_BIT:
					lod_bias -= 1.5f;
					break;
				case VK_SAMPLE_COUNT_16_BIT:
					lod_bias -= 2.0f;
					break;
				default: /* silences gcc's -Wswitch */
					break;
				}
			}
		}

		lod_bias += gl_lodbias.value;

		Sys_Printf ("Texture lod bias: %f\n", lod_bias);

		ZEROED_STRUCT (VkSamplerCreateInfo, sampler_create_info);
		sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_create_info.magFilter = VK_FILTER_NEAREST;
		sampler_create_info.minFilter = VK_FILTER_NEAREST;
		sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sampler_create_info.mipLodBias = lod_bias;
		sampler_create_info.maxAnisotropy = 1.0f;
		sampler_create_info.minLod = 0;
		sampler_create_info.maxLod = FLT_MAX;

		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.point_sampler_lod_bias);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.point_sampler_lod_bias, VK_OBJECT_TYPE_SAMPLER, "point_lod_bias");

		sampler_create_info.anisotropyEnable = VK_TRUE;
		sampler_create_info.maxAnisotropy = vulkan_globals.device_properties.limits.maxSamplerAnisotropy;
		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.point_aniso_sampler_lod_bias);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.point_aniso_sampler_lod_bias, VK_OBJECT_TYPE_SAMPLER, "point_aniso_lod_bias");

		sampler_create_info.magFilter = VK_FILTER_LINEAR;
		sampler_create_info.minFilter = VK_FILTER_LINEAR;
		sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler_create_info.anisotropyEnable = VK_FALSE;
		sampler_create_info.maxAnisotropy = 1.0f;

		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.linear_sampler_lod_bias);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.linear_sampler_lod_bias, VK_OBJECT_TYPE_SAMPLER, "linear_lod_bias");

		sampler_create_info.anisotropyEnable = VK_TRUE;
		sampler_create_info.maxAnisotropy = vulkan_globals.device_properties.limits.maxSamplerAnisotropy;
		err = vkCreateSampler (vulkan_globals.device, &sampler_create_info, NULL, &vulkan_globals.linear_aniso_sampler_lod_bias);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateSampler failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)vulkan_globals.linear_aniso_sampler_lod_bias, VK_OBJECT_TYPE_SAMPLER, "linear_aniso_lod_bias");
	}

	if (vulkan_globals.gui_sampler_descriptor_sets[0] == VK_NULL_HANDLE)
	{
		const VkSampler samplers[2] = {vulkan_globals.gui_point_sampler, vulkan_globals.gui_linear_sampler};
		for (int i = 0; i < countof (samplers); ++i)
		{
			vulkan_globals.gui_sampler_descriptor_sets[i] = R_AllocateDescriptorSet (&vulkan_globals.gui_sampler_set_layout);
			GL_SetObjectName (
				(uint64_t)vulkan_globals.gui_sampler_descriptor_sets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET,
				i == 0 ? "GUI point sampler descriptor set" : "GUI linear sampler descriptor set");

			ZEROED_STRUCT (VkDescriptorImageInfo, sampler_info);
			sampler_info.sampler = samplers[i];

			ZEROED_STRUCT (VkWriteDescriptorSet, sampler_write);
			sampler_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			sampler_write.dstSet = vulkan_globals.gui_sampler_descriptor_sets[i];
			sampler_write.dstBinding = 0;
			sampler_write.descriptorCount = 1;
			sampler_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			sampler_write.pImageInfo = &sampler_info;
			vkUpdateDescriptorSets (vulkan_globals.device, 1, &sampler_write, 0, NULL);
		}
	}

	TexMgr_UpdateTextureDescriptorSets ();
}

/*
===============
R_CreateShaderModule
===============
*/
static VkShaderModule R_TryCreateShaderModule (const byte *code, const int size, const char *name)
{
	ZEROED_STRUCT (VkShaderModuleCreateInfo, module_create_info);
	module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_create_info.pNext = NULL;
	module_create_info.codeSize = size;
	module_create_info.pCode = (const uint32_t *)code;

	VkShaderModule module;
	VkResult	   err = vkCreateShaderModule (vulkan_globals.device, &module_create_info, NULL, &module);
	if (err != VK_SUCCESS)
	{
		Sys_Printf ("vkCreateShaderModule failed (%s) with code %i\n", name, (int)err);
		return VK_NULL_HANDLE;
	}

	GL_SetObjectName ((uint64_t)module, VK_OBJECT_TYPE_SHADER_MODULE, name);

	return module;
}

static VkShaderModule R_CreateShaderModuleForTransaction (void *context, const unsigned char *code, int size, const char *name)
{
	(void)context;
	return R_TryCreateShaderModule (code, size, name);
}

static void R_DestroyShaderModuleForTransaction (void *context, VkShaderModule module)
{
	(void)context;
	vkDestroyShaderModule (vulkan_globals.device, module, NULL);
}

typedef struct pipeline_create_infos_s
{
	VkPipelineShaderStageCreateInfo		   shader_stages[3];
	VkPipelineDynamicStateCreateInfo	   dynamic_state;
	VkDynamicState						   dynamic_states[3];
	VkPipelineVertexInputStateCreateInfo   vertex_input_state;
	VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
	VkPipelineViewportStateCreateInfo	   viewport_state;
	VkPipelineRasterizationStateCreateInfo rasterization_state;
	VkPipelineMultisampleStateCreateInfo   multisample_state;
	VkPipelineDepthStencilStateCreateInfo  depth_stencil_state;
	VkPipelineColorBlendStateCreateInfo	   color_blend_state;
	VkPipelineColorBlendAttachmentState	   blend_attachment_states[3];
	VkGraphicsPipelineCreateInfo		   graphics_pipeline;
	qboolean							   skip_ssao_stencil;
} pipeline_create_infos_t;

static VkVertexInputAttributeDescription basic_vertex_input_attribute_descriptions[3];
static VkVertexInputBindingDescription	 basic_vertex_binding_description;
static VkVertexInputAttributeDescription draw_pic_vertex_input_attribute_descriptions[4];
static VkVertexInputBindingDescription	 draw_pic_vertex_binding_description;
static VkVertexInputAttributeDescription world_vertex_input_attribute_descriptions[3];
static VkVertexInputBindingDescription	 world_vertex_binding_description;
static VkVertexInputAttributeDescription alias_vertex_input_attribute_descriptions[5];
static VkVertexInputBindingDescription	 alias_vertex_binding_descriptions[3];
static VkVertexInputAttributeDescription md5_vertex_input_attribute_descriptions[8];
static VkVertexInputBindingDescription	 md5_vertex_binding_description;
static VkVertexInputAttributeDescription md5_8_vertex_input_attribute_descriptions[13];
static VkVertexInputBindingDescription	 md5_8_vertex_binding_description;

#define DECLARE_SHADER_MODULE(name) static VkShaderModule name##_module
#define CREATE_SHADER_MODULE_CODE(name, code_name, required)                                               \
	do                                                                                                    \
	{                                                                                                     \
		if (!ShaderTransaction_Create (&transaction, &name##_module, code_name##_spv, code_name##_spv_size, \
			#code_name, required))                                                                        \
			goto fail;                                                                                \
	} while (0)
#define CREATE_SHADER_MODULE(name) CREATE_SHADER_MODULE_CODE (name, name, true)
#define CREATE_SHADER_MODULE_COND(name, cond) do { if (cond) CREATE_SHADER_MODULE (name); } while (0)
#define CREATE_SHADER_MODULE_OPTIONAL(name, cond) do { if (cond) CREATE_SHADER_MODULE_CODE (name, name, false); } while (0)
#define DESTROY_SHADER_MODULE(name)                                             \
	do                                                                          \
	{                                                                           \
		if (name##_module != VK_NULL_HANDLE)                                    \
			vkDestroyShaderModule (vulkan_globals.device, name##_module, NULL); \
		name##_module = VK_NULL_HANDLE;                                         \
	} while (0)

DECLARE_SHADER_MODULE (basic_vert);
DECLARE_SHADER_MODULE (basic_frag);
DECLARE_SHADER_MODULE (fte_particles_frag);
DECLARE_SHADER_MODULE (fte_particles_msaa_frag);
DECLARE_SHADER_MODULE (basic_oit_frag);
DECLARE_SHADER_MODULE (basic_mboit_moment_frag);
DECLARE_SHADER_MODULE (basic_mboit_composite_frag);
DECLARE_SHADER_MODULE (basic_mboit_composite_msaa_frag);
DECLARE_SHADER_MODULE (basic_alphatest_frag);
DECLARE_SHADER_MODULE (basic_notex_frag);
DECLARE_SHADER_MODULE (draw_pic_frag);
DECLARE_SHADER_MODULE (draw_pic_alphatest_frag);
DECLARE_SHADER_MODULE (draw_pic_xbr_frag);
DECLARE_SHADER_MODULE (draw_pic_xbr_alphatest_frag);
DECLARE_SHADER_MODULE (draw_pic_xbr_vert);
DECLARE_SHADER_MODULE (world_vert);
DECLARE_SHADER_MODULE (world_frag);
DECLARE_SHADER_MODULE (world_oit_frag);
DECLARE_SHADER_MODULE (world_mboit_moment_frag);
DECLARE_SHADER_MODULE (world_mboit_composite_frag);
DECLARE_SHADER_MODULE (world_mboit_composite_msaa_frag);
DECLARE_SHADER_MODULE (alias_vert);
DECLARE_SHADER_MODULE (alias_instanced_vert);
DECLARE_SHADER_MODULE (alias_frag);
DECLARE_SHADER_MODULE (alias_alphatest_frag);
DECLARE_SHADER_MODULE (alias_oit_frag);
DECLARE_SHADER_MODULE (alias_alphatest_oit_frag);
DECLARE_SHADER_MODULE (alias_mboit_moment_frag);
DECLARE_SHADER_MODULE (alias_mboit_composite_frag);
DECLARE_SHADER_MODULE (alias_mboit_composite_msaa_frag);
DECLARE_SHADER_MODULE (alias_alphatest_mboit_moment_frag);
DECLARE_SHADER_MODULE (alias_alphatest_mboit_composite_frag);
DECLARE_SHADER_MODULE (alias_alphatest_mboit_composite_msaa_frag);
DECLARE_SHADER_MODULE (md5_mboit_composite_frag);
DECLARE_SHADER_MODULE (md5_mboit_composite_msaa_frag);
DECLARE_SHADER_MODULE (md5_alphatest_mboit_composite_frag);
DECLARE_SHADER_MODULE (md5_alphatest_mboit_composite_msaa_frag);
DECLARE_SHADER_MODULE (md5_vert);
DECLARE_SHADER_MODULE (md5_8_vert);
DECLARE_SHADER_MODULE (md5_debug_vert);
DECLARE_SHADER_MODULE (sky_layer_vert);
DECLARE_SHADER_MODULE (sky_layer_frag);
DECLARE_SHADER_MODULE (sky_box_frag);
DECLARE_SHADER_MODULE (sky_cube_vert);
DECLARE_SHADER_MODULE (sky_cube_frag);
DECLARE_SHADER_MODULE (postprocess_vert);
DECLARE_SHADER_MODULE (postprocess_frag);
DECLARE_SHADER_MODULE (scene_upscale_frag);
DECLARE_SHADER_MODULE (ssao_composite_frag);
DECLARE_SHADER_MODULE (ssao_composite_msaa_frag);
DECLARE_SHADER_MODULE (ssao_prepare_comp);
DECLARE_SHADER_MODULE (ssao_prepare_msaa_comp);
DECLARE_SHADER_MODULE (ssao_evaluate_comp);
DECLARE_SHADER_MODULE (ssao_evaluate_fp16_comp);
DECLARE_SHADER_MODULE (ssao_filter_comp);
DECLARE_SHADER_MODULE (ssao_filter_fp16_comp);
DECLARE_SHADER_MODULE (ssao_mip_comp);
DECLARE_SHADER_MODULE (ssao_mip_fp16_comp);
DECLARE_SHADER_MODULE (ssao_mip_msaa_comp);
DECLARE_SHADER_MODULE (ssao_mip_msaa_fp16_comp);
DECLARE_SHADER_MODULE (wboit_resolve_frag);
DECLARE_SHADER_MODULE (wboit_resolve_msaa_frag);
DECLARE_SHADER_MODULE (mboit_resolve_frag);
DECLARE_SHADER_MODULE (mboit_resolve_msaa_frag);
DECLARE_SHADER_MODULE (screen_effects_8bit_comp);
DECLARE_SHADER_MODULE (screen_effects_10bit_comp);
DECLARE_SHADER_MODULE (screen_effects_hdr_comp);
DECLARE_SHADER_MODULE (cs_tex_warp_comp);
DECLARE_SHADER_MODULE (indirect_comp);
DECLARE_SHADER_MODULE (indirect_clear_comp);
DECLARE_SHADER_MODULE (meshlet_cull_comp);
DECLARE_SHADER_MODULE (alias_ecs_cull_comp);
DECLARE_SHADER_MODULE (meshlet_hiz_comp);
#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
DECLARE_SHADER_MODULE (world_mesh_mesh);
#if defined(HAVE_WORLD_TASK_SHADER)
DECLARE_SHADER_MODULE (world_task_task);
DECLARE_SHADER_MODULE (world_task_mesh_mesh);
#endif
#endif
DECLARE_SHADER_MODULE (showtris_vert);
DECLARE_SHADER_MODULE (showtris_frag);
DECLARE_SHADER_MODULE (update_lightmap_8bit_comp);
DECLARE_SHADER_MODULE (update_lightmap_8bit_rt_comp);
DECLARE_SHADER_MODULE (update_lightmap_10bit_comp);
DECLARE_SHADER_MODULE (update_lightmap_10bit_rt_comp);
DECLARE_SHADER_MODULE (ray_debug_comp);
DECLARE_SHADER_MODULE (ray_debug_hdr_comp);
DECLARE_SHADER_MODULE (mesh_interpolate_comp);
DECLARE_SHADER_MODULE (skinning_comp);
DECLARE_SHADER_MODULE (skinning_8_comp);

enum
{
	MAIN_COLOR_ATTACHMENT_COUNT = 1,
	WBOIT_COLOR_ATTACHMENT_COUNT = 2,
	MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT = 2,
	MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT = 1,
};

static void R_SetWBOITBlend (VkPipelineColorBlendAttachmentState *blend_attachment_states)
{
	for (int i = 0; i < 3; ++i)
	{
		blend_attachment_states[i].blendEnable = VK_FALSE;
		blend_attachment_states[i].colorWriteMask = 0;
		blend_attachment_states[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment_states[i].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend_attachment_states[i].colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment_states[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment_states[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend_attachment_states[i].alphaBlendOp = VK_BLEND_OP_ADD;
	}

	blend_attachment_states[0].blendEnable = VK_TRUE;
	blend_attachment_states[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blend_attachment_states[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment_states[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment_states[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment_states[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

	blend_attachment_states[1].blendEnable = VK_TRUE;
	blend_attachment_states[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
	blend_attachment_states[1].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
	blend_attachment_states[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	blend_attachment_states[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blend_attachment_states[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
}

static void R_SetMBOITMomentBlend (VkPipelineColorBlendAttachmentState *blend_attachment_states)
{
	for (int i = 0; i < 2; ++i)
	{
		blend_attachment_states[i].blendEnable = VK_TRUE;
		blend_attachment_states[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blend_attachment_states[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment_states[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment_states[i].colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment_states[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment_states[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment_states[i].alphaBlendOp = VK_BLEND_OP_ADD;
	}
	blend_attachment_states[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
}

static void R_SetMBOITCompositeBlend (VkPipelineColorBlendAttachmentState *blend_attachment_states)
{
	blend_attachment_states[0].blendEnable = VK_TRUE;
	blend_attachment_states[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blend_attachment_states[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment_states[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment_states[0].colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment_states[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment_states[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment_states[0].alphaBlendOp = VK_BLEND_OP_ADD;
}

/*
===============
R_InitVertexAttributes
===============
*/
static void R_InitVertexAttributes ()
{
	{
		basic_vertex_input_attribute_descriptions[0].binding = 0;
		basic_vertex_input_attribute_descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		basic_vertex_input_attribute_descriptions[0].location = 0;
		basic_vertex_input_attribute_descriptions[0].offset = 0;
		basic_vertex_input_attribute_descriptions[1].binding = 0;
		basic_vertex_input_attribute_descriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
		basic_vertex_input_attribute_descriptions[1].location = 1;
		basic_vertex_input_attribute_descriptions[1].offset = 12;
		basic_vertex_input_attribute_descriptions[2].binding = 0;
		basic_vertex_input_attribute_descriptions[2].format = VK_FORMAT_R8G8B8A8_UNORM;
		basic_vertex_input_attribute_descriptions[2].location = 2;
		basic_vertex_input_attribute_descriptions[2].offset = 20;

		basic_vertex_binding_description.binding = 0;
		basic_vertex_binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		basic_vertex_binding_description.stride = 24;
	}

	{
		memcpy (draw_pic_vertex_input_attribute_descriptions, basic_vertex_input_attribute_descriptions, sizeof (basic_vertex_input_attribute_descriptions));
		draw_pic_vertex_input_attribute_descriptions[3].binding = 0;
		draw_pic_vertex_input_attribute_descriptions[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		draw_pic_vertex_input_attribute_descriptions[3].location = 3;
		draw_pic_vertex_input_attribute_descriptions[3].offset = offsetof (draw_pic_vertex_t, texture_region);
		draw_pic_vertex_binding_description.binding = 0;
		draw_pic_vertex_binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		draw_pic_vertex_binding_description.stride = sizeof (draw_pic_vertex_t);
	}

	{
		world_vertex_input_attribute_descriptions[0].binding = 0;
		world_vertex_input_attribute_descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		world_vertex_input_attribute_descriptions[0].location = 0;
		world_vertex_input_attribute_descriptions[0].offset = 0;
		world_vertex_input_attribute_descriptions[1].binding = 0;
		world_vertex_input_attribute_descriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
		world_vertex_input_attribute_descriptions[1].location = 1;
		world_vertex_input_attribute_descriptions[1].offset = 12;
		world_vertex_input_attribute_descriptions[2].binding = 0;
		world_vertex_input_attribute_descriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
		world_vertex_input_attribute_descriptions[2].location = 2;
		world_vertex_input_attribute_descriptions[2].offset = 20;

		world_vertex_binding_description.binding = 0;
		world_vertex_binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		world_vertex_binding_description.stride = 28;
	}

	{
		alias_vertex_input_attribute_descriptions[0].binding = 0;
		alias_vertex_input_attribute_descriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
		alias_vertex_input_attribute_descriptions[0].location = 0;
		alias_vertex_input_attribute_descriptions[0].offset = 0;
		alias_vertex_input_attribute_descriptions[1].binding = 1;
		alias_vertex_input_attribute_descriptions[1].format = VK_FORMAT_R16G16B16A16_UNORM;
		alias_vertex_input_attribute_descriptions[1].location = 1;
		alias_vertex_input_attribute_descriptions[1].offset = 0;
		alias_vertex_input_attribute_descriptions[2].binding = 1;
		alias_vertex_input_attribute_descriptions[2].format = VK_FORMAT_R8G8B8A8_SNORM;
		alias_vertex_input_attribute_descriptions[2].location = 2;
		alias_vertex_input_attribute_descriptions[2].offset = 8;
		alias_vertex_input_attribute_descriptions[3].binding = 2;
		alias_vertex_input_attribute_descriptions[3].format = VK_FORMAT_R16G16B16A16_UNORM;
		alias_vertex_input_attribute_descriptions[3].location = 3;
		alias_vertex_input_attribute_descriptions[3].offset = 0;
		alias_vertex_input_attribute_descriptions[4].binding = 2;
		alias_vertex_input_attribute_descriptions[4].format = VK_FORMAT_R8G8B8A8_SNORM;
		alias_vertex_input_attribute_descriptions[4].location = 4;
		alias_vertex_input_attribute_descriptions[4].offset = 8;

		alias_vertex_binding_descriptions[0].binding = 0;
		alias_vertex_binding_descriptions[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		alias_vertex_binding_descriptions[0].stride = 8;
		alias_vertex_binding_descriptions[1].binding = 1;
		alias_vertex_binding_descriptions[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		alias_vertex_binding_descriptions[1].stride = 12;
		alias_vertex_binding_descriptions[2].binding = 2;
		alias_vertex_binding_descriptions[2].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		alias_vertex_binding_descriptions[2].stride = 12;
	}

	{
		// Matches md5vert_t
		md5_vertex_input_attribute_descriptions[0].binding = 0;
		md5_vertex_input_attribute_descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		md5_vertex_input_attribute_descriptions[0].location = 0;
		md5_vertex_input_attribute_descriptions[0].offset = 0;
		md5_vertex_input_attribute_descriptions[1].binding = 0;
		md5_vertex_input_attribute_descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		md5_vertex_input_attribute_descriptions[1].location = 1;
		md5_vertex_input_attribute_descriptions[1].offset = 12;
		md5_vertex_input_attribute_descriptions[2].binding = 0;
		md5_vertex_input_attribute_descriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
		md5_vertex_input_attribute_descriptions[2].location = 2;
		md5_vertex_input_attribute_descriptions[2].offset = 24;
		md5_vertex_input_attribute_descriptions[3].binding = 0;
		md5_vertex_input_attribute_descriptions[3].format = VK_FORMAT_R8G8B8A8_UNORM;
		md5_vertex_input_attribute_descriptions[3].location = 3;
		md5_vertex_input_attribute_descriptions[3].offset = 32;
		md5_vertex_input_attribute_descriptions[4].binding = 0;
		md5_vertex_input_attribute_descriptions[4].format = VK_FORMAT_R8G8B8A8_UINT;
		md5_vertex_input_attribute_descriptions[4].location = 4;
		md5_vertex_input_attribute_descriptions[4].offset = 36;
		md5_vertex_input_attribute_descriptions[5].binding = 0;
		md5_vertex_input_attribute_descriptions[5].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_vertex_input_attribute_descriptions[5].location = 5;
		md5_vertex_input_attribute_descriptions[5].offset = (uint32_t)offsetof (md5vert_t, joint_position_x);
		md5_vertex_input_attribute_descriptions[6].binding = 0;
		md5_vertex_input_attribute_descriptions[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_vertex_input_attribute_descriptions[6].location = 6;
		md5_vertex_input_attribute_descriptions[6].offset = (uint32_t)offsetof (md5vert_t, joint_position_y);
		md5_vertex_input_attribute_descriptions[7].binding = 0;
		md5_vertex_input_attribute_descriptions[7].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_vertex_input_attribute_descriptions[7].location = 7;
		md5_vertex_input_attribute_descriptions[7].offset = (uint32_t)offsetof (md5vert_t, joint_position_z);

		md5_vertex_binding_description.binding = 0;
		md5_vertex_binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		md5_vertex_binding_description.stride = sizeof (md5vert_t);
	}

	{
		// Matches md5vert8_t
		md5_8_vertex_input_attribute_descriptions[0].binding = 0;
		md5_8_vertex_input_attribute_descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[0].location = 0;
		md5_8_vertex_input_attribute_descriptions[0].offset = 0;
		md5_8_vertex_input_attribute_descriptions[1].binding = 0;
		md5_8_vertex_input_attribute_descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[1].location = 1;
		md5_8_vertex_input_attribute_descriptions[1].offset = 12;
		md5_8_vertex_input_attribute_descriptions[2].binding = 0;
		md5_8_vertex_input_attribute_descriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[2].location = 2;
		md5_8_vertex_input_attribute_descriptions[2].offset = 24;
		md5_8_vertex_input_attribute_descriptions[3].binding = 0;
		md5_8_vertex_input_attribute_descriptions[3].format = VK_FORMAT_R8G8B8A8_UNORM;
		md5_8_vertex_input_attribute_descriptions[3].location = 3;
		md5_8_vertex_input_attribute_descriptions[3].offset = 32;
		md5_8_vertex_input_attribute_descriptions[4].binding = 0;
		md5_8_vertex_input_attribute_descriptions[4].format = VK_FORMAT_R8G8B8A8_UNORM;
		md5_8_vertex_input_attribute_descriptions[4].location = 4;
		md5_8_vertex_input_attribute_descriptions[4].offset = 36;
		md5_8_vertex_input_attribute_descriptions[5].binding = 0;
		md5_8_vertex_input_attribute_descriptions[5].format = VK_FORMAT_R8G8B8A8_UINT;
		md5_8_vertex_input_attribute_descriptions[5].location = 5;
		md5_8_vertex_input_attribute_descriptions[5].offset = 40;
		md5_8_vertex_input_attribute_descriptions[6].binding = 0;
		md5_8_vertex_input_attribute_descriptions[6].format = VK_FORMAT_R8G8B8A8_UINT;
		md5_8_vertex_input_attribute_descriptions[6].location = 6;
		md5_8_vertex_input_attribute_descriptions[6].offset = 44;
		const uint32_t influence_group_bytes = NUM_JOINT_INFLUENCES_4_WEIGHT * sizeof (float);
		md5_8_vertex_input_attribute_descriptions[7].binding = 0;
		md5_8_vertex_input_attribute_descriptions[7].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[7].location = 7;
		md5_8_vertex_input_attribute_descriptions[7].offset = (uint32_t)offsetof (md5vert8_t, joint_position_x);
		md5_8_vertex_input_attribute_descriptions[8].binding = 0;
		md5_8_vertex_input_attribute_descriptions[8].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[8].location = 8;
		md5_8_vertex_input_attribute_descriptions[8].offset = (uint32_t)offsetof (md5vert8_t, joint_position_x) + influence_group_bytes;
		md5_8_vertex_input_attribute_descriptions[9].binding = 0;
		md5_8_vertex_input_attribute_descriptions[9].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[9].location = 9;
		md5_8_vertex_input_attribute_descriptions[9].offset = (uint32_t)offsetof (md5vert8_t, joint_position_y);
		md5_8_vertex_input_attribute_descriptions[10].binding = 0;
		md5_8_vertex_input_attribute_descriptions[10].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[10].location = 10;
		md5_8_vertex_input_attribute_descriptions[10].offset = (uint32_t)offsetof (md5vert8_t, joint_position_y) + influence_group_bytes;
		md5_8_vertex_input_attribute_descriptions[11].binding = 0;
		md5_8_vertex_input_attribute_descriptions[11].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[11].location = 11;
		md5_8_vertex_input_attribute_descriptions[11].offset = (uint32_t)offsetof (md5vert8_t, joint_position_z);
		md5_8_vertex_input_attribute_descriptions[12].binding = 0;
		md5_8_vertex_input_attribute_descriptions[12].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		md5_8_vertex_input_attribute_descriptions[12].location = 12;
		md5_8_vertex_input_attribute_descriptions[12].offset = (uint32_t)offsetof (md5vert8_t, joint_position_z) + influence_group_bytes;

		md5_8_vertex_binding_description.binding = 0;
		md5_8_vertex_binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		md5_8_vertex_binding_description.stride = sizeof (md5vert8_t);
	}
}

/*
===============
R_InitDefaultStates
===============
*/
static void R_SetPipelineRenderPassVariant (pipeline_create_infos_t *infos, subpass_type_t stage, main_render_pass_variant_t variant);

static void R_InitDefaultStates (pipeline_create_infos_t *infos)
{
	memset (infos, 0, sizeof (pipeline_create_infos_t));

	infos->dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	infos->dynamic_state.pDynamicStates = infos->dynamic_states;

	infos->shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	infos->shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	infos->shader_stages[0].module = basic_vert_module;
	infos->shader_stages[0].pName = "main";

	infos->shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	infos->shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	infos->shader_stages[1].module = basic_frag_module;
	infos->shader_stages[1].pName = "main";

	infos->vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	infos->vertex_input_state.vertexAttributeDescriptionCount = 3;
	infos->vertex_input_state.pVertexAttributeDescriptions = basic_vertex_input_attribute_descriptions;
	infos->vertex_input_state.vertexBindingDescriptionCount = 1;
	infos->vertex_input_state.pVertexBindingDescriptions = &basic_vertex_binding_description;

	infos->input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	infos->input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	infos->viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	infos->viewport_state.viewportCount = 1;
	infos->dynamic_states[infos->dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
	infos->viewport_state.scissorCount = 1;
	infos->dynamic_states[infos->dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;

	infos->rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	infos->rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
	infos->rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT;
	infos->rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE;
	infos->rasterization_state.depthClampEnable = VK_FALSE;
	infos->rasterization_state.rasterizerDiscardEnable = VK_FALSE;
	infos->rasterization_state.depthBiasEnable = VK_FALSE;
	infos->rasterization_state.lineWidth = 1.0f;

	infos->multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	infos->multisample_state.rasterizationSamples = vulkan_globals.sample_count;
	// Only opaque geometry pipelines opt into supersampling.
	infos->multisample_state.minSampleShading = 1.0f;

	infos->depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	infos->depth_stencil_state.depthTestEnable = VK_FALSE;
	infos->depth_stencil_state.depthWriteEnable = VK_FALSE;
	infos->depth_stencil_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
	infos->depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
	infos->depth_stencil_state.back.failOp = VK_STENCIL_OP_KEEP;
	infos->depth_stencil_state.back.passOp = VK_STENCIL_OP_KEEP;
	infos->depth_stencil_state.back.compareOp = VK_COMPARE_OP_ALWAYS;
	infos->depth_stencil_state.stencilTestEnable = VK_FALSE;
	infos->depth_stencil_state.front = infos->depth_stencil_state.back;

	infos->color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	for (int i = 0; i < countof (infos->blend_attachment_states); ++i)
	{
		infos->blend_attachment_states[i].colorWriteMask =
			(i == 0) ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) : 0;
		infos->blend_attachment_states[i].blendEnable = VK_FALSE;
		infos->blend_attachment_states[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		infos->blend_attachment_states[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		infos->blend_attachment_states[i].colorBlendOp = VK_BLEND_OP_ADD;
		infos->blend_attachment_states[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		infos->blend_attachment_states[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		infos->blend_attachment_states[i].alphaBlendOp = VK_BLEND_OP_ADD;
	}
	infos->color_blend_state.attachmentCount = MAIN_COLOR_ATTACHMENT_COUNT;
	infos->color_blend_state.pAttachments = infos->blend_attachment_states;

	infos->graphics_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	infos->graphics_pipeline.stageCount = 2;
	infos->graphics_pipeline.pStages = infos->shader_stages;
	infos->graphics_pipeline.pVertexInputState = &infos->vertex_input_state;
	infos->graphics_pipeline.pInputAssemblyState = &infos->input_assembly_state;
	infos->graphics_pipeline.pViewportState = &infos->viewport_state;
	infos->graphics_pipeline.pRasterizationState = &infos->rasterization_state;
	infos->graphics_pipeline.pMultisampleState = &infos->multisample_state;
	infos->graphics_pipeline.pDepthStencilState = &infos->depth_stencil_state;
	infos->graphics_pipeline.pColorBlendState = &infos->color_blend_state;
	infos->graphics_pipeline.pDynamicState = &infos->dynamic_state;
	infos->graphics_pipeline.layout = vulkan_globals.basic_pipeline_layout.handle;
	R_SetPipelineRenderPassVariant (infos, SUBPASS_MAIN, MAIN_RENDER_PASS_STANDARD);
}

/*
===============
R_CopyPipelineCreateInfos

Every pipeline is created from an explicit baseline instead of inheriting whatever
the previously created pipeline happened to leave behind in the shared struct.
The copy re-points the nested create info pointers at the destination.
===============
*/
static void R_CopyPipelineCreateInfos (pipeline_create_infos_t *dst, const pipeline_create_infos_t *src)
{
	memcpy (dst, src, sizeof (pipeline_create_infos_t));
	dst->dynamic_state.pDynamicStates = dst->dynamic_states;
	dst->color_blend_state.pAttachments = dst->blend_attachment_states;
	dst->graphics_pipeline.pStages = dst->shader_stages;
	dst->graphics_pipeline.pVertexInputState = &dst->vertex_input_state;
	dst->graphics_pipeline.pInputAssemblyState = &dst->input_assembly_state;
	dst->graphics_pipeline.pViewportState = &dst->viewport_state;
	dst->graphics_pipeline.pRasterizationState = &dst->rasterization_state;
	dst->graphics_pipeline.pMultisampleState = &dst->multisample_state;
	dst->graphics_pipeline.pDepthStencilState = &dst->depth_stencil_state;
	dst->graphics_pipeline.pColorBlendState = &dst->color_blend_state;
	dst->graphics_pipeline.pDynamicState = &dst->dynamic_state;
}

static void R_SetPipelineRenderPassVariant (pipeline_create_infos_t *infos, subpass_type_t stage, main_render_pass_variant_t variant)
{
	const render_pass_binding_t *binding = R_RenderPassBinding (stage, variant);
	infos->graphics_pipeline.renderPass = binding->render_pass[MAIN_RENDER_PASS_STENCIL_CLEAR];
	infos->graphics_pipeline.subpass = binding->subpass;
}

/* Slot-private host-coherent viewport data and secondary pool for the isolated
 * receiver experiment. The live slot's fence owns both until shutdown. */
typedef struct {
	VkBuffer buffer; VkDeviceMemory memory; void *mapped; VkCommandPool pool; VkSampler sampler;
	rc_receiver_diagnostics_t diagnostics;
	VkImageView diagnostic_views[RC_RECEIVER_TARGET_COUNT];
} rc_experimental_aux_t;
static rc_experimental_aux_t rc_experimental_aux[RC_RECEIVER_LIVE_SLOTS];

void R_ReceiverExperimentalDiagnosticsCollect(uint32_t slot)
{
	if(slot>=RC_RECEIVER_LIVE_SLOTS)return;
	rc_receiver_diagnostic_stats_t stats;
	if(RCReceiverDiagnostics_Collect(&rc_experimental_aux[slot].diagnostics,&stats))
		R_RadianceSetReceiverDiagnostics(&stats);
}

bool R_ReceiverExperimentalDiagnosticsSubmitted(uint32_t slot,VkFence fence)
{
	if(slot>=RC_RECEIVER_LIVE_SLOTS)return false;
	rc_receiver_diagnostics_t *d=&rc_experimental_aux[slot].diagnostics;
	return !d->recorded || RCReceiverDiagnostics_Submitted(d,fence);
}

void R_ReceiverExperimentalDiagnosticsRecord(uint32_t slot,VkCommandBuffer command,uint64_t serial)
{
	if(slot>=RC_RECEIVER_LIVE_SLOTS || r_radiance_receiver_diagnostics.value<=0 ||
		RCReceiverFrame_ActiveFilterMode(serial)!=1)return;
	rc_experimental_aux_t *a=&rc_experimental_aux[slot];
	rc_receiver_slot_t *s=RCReceiverLive_Get(slot);
	const rc_receiver_params_t *params=RCReceiverFrame_ActiveParams(serial);
	if(!s || !params || !a->diagnostics.device)return;
	/* Failures in this optional estimate never disable receiver lighting. */
	if(!RCReceiverDiagnostics_Record(&a->diagnostics,command,serial,params,
		R_RadianceActiveProbeSpacing(),RCReceiverFrame_ActivePostprocess(serial),true))
		Con_DPrintf("Receiver coverage diagnostic skipped\n");
}

void R_ReceiverExperimentalShutdown(void)
{
	bool had_aux=false;
	for (unsigned i=0;i<RC_RECEIVER_LIVE_SLOTS;++i) {
		if (rc_experimental_aux[i].pool || rc_experimental_aux[i].buffer) had_aux=true;
		rc_experimental_aux_t *a=&rc_experimental_aux[i];
		R_ReceiverExperimentalDiagnosticsCollect(i);
		if(a->diagnostics.recorded && !a->diagnostics.fence)RCReceiverDiagnostics_Discard(&a->diagnostics);
		if(!RCReceiverDiagnostics_Destroy(&a->diagnostics))Sys_Error("Receiver diagnostic still in flight during shutdown");
		if (a->pool) vkDestroyCommandPool(vulkan_globals.device,a->pool,NULL);
		if (a->sampler) vkDestroySampler(vulkan_globals.device,a->sampler,NULL);
		if (a->mapped) vkUnmapMemory(vulkan_globals.device,a->memory);
		if (a->buffer) vkDestroyBuffer(vulkan_globals.device,a->buffer,NULL);
		if (a->memory) vkFreeMemory(vulkan_globals.device,a->memory,NULL);
		memset(a,0,sizeof(*a));
	}
	if(had_aux) Con_Printf("RC experimental auxiliary resources released\n");
}

static bool R_ReceiverExperimentalAux(uint32_t slot)
{
	rc_experimental_aux_t *a=&rc_experimental_aux[slot];
	if (a->pool && a->mapped && a->sampler) return true;
	VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=32,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
	if (vkCreateBuffer(vulkan_globals.device,&bi,NULL,&a->buffer)!=VK_SUCCESS) return false;
	VkMemoryRequirements req; vkGetBufferMemoryRequirements(vulkan_globals.device,a->buffer,&req);
	const VkMemoryPropertyFlags wanted=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type=0;
	while(type<vulkan_globals.memory_properties.memoryTypeCount && (!(req.memoryTypeBits & (1u<<type)) ||
		(vulkan_globals.memory_properties.memoryTypes[type].propertyFlags & wanted)!=wanted)) ++type;
	if(type==vulkan_globals.memory_properties.memoryTypeCount) goto fail;
	VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
	if(vkAllocateMemory(vulkan_globals.device,&ai,NULL,&a->memory)!=VK_SUCCESS ||
		vkBindBufferMemory(vulkan_globals.device,a->buffer,a->memory,0)!=VK_SUCCESS ||
		vkMapMemory(vulkan_globals.device,a->memory,0,VK_WHOLE_SIZE,0,&a->mapped)!=VK_SUCCESS) goto fail;
	VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=vulkan_globals.gfx_queue_family_index};
	if(vkCreateCommandPool(vulkan_globals.device,&pi,NULL,&a->pool)!=VK_SUCCESS) goto fail;
	VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,
		.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0.0f};
	if(vkCreateSampler(vulkan_globals.device,&si,NULL,&a->sampler)!=VK_SUCCESS) goto fail;
	return true;
fail:
	if(a->pool) vkDestroyCommandPool(vulkan_globals.device,a->pool,NULL);
	if(a->sampler) vkDestroySampler(vulkan_globals.device,a->sampler,NULL);
	if(a->mapped) vkUnmapMemory(vulkan_globals.device,a->memory);
	if(a->buffer) vkDestroyBuffer(vulkan_globals.device,a->buffer,NULL);
	if(a->memory) vkFreeMemory(vulkan_globals.device,a->memory,NULL);
	memset(a,0,sizeof(*a)); return false;
}

bool R_ReceiverExperimentalPrepare(uint32_t slot,VkRenderPass pass,uint32_t width,uint32_t height,
	const rc_experimental_bindings_t *bindings,const int32_t viewport[4],float divisor,bool texture_filter,bool soft_shadows,VkCommandPool *pool)
{
	if(slot>=RC_RECEIVER_LIVE_SLOTS || !pass || !bindings || !viewport || !pool ||
		!RCReceiver_ValidDivisor(divisor) || (divisor <= 1 && r_radiance_deferred.value <= 0) || !vulkan_globals.device_features.geometryShader ||
		vulkan_globals.sample_count!=VK_SAMPLE_COUNT_1_BIT) {
		static bool reported; if(!reported) { Con_Printf("RC experimental provider prerequisites: geometry=%d samples=%d\n",vulkan_globals.device_features.geometryShader,(int)vulkan_globals.sample_count); reported=true; } return false;
	}
	if(!R_ReceiverExperimentalAux(slot)) { static bool reported; if(!reported){Con_Printf("RC experimental auxiliary allocation failed\n");reported=true;} return false; }
	rc_experimental_aux_t *a=&rc_experimental_aux[slot];
	struct { int32_t viewport[4]; float threshold[4]; } data={{viewport[0],viewport[1],viewport[2],viewport[3]},{.99f,.01f,divisor,0}};
	memcpy(a->mapped,&data,sizeof(data));
	rc_receiver_slot_config_t c={.physical=GL_RadiancePhysicalDevice(),.device=vulkan_globals.device,.memory=&vulkan_globals.memory_properties,
		.revision=(uint64_t)(uintptr_t)pass ^ ((uint64_t)(uintptr_t)bindings->graphics<<1) ^ (bindings->hardware ? UINT64_C(1)<<63 : 0) ^
			(texture_filter ? UINT64_C(1)<<62 : 0),
		.width=width,.height=height,.samples=VK_SAMPLE_COUNT_1_BIT,.hardware=bindings->hardware,
		.texture_filter=texture_filter,
		.soft_shadows=soft_shadows,
		.geometry_enabled=true,.ray_query_enabled=vulkan_globals.ray_query,.nearest=a->sampler,
		.viewport_buffer=a->buffer,.viewport_range=32,.radiance=bindings->graphics,.cascade=bindings->cascade,.light=bindings->lighting,.cache=R_PipelineCache()};
	if (!c.revision) c.revision=1;
	for(int i=0;i<3;++i)c.world[i]=vulkan_globals.single_texture_set_layout.handle;
	c.world[3]=vulkan_globals.mboit_input_attachment_set_layout.handle;
	c.world[4]=vulkan_globals.bmodel_instances_set_layout.handle;
	pipeline_create_infos_t state[6]; VkSpecializationInfo spec[6]; uint32_t values[6][5];
	VkSpecializationMapEntry entries[5];
	for(unsigned j=0;j<5;++j) entries[j]=(VkSpecializationMapEntry){j,j*4,4};
	for(unsigned i=0;i<6;++i) {
		R_InitDefaultStates(&state[i]);
		state[i].depth_stencil_state.depthTestEnable=VK_TRUE; state[i].depth_stencil_state.depthWriteEnable=VK_TRUE;
		if(i>=2 && r_ssao.value>0) {
			state[i].depth_stencil_state.stencilTestEnable=VK_TRUE;
			state[i].depth_stencil_state.front=(VkStencilOpState){VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,2,2,2};
			state[i].depth_stencil_state.back=state[i].depth_stencil_state.front;
		}
		state[i].rasterization_state.depthBiasEnable=VK_TRUE;
		state[i].dynamic_states[state[i].dynamic_state.dynamicStateCount++]=VK_DYNAMIC_STATE_DEPTH_BIAS;
		state[i].vertex_input_state.vertexAttributeDescriptionCount=3;
		state[i].vertex_input_state.pVertexAttributeDescriptions=world_vertex_input_attribute_descriptions;
		state[i].vertex_input_state.vertexBindingDescriptionCount=1;
		state[i].vertex_input_state.pVertexBindingDescriptions=&world_vertex_binding_description;
		state[i].graphics_pipeline.renderPass=pass; state[i].graphics_pipeline.subpass=0;
		memset(values[i],0,sizeof(values[i]));
		if(i<2) { values[i][1]=i; state[i].color_blend_state.attachmentCount=2;
			state[i].blend_attachment_states[1].colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; }
		else { unsigned variant=i-2; values[i][0]=variant&1; values[i][1]=(variant>>1)&1;
			values[i][4]=vulkan_globals.color_format==VK_FORMAT_A2B10G10R10_UNORM_PACK32; }
		spec[i]=(VkSpecializationInfo){5,entries,sizeof(values[i]),values[i]}; state[i].shader_stages[1].pSpecializationInfo=&spec[i];
		if(i<2)c.prepass[i]=state[i].graphics_pipeline; else c.forward[i-2]=state[i].graphics_pipeline;
	}
	c.classify=(VkComputePipelineCreateInfo){.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.pName="main"}};
	c.direct=c.classify;
	bool was_ready=RCReceiverLive_Get(slot)->ready;
	if(!RCReceiverLive_Prepare(slot,&c)){static bool reported;if(!reported){Con_Printf("RC experimental live slot creation failed\n");reported=true;}return false;}
	if(r_radiance_receiver_diagnostics.value>0 && c.texture_filter) {
		const rc_receiver_targets_t *targets=&RCReceiverLive_Get(slot)->resources.targets;
		bool changed=!a->diagnostics.device;
		for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i)changed|=a->diagnostic_views[i]!=targets->images[i].view;
		if(changed) {
			if(RCReceiverDiagnostics_Destroy(&a->diagnostics) &&
				RCReceiverDiagnostics_Create(&a->diagnostics,vulkan_globals.device,&vulkan_globals.memory_properties,
					targets,a->sampler,R_PipelineCache())) {
				for(unsigned i=0;i<RC_RECEIVER_TARGET_COUNT;++i)a->diagnostic_views[i]=targets->images[i].view;
				Con_DPrintf("Receiver coverage diagnostic ready: slot=%u\n",slot);
			} else {
				static unsigned failures;
				if(failures++<4)Con_Printf("Receiver coverage diagnostic resources unavailable: slot=%u\n",slot);
			}
		}
	}
	if(!was_ready) Con_Printf("RC experimental provider ready: slot=%u\n",slot);
	*pool=a->pool; return true;
}

bool R_ReceiverExperimentalModelsPrepare(uint32_t slot,VkRenderPass pass,uint32_t subpass)
{
	rc_receiver_slot_t *s=slot<RC_RECEIVER_LIVE_SLOTS?RCReceiverLive_Get(slot):NULL;
	if(!s || !s->ready || !s->texture_filter || !pass || s->last_fence ||
		vulkan_globals.sample_count!=VK_SAMPLE_COUNT_1_BIT)return false;
	rc_receiver_model_pipelines_t *models=&s->resources.models;
	uint64_t revision=(uint64_t)(uintptr_t)pass ^ ((uint64_t)subpass<<48) ^ ((uint64_t)s->hardware<<63);
	if(!revision)revision=1;
	if(models->attempted_revision==revision)return models->device && models->forward_pass==pass && models->forward_subpass==subpass;
	models->attempted_revision=revision;
	rc_experimental_bindings_t bindings;
	if(!R_RadianceExperimentalBindings(slot,&bindings))return false;
	VkShaderModule vertex[3]={0},prepass[2]={0},forward[2]={0};
	bool ready=false;
#define MODEL_MODULE(dst,name) do { dst=R_TryCreateShaderModule(name##_spv,name##_spv_size,#name);if(!dst)goto cleanup; } while(0)
	MODEL_MODULE(vertex[0],alias_radiance_vert);MODEL_MODULE(vertex[1],md5_radiance_vert);MODEL_MODULE(vertex[2],md5_8_radiance_vert);
	MODEL_MODULE(prepass[0],radiance_receiver_model_frag);MODEL_MODULE(prepass[1],radiance_receiver_model_alphatest_frag);
	if(s->hardware) {
		MODEL_MODULE(forward[0],alias_receiver_forward_fast_rt_frag);MODEL_MODULE(forward[1],alias_receiver_forward_fast_rt_alphatest_frag);
	} else {
		MODEL_MODULE(forward[0],alias_receiver_forward_fast_sw_frag);MODEL_MODULE(forward[1],alias_receiver_forward_fast_sw_alphatest_frag);
	}
#undef MODEL_MODULE
	pipeline_create_infos_t state[3][2][2];VkGraphicsPipelineCreateInfo pre[3][2],fwd[3][2];
	for(unsigned kind=0;kind<3;++kind)for(unsigned cutout=0;cutout<2;++cutout)for(unsigned stage=0;stage<2;++stage) {
		pipeline_create_infos_t *info=&state[kind][cutout][stage];R_InitDefaultStates(info);
		info->depth_stencil_state.depthTestEnable=VK_TRUE;info->depth_stencil_state.depthWriteEnable=VK_TRUE;
		/* Raw receiver pipeline creation must retain the same opaque SSAO
		 * coverage bit as R_TryCreateGraphicsPipeline. The private prepass
		 * has its own depth-only attachment and does not write this mask. */
		if(stage && r_ssao.value>0) {
			info->depth_stencil_state.stencilTestEnable=VK_TRUE;
			info->depth_stencil_state.front=(VkStencilOpState){VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,2,2,2};
			info->depth_stencil_state.back=info->depth_stencil_state.front;
		}
		info->multisample_state.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
		info->color_blend_state.attachmentCount=stage?1:2;
		if(!stage)info->blend_attachment_states[1].colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
		info->vertex_input_state.vertexAttributeDescriptionCount=kind?kind==1?countof(md5_vertex_input_attribute_descriptions):countof(md5_8_vertex_input_attribute_descriptions):countof(alias_vertex_input_attribute_descriptions);
		info->vertex_input_state.pVertexAttributeDescriptions=kind?kind==1?md5_vertex_input_attribute_descriptions:md5_8_vertex_input_attribute_descriptions:alias_vertex_input_attribute_descriptions;
		info->vertex_input_state.vertexBindingDescriptionCount=kind?1:3;
		info->vertex_input_state.pVertexBindingDescriptions=kind?kind==1?&md5_vertex_binding_description:&md5_8_vertex_binding_description:alias_vertex_binding_descriptions;
		info->shader_stages[0].module=vertex[kind];info->shader_stages[1].module=stage?forward[cutout]:prepass[cutout];
		if(stage)fwd[kind][cutout]=info->graphics_pipeline;else pre[kind][cutout]=info->graphics_pipeline;
	}
	const VkDescriptorSetLayout sets[2][5]={
		{vulkan_globals.single_texture_set_layout.handle,vulkan_globals.single_texture_set_layout.handle,vulkan_globals.ubo_set_layout.handle,
		 vulkan_globals.mboit_input_attachment_set_layout.handle,VK_NULL_HANDLE},
		{vulkan_globals.single_texture_set_layout.handle,vulkan_globals.single_texture_set_layout.handle,vulkan_globals.ubo_set_layout.handle,
		 vulkan_globals.joints_buffer_set_layout.handle,vulkan_globals.mboit_input_attachment_set_layout.handle}};
	ready=RCReceiverModelPipelines_Create(models,vulkan_globals.device,R_PipelineCache(),sets,bindings.graphics,
		s->resources.descriptors.layouts[2],s->resources.targets.pass,pass,subpass,pre,fwd);
	models->attempted_revision=revision;
cleanup:
	for(unsigned i=0;i<3;++i)if(vertex[i])vkDestroyShaderModule(vulkan_globals.device,vertex[i],NULL);
	for(unsigned i=0;i<2;++i) {
		if(prepass[i])vkDestroyShaderModule(vulkan_globals.device,prepass[i],NULL);
		if(forward[i])vkDestroyShaderModule(vulkan_globals.device,forward[i],NULL);
	}
	return ready;
}

/*
===============
R_CreateGraphicsPipeline
===============
*/
typedef struct pipeline_instance_s
{
	VkPipeline					handle;
	render_pass_binding_t		binding;
	struct pipeline_instance_s *next;
	struct pipeline_instance_s *allocation_next;
} pipeline_instance_t;

static pipeline_instance_t *pipeline_instances;

/* During scene-pipeline creation, track only the objects created by this
 * attempt. Rollback must not reset Radiance, ECS, or unrelated GPU state. */
typedef struct pipeline_transaction_s
{
	vulkan_pipeline_t **created;
	size_t count, capacity;
} pipeline_transaction_t;
static pipeline_transaction_t *active_pipeline_transaction;
static bool renderer_pipelines_created;

static bool R_PipelineTransactionReserve (void)
{
	pipeline_transaction_t *transaction = active_pipeline_transaction;
	if (!transaction || transaction->count < transaction->capacity) return true;
	if (transaction->capacity > SIZE_MAX / (2 * sizeof (*transaction->created))) return false;
	size_t capacity = transaction->capacity ? transaction->capacity * 2 : 128;
	vulkan_pipeline_t **created = realloc (transaction->created, capacity * sizeof (*created));
	if (!created) return false;
	transaction->created = created;
	transaction->capacity = capacity;
	return true;
}

static void R_PipelineTransactionRecord (vulkan_pipeline_t *pipeline)
{
	if (active_pipeline_transaction)
		active_pipeline_transaction->created[active_pipeline_transaction->count++] = pipeline;
}

static void R_PipelineTransactionRollback (pipeline_transaction_t *transaction)
{
	while (transaction->count)
	{
		vulkan_pipeline_t *pipeline = transaction->created[--transaction->count];
		while (pipeline->alternatives)
		{
			pipeline_instance_t *instance = pipeline->alternatives;
			pipeline->alternatives = instance->next;
			assert (pipeline_instances == instance);
			pipeline_instances = instance->allocation_next;
			vkDestroyPipeline (vulkan_globals.device, instance->handle, NULL);
			Mem_Free (instance);
		}
		vkDestroyPipeline (vulkan_globals.device, pipeline->handle, NULL);
		pipeline->handle = VK_NULL_HANDLE;
	}
}

/* The driver may persist its own shader cache, but it is not guaranteed to do
 * so. Keep a bounded application cache across launches and video restarts.
 * The private envelope catches partial/corrupt files and driver changes before
 * passing a cache blob to the driver. Cache misses are always nonfatal. */
#define PIPELINE_CACHE_LIMIT (64u * 1024u * 1024u)
typedef struct pipeline_cache_file_s
{
	uint32_t magic, version, vendor, device, driver;
	uint8_t uuid[VK_UUID_SIZE];
	uint64_t bytes, checksum;
} pipeline_cache_file_t;
static VkPipelineCache pipeline_cache;
static bool pipeline_cache_attempted;
static char pipeline_cache_path[MAX_OSPATH];

static uint64_t R_PipelineCacheHash (const void *data, size_t bytes)
{
	const unsigned char *p = data;
	uint64_t hash = UINT64_C (1469598103934665603);
	while (bytes--) { hash ^= *p++; hash *= UINT64_C (1099511628211); }
	return hash;
}

VkPipelineCache R_PipelineCache (void)
{
	if (pipeline_cache_attempted) return pipeline_cache;
	pipeline_cache_attempted = true;
	const VkPhysicalDeviceProperties *properties = &vulkan_globals.device_properties;
	const char *root = COM_GetWriteRoot ();
	if (strlen (root) + sizeof ("/vulkan-pipeline-cache.bin") + 32 < sizeof (pipeline_cache_path))
		q_snprintf (pipeline_cache_path, sizeof (pipeline_cache_path), "%s/vulkan-pipeline-cache.bin", root);
	else pipeline_cache_path[0] = '\0';
	FILE *file = pipeline_cache_path[0] ? Sys_fopen (pipeline_cache_path, "rb") : NULL;
	pipeline_cache_file_t header;
	void *data = NULL;
	size_t size = 0;
	if (file)
	{
		if (fread (&header, sizeof (header), 1, file) == 1 &&
			header.magic == 0x51565043u && header.version == 1 && header.vendor == properties->vendorID &&
			header.device == properties->deviceID && header.driver == properties->driverVersion &&
			!memcmp (header.uuid, properties->pipelineCacheUUID, VK_UUID_SIZE) &&
			header.bytes >= sizeof (VkPipelineCacheHeaderVersionOne) && header.bytes <= PIPELINE_CACHE_LIMIT &&
			(data = malloc ((size_t)header.bytes)) != NULL)
		{
			VkPipelineCacheHeaderVersionOne vkheader;
			size = (size_t)header.bytes;
			if (fread (data, 1, size, file) != size || fgetc (file) != EOF ||
				R_PipelineCacheHash (data, size) != header.checksum)
				size = 0;
			else
			{
				memcpy (&vkheader, data, sizeof (vkheader));
				if (vkheader.headerSize < sizeof (vkheader) || vkheader.headerSize > size ||
					vkheader.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
					vkheader.vendorID != properties->vendorID || vkheader.deviceID != properties->deviceID ||
					memcmp (vkheader.pipelineCacheUUID, properties->pipelineCacheUUID, VK_UUID_SIZE)) size = 0;
			}
		}
		fclose (file);
	}
	VkPipelineCacheCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		.initialDataSize = size, .pInitialData = size ? data : NULL};
	VkResult result = vkCreatePipelineCache (vulkan_globals.device, &info, NULL, &pipeline_cache);
	if (result != VK_SUCCESS && size)
	{
		info.initialDataSize = 0; info.pInitialData = NULL;
		result = vkCreatePipelineCache (vulkan_globals.device, &info, NULL, &pipeline_cache);
	}
	free (data);
	if (result != VK_SUCCESS) pipeline_cache = VK_NULL_HANDLE;
	else if (size) Sys_Printf ("Loaded Vulkan pipeline cache (%zu bytes)\n", size);
	return pipeline_cache;
}

void R_PipelineCacheShutdown (void)
{
	if (!pipeline_cache) { pipeline_cache_attempted = false; return; }
	size_t size = 0;
	if (pipeline_cache_path[0] && vkGetPipelineCacheData (vulkan_globals.device, pipeline_cache, &size, NULL) == VK_SUCCESS &&
		size >= sizeof (VkPipelineCacheHeaderVersionOne) && size <= PIPELINE_CACHE_LIMIT)
	{
		void *data = malloc (size);
		if (data && vkGetPipelineCacheData (vulkan_globals.device, pipeline_cache, &size, data) == VK_SUCCESS)
		{
			const VkPhysicalDeviceProperties *properties = &vulkan_globals.device_properties;
			pipeline_cache_file_t header = {0};
			header.magic = 0x51565043u; header.version = 1;
			header.vendor = properties->vendorID; header.device = properties->deviceID; header.driver = properties->driverVersion;
			memcpy (header.uuid, properties->pipelineCacheUUID, VK_UUID_SIZE);
			header.bytes = size; header.checksum = R_PipelineCacheHash (data, size);
			char temp[MAX_OSPATH];
			q_snprintf (temp, sizeof (temp), "%s.tmp.%llu", pipeline_cache_path, (unsigned long long)SDL_GetPerformanceCounter ());
			FILE *file = Sys_fopen (temp, "wb");
			if (file)
			{
				bool ok = fwrite (&header, sizeof (header), 1, file) == 1 && fwrite (data, 1, size, file) == size;
				if (fclose (file)) ok = false;
				if (!ok || Sys_rename (temp, pipeline_cache_path)) Sys_remove (temp);
			}
		}
		free (data);
	}
	vkDestroyPipelineCache (vulkan_globals.device, pipeline_cache, NULL);
	pipeline_cache = VK_NULL_HANDLE;
	pipeline_cache_attempted = false;
}

static void R_PipelineCacheRetryFailed (void)
{
	if (!pipeline_cache) pipeline_cache_attempted = false;
}

VkPipeline R_ResolvePipelineInstance (const cb_context_t *cbx, vulkan_pipeline_t pipeline)
{
	for (const pipeline_instance_t *instance = pipeline.alternatives; instance; instance = instance->next)
		if (instance->binding.subpass == cbx->subpass && (instance->binding.render_pass[MAIN_RENDER_PASS_STENCIL_CLEAR] == cbx->render_pass ||
														  instance->binding.render_pass[MAIN_RENDER_PASS_NO_STENCIL] == cbx->render_pass))
			return instance->handle;
	assert (pipeline.base_subpass == cbx->subpass && R_DebugRenderPassCompatible (pipeline.base_render_pass, cbx->render_pass));
	return pipeline.handle;
}

static bool R_TryCreateGraphicsPipeline (vulkan_pipeline_t *pipeline, pipeline_create_infos_t *infos, vulkan_pipeline_layout_t layout, const char *name)
{
	assert (pipeline->handle == VK_NULL_HANDLE);
	if (!R_PipelineTransactionReserve ()) return false;
	/* A prior normal teardown freed the alternative list; a failed retry must
	 * not leave the old, now-dangling pointer attached to this pipeline. */
	pipeline->alternatives = NULL;
	infos->graphics_pipeline.layout = layout.handle;
	VkPipelineDepthStencilStateCreateInfo depth_stencil = infos->depth_stencil_state;
	if (r_ssao.value > 0 && !infos->skip_ssao_stencil && depth_stencil.depthTestEnable && depth_stencil.depthWriteEnable && !depth_stencil.stencilTestEnable)
	{
		// Bit 0 belongs to sky. Bit 1 records surviving opaque samples for SSAO.
		depth_stencil.stencilTestEnable = VK_TRUE;
		depth_stencil.front = (VkStencilOpState){VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 2, 2, 2};
		depth_stencil.back = depth_stencil.front;
	}
	infos->graphics_pipeline.pDepthStencilState = &depth_stencil;
	const VkResult err = vkCreateGraphicsPipelines (vulkan_globals.device, R_PipelineCache (), 1, &infos->graphics_pipeline, NULL, &pipeline->handle);
	if (err != VK_SUCCESS)
	{
		Con_Warning ("vkCreateGraphicsPipelines failed (%s) with code %i\n", name, (int)err);
		infos->graphics_pipeline.pDepthStencilState = &infos->depth_stencil_state;
		if (pipeline->handle != VK_NULL_HANDLE)
			vkDestroyPipeline (vulkan_globals.device, pipeline->handle, NULL);
		pipeline->handle = VK_NULL_HANDLE;
		return false;
	}
	pipeline->layout = layout;
	pipeline->alternatives = NULL;
#ifndef NDEBUG
	pipeline->base_render_pass = infos->graphics_pipeline.renderPass;
	pipeline->base_subpass = infos->graphics_pipeline.subpass;
#endif
	GL_SetObjectName ((uint64_t)pipeline->handle, VK_OBJECT_TYPE_PIPELINE, name);
	for (uint32_t i = 1;; ++i)
	{
		const render_pass_binding_t *binding = R_RenderPassAlternative (infos->graphics_pipeline.renderPass, infos->graphics_pipeline.subpass, i);
		if (!binding)
			break;
		pipeline_instance_t			*instance = Mem_Alloc (sizeof (*instance));
		if (!instance)
			goto fail;
		VkGraphicsPipelineCreateInfo create_info = infos->graphics_pipeline;
		create_info.renderPass = binding->render_pass[MAIN_RENDER_PASS_STENCIL_CLEAR];
		create_info.subpass = binding->subpass;
		const VkResult instance_err = vkCreateGraphicsPipelines (vulkan_globals.device, R_PipelineCache (), 1, &create_info, NULL, &instance->handle);
		if (instance_err != VK_SUCCESS)
		{
			Con_Warning ("vkCreateGraphicsPipelines failed (%s instance) with code %i\n", name, (int)instance_err);
			if (instance->handle != VK_NULL_HANDLE)
				vkDestroyPipeline (vulkan_globals.device, instance->handle, NULL);
			Mem_Free (instance);
			goto fail;
		}
		instance->binding = *binding;
		instance->next = pipeline->alternatives;
		pipeline->alternatives = instance;
		instance->allocation_next = pipeline_instances;
		pipeline_instances = instance;
		GL_SetObjectName ((uint64_t)instance->handle, VK_OBJECT_TYPE_PIPELINE, name);
	}
	infos->graphics_pipeline.pDepthStencilState = &infos->depth_stencil_state;
	R_PipelineTransactionRecord (pipeline);
	return true;

fail:
	while (pipeline->alternatives)
	{
		pipeline_instance_t *instance = pipeline->alternatives;
		pipeline->alternatives = instance->next;
		assert (pipeline_instances == instance);
		pipeline_instances = instance->allocation_next;
		vkDestroyPipeline (vulkan_globals.device, instance->handle, NULL);
		Mem_Free (instance);
	}
	vkDestroyPipeline (vulkan_globals.device, pipeline->handle, NULL);
	pipeline->handle = VK_NULL_HANDLE;
	infos->graphics_pipeline.pDepthStencilState = &infos->depth_stencil_state;
	return false;
}

#define R_CreateGraphicsPipeline(...) do { if (!R_TryCreateGraphicsPipeline (__VA_ARGS__)) return false; } while (0)

/*
===============
R_CreateComputePipeline

The pipeline layout must already be stored in pipeline->layout.
===============
*/
static bool R_TryCreateComputePipeline (
	vulkan_pipeline_t *pipeline, VkShaderModule module, VkPipelineShaderStageCreateFlags stage_flags, const VkSpecializationInfo *specialization_info,
	const char *name, bool fail_on_error)
{
	assert (pipeline->handle == VK_NULL_HANDLE);
	assert (pipeline->layout.handle != VK_NULL_HANDLE);
	if (!R_PipelineTransactionReserve ()) return !fail_on_error;

	ZEROED_STRUCT (VkComputePipelineCreateInfo, create_info);
	create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	create_info.stage.flags = stage_flags;
	create_info.stage.module = module;
	create_info.stage.pName = "main";
	create_info.stage.pSpecializationInfo = specialization_info;
	create_info.layout = pipeline->layout.handle;

	const VkResult err = vkCreateComputePipelines (vulkan_globals.device, R_PipelineCache (), 1, &create_info, NULL, &pipeline->handle);
	if (err != VK_SUCCESS)
	{
		pipeline->handle = VK_NULL_HANDLE;
		Sys_Printf ("vkCreateComputePipelines failed (%s) with code %i\n", name, (int)err);
		return !fail_on_error;
	}
	GL_SetObjectName ((uint64_t)pipeline->handle, VK_OBJECT_TYPE_PIPELINE, name);
	R_PipelineTransactionRecord (pipeline);
	return true;
}
#define R_CreateComputePipeline(...) do { if (!R_TryCreateComputePipeline (__VA_ARGS__)) return false; } while (0)

// Compatible instances are owned here; draw sites only name the logical pipeline.
static vulkan_pipeline_t graphics_pipelines[GRAPHICS_PIPELINE_COUNT][SUBPASS_COUNT][MAIN_RENDER_PASS_VARIANT_COUNT];

static vulkan_pipeline_t R_ResolveGraphicsPipeline (const cb_context_t *cbx, graphics_pipeline_t pipeline)
{
	assert (pipeline >= 0 && pipeline < GRAPHICS_PIPELINE_COUNT);
	assert (cbx->subpass_type >= 0 && cbx->subpass_type < SUBPASS_COUNT);
	assert (cbx->pipeline_variant >= 0 && cbx->pipeline_variant < MAIN_RENDER_PASS_VARIANT_COUNT);
	return graphics_pipelines[pipeline][cbx->subpass_type][cbx->pipeline_variant];
}

bool R_HasGraphicsPipeline (const cb_context_t *cbx, graphics_pipeline_t pipeline)
{
	return R_ResolveGraphicsPipeline (cbx, pipeline).handle != VK_NULL_HANDLE;
}

void R_BindGraphicsPipeline (cb_context_t *cbx, graphics_pipeline_t pipeline)
{
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, R_ResolveGraphicsPipeline (cbx, pipeline));
}

static bool R_CreateBasicPipelines ()
{
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	const struct
	{
		subpass_type_t			   stage;
		main_render_pass_variant_t variant;
		VkSampleCountFlagBits	   rasterization_samples;
		uint32_t				   attachment_count;
	} pipeline_variants[] = {
		{SUBPASS_MAIN, MAIN_RENDER_PASS_STANDARD, vulkan_globals.sample_count, MAIN_COLOR_ATTACHMENT_COUNT},
		{SUBPASS_UI, MAIN_RENDER_PASS_STANDARD, VK_SAMPLE_COUNT_1_BIT, 1},
		{SUBPASS_UI, MAIN_RENDER_PASS_OIT, VK_SAMPLE_COUNT_1_BIT, 1},
		{SUBPASS_UI, MAIN_RENDER_PASS_MBOIT, VK_SAMPLE_COUNT_1_BIT, 1},
		{SUBPASS_MAIN, MAIN_RENDER_PASS_OIT, vulkan_globals.sample_count, MAIN_COLOR_ATTACHMENT_COUNT},
		{SUBPASS_MAIN, MAIN_RENDER_PASS_MBOIT, vulkan_globals.sample_count, MAIN_COLOR_ATTACHMENT_COUNT},
		{SUBPASS_WBOIT, MAIN_RENDER_PASS_OIT, vulkan_globals.sample_count, WBOIT_COLOR_ATTACHMENT_COUNT},
		{SUBPASS_MBOIT_MOMENTS, MAIN_RENDER_PASS_MBOIT, vulkan_globals.sample_count, MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT},
		{SUBPASS_MBOIT_COMPOSITE, MAIN_RENDER_PASS_MBOIT, vulkan_globals.sample_count, MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT},
		{SUBPASS_OIT_RESOLVE, MAIN_RENDER_PASS_OIT, vulkan_globals.sample_count, 1},
		{SUBPASS_OIT_RESOLVE, MAIN_RENDER_PASS_MBOIT, vulkan_globals.sample_count, 1},
	};

	pipeline_create_infos_t infos;
	for (int entry = 0; entry < countof (pipeline_variants); ++entry)
	{
		const subpass_type_t			 stage = pipeline_variants[entry].stage;
		const main_render_pass_variant_t variant = pipeline_variants[entry].variant;
		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, stage, variant);
		infos.multisample_state.rasterizationSamples = pipeline_variants[entry].rasterization_samples;
		infos.color_blend_state.attachmentCount = pipeline_variants[entry].attachment_count;
		infos.shader_stages[1].module = basic_alphatest_frag_module;
		R_CreateGraphicsPipeline (
			&graphics_pipelines[PIPELINE_BASIC_ALPHATEST][stage][variant], &infos, vulkan_globals.basic_pipeline_layout, "basic_alphatest");

		infos.shader_stages[1].module = draw_pic_alphatest_frag_module;
		infos.vertex_input_state.pVertexBindingDescriptions = &draw_pic_vertex_binding_description;
		R_CreateGraphicsPipeline (&graphics_pipelines[PIPELINE_GUI][stage][variant], &infos, vulkan_globals.gui_pipeline_layout, "draw_pic_alphatest");
		infos.shader_stages[1].module = draw_pic_frag_module;
		infos.blend_attachment_states[0].blendEnable = VK_TRUE;
		R_CreateGraphicsPipeline (&graphics_pipelines[PIPELINE_GUI_BLEND][stage][variant], &infos, vulkan_globals.gui_pipeline_layout, "draw_pic");

		infos.shader_stages[0].module = draw_pic_xbr_vert_module;
		infos.shader_stages[1].module = draw_pic_xbr_alphatest_frag_module;
		infos.vertex_input_state.vertexAttributeDescriptionCount = countof (draw_pic_vertex_input_attribute_descriptions);
		infos.vertex_input_state.pVertexAttributeDescriptions = draw_pic_vertex_input_attribute_descriptions;
		infos.blend_attachment_states[0].blendEnable = VK_FALSE;
		R_CreateGraphicsPipeline (&graphics_pipelines[PIPELINE_MENU_XBR][stage][variant], &infos, vulkan_globals.gui_pipeline_layout, "draw_pic_xbr_alphatest");
		infos.shader_stages[1].module = draw_pic_xbr_frag_module;
		infos.blend_attachment_states[0].blendEnable = VK_TRUE;
		R_CreateGraphicsPipeline (&graphics_pipelines[PIPELINE_MENU_XBR_BLEND][stage][variant], &infos, vulkan_globals.gui_pipeline_layout, "draw_pic_xbr");
	}

	for (int entry = 0; entry < countof (pipeline_variants); ++entry)
	{
		const subpass_type_t			 stage = pipeline_variants[entry].stage;
		const main_render_pass_variant_t variant = pipeline_variants[entry].variant;
		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, stage, variant);
		infos.multisample_state.rasterizationSamples = pipeline_variants[entry].rasterization_samples;
		infos.color_blend_state.attachmentCount = pipeline_variants[entry].attachment_count;
		infos.shader_stages[1].module = basic_notex_frag_module;
		infos.blend_attachment_states[0].blendEnable = VK_TRUE;
		R_CreateGraphicsPipeline (
			&graphics_pipelines[PIPELINE_BASIC_NOTEX_BLEND][stage][variant], &infos, vulkan_globals.basic_pipeline_layout, "basic_notex_blend");
		if (stage == SUBPASS_UI)
		{
			infos.depth_stencil_state.depthTestEnable = VK_TRUE;
			infos.depth_stencil_state.depthWriteEnable = VK_TRUE;
			infos.skip_ssao_stencil = true;
			R_CreateGraphicsPipeline (&graphics_pipelines[PIPELINE_EDITOR_DEPTH][stage][variant], &infos, vulkan_globals.basic_pipeline_layout, "editor_depth");
			infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
			R_CreateGraphicsPipeline (&graphics_pipelines[PIPELINE_EDITOR_DEPTH_READ][stage][variant], &infos, vulkan_globals.basic_pipeline_layout, "editor_depth_read");
		}
	}

	for (int entry = 0; entry < countof (pipeline_variants); ++entry)
	{
		const subpass_type_t			 stage = pipeline_variants[entry].stage;
		const main_render_pass_variant_t variant = pipeline_variants[entry].variant;
		const qboolean					 wboit_pass = (stage == SUBPASS_WBOIT);
		const qboolean					 mboit_moment_pass = (stage == SUBPASS_MBOIT_MOMENTS);
		const qboolean					 mboit_composite_pass = (stage == SUBPASS_MBOIT_COMPOSITE);
		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, stage, variant);
		infos.multisample_state.rasterizationSamples = pipeline_variants[entry].rasterization_samples;
		infos.color_blend_state.attachmentCount = pipeline_variants[entry].attachment_count;
		infos.shader_stages[1].module =
			wboit_pass			? basic_oit_frag_module
			: mboit_moment_pass ? basic_mboit_moment_frag_module
			: mboit_composite_pass
				? ((vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT) ? basic_mboit_composite_frag_module : basic_mboit_composite_msaa_frag_module)
				: basic_frag_module;
		if (wboit_pass)
			R_SetWBOITBlend (infos.blend_attachment_states);
		else if (mboit_moment_pass)
			R_SetMBOITMomentBlend (infos.blend_attachment_states);
		else if (mboit_composite_pass)
			R_SetMBOITCompositeBlend (infos.blend_attachment_states);
		else
			infos.blend_attachment_states[0].blendEnable = VK_TRUE;
		R_CreateGraphicsPipeline (&graphics_pipelines[PIPELINE_BASIC_BLEND][stage][variant], &infos, vulkan_globals.basic_pipeline_layout, "basic_blend");
	}
	return true;
}

/*
===============
R_CreateWarpPipelines
===============
*/
static bool R_CreateWarpPipelines ()
{
	pipeline_create_infos_t infos;
	R_InitDefaultStates (&infos);

	infos.multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	infos.input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	infos.rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	infos.color_blend_state.attachmentCount = 1;
	infos.graphics_pipeline.renderPass = vulkan_globals.warp_render_pass;

	R_CreateGraphicsPipeline (&vulkan_globals.raster_tex_warp_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "warp");

	R_CreateComputePipeline (&vulkan_globals.cs_tex_warp_pipeline, cs_tex_warp_comp_module, 0, NULL, "cs_tex_warp", true);
	return true;
}

/*
===============
R_CreateParticlesPipelines
===============
*/
static bool R_CreateParticlesPipelines ()
{
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.depth_stencil_state.depthTestEnable = VK_TRUE;
	base.blend_attachment_states[0].blendEnable = VK_TRUE;

	pipeline_create_infos_t infos;
	R_CopyPipelineCreateInfos (&infos, &base);
	R_CreateGraphicsPipeline (&vulkan_globals.particle_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "particles");

	for (int variant = MAIN_RENDER_PASS_OIT; variant <= MAIN_RENDER_PASS_MBOIT; ++variant)
	{
		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_OIT_RESOLVE, variant);
		R_CreateGraphicsPipeline (
			&vulkan_globals.particle_post_oit_pipeline[variant], &infos, vulkan_globals.basic_pipeline_layout,
			variant == MAIN_RENDER_PASS_MBOIT ? "particles_post_mboit" : "particles_post_oit");
	}

	R_CopyPipelineCreateInfos (&infos, &base);
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_WBOIT, MAIN_RENDER_PASS_OIT);
	infos.color_blend_state.attachmentCount = WBOIT_COLOR_ATTACHMENT_COUNT;
	infos.shader_stages[1].module = basic_oit_frag_module;
	R_SetWBOITBlend (infos.blend_attachment_states);
	R_CreateGraphicsPipeline (&vulkan_globals.particle_oit_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "particles_oit");

	R_CopyPipelineCreateInfos (&infos, &base);
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_MOMENTS, MAIN_RENDER_PASS_MBOIT);
	infos.color_blend_state.attachmentCount = MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT;
	infos.shader_stages[1].module = basic_mboit_moment_frag_module;
	R_SetMBOITMomentBlend (infos.blend_attachment_states);
	R_CreateGraphicsPipeline (&vulkan_globals.particle_mboit_moment_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "particles_mboit_moment");

	R_CopyPipelineCreateInfos (&infos, &base);
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_COMPOSITE, MAIN_RENDER_PASS_MBOIT);
	infos.color_blend_state.attachmentCount = MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT;
	infos.shader_stages[1].module =
		(vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT) ? basic_mboit_composite_frag_module : basic_mboit_composite_msaa_frag_module;
	R_SetMBOITCompositeBlend (infos.blend_attachment_states);
	R_CreateGraphicsPipeline (&vulkan_globals.particle_mboit_composite_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "particles_mboit_composite");
	return true;
}

/*
===============
R_CreateRayDebugPipelines
===============
*/
static bool R_CreateRayDebugPipelines ()
{
#if defined(_DEBUG)
	if (!vulkan_globals.ray_query)
		return true;

	R_CreateComputePipeline (&vulkan_globals.ray_debug_pipeline, vulkan_globals.scene_hdr_active ? ray_debug_hdr_comp_module : ray_debug_comp_module, 0, NULL, "ray_debug_pipeline", true);
#endif
	return true;
}

/*
===============
R_CreateAnimComputePipelines
===============
*/
static bool R_CreateAnimComputePipelines ()
{
	if (!vulkan_globals.ray_query)
		return true;

	R_CreateComputePipeline (&vulkan_globals.mesh_interpolate_pipeline, mesh_interpolate_comp_module, 0, NULL, "mesh_interpolate_pipeline", true);
	R_CreateComputePipeline (&vulkan_globals.skinning_pipeline, skinning_comp_module, 0, NULL, "skinning_pipeline", true);
	R_CreateComputePipeline (&vulkan_globals.skinning_8_pipeline, skinning_8_comp_module, 0, NULL, "skinning_8_pipeline", true);
	return true;
}

/*
===============
R_CreateFTEParticlesPipelines
===============
*/
static void R_SetFTEParticleBlend (VkPipelineColorBlendAttachmentState *blend_attachment_state, int blend_mode)
{
	static const VkBlendFactor source_blend_factors[8] = {
		VK_BLEND_FACTOR_SRC_ALPHA, // BM_BLEND
		VK_BLEND_FACTOR_SRC_COLOR, // BM_BLENDCOLOUR
		VK_BLEND_FACTOR_SRC_ALPHA, // BM_ADDA
		VK_BLEND_FACTOR_SRC_COLOR, // BM_ADDC
		VK_BLEND_FACTOR_SRC_ALPHA, // BM_SUBTRACT
		VK_BLEND_FACTOR_ZERO,	   // BM_INVMODA
		VK_BLEND_FACTOR_ZERO,	   // BM_INVMODC
		VK_BLEND_FACTOR_ONE		   // BM_PREMUL
	};
	static const VkBlendFactor dest_blend_factors[8] = {
		VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // BM_BLEND
		VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, // BM_BLENDCOLOUR
		VK_BLEND_FACTOR_ONE,				 // BM_ADDA
		VK_BLEND_FACTOR_ONE,				 // BM_ADDC
		VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, // BM_SUBTRACT
		VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // BM_INVMODA
		VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, // BM_INVMODC
		VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA	 // BM_PREMUL
	};

	blend_attachment_state->blendEnable = VK_TRUE;
	blend_attachment_state->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blend_attachment_state->srcColorBlendFactor = source_blend_factors[blend_mode];
	blend_attachment_state->dstColorBlendFactor = dest_blend_factors[blend_mode];
	blend_attachment_state->colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment_state->srcAlphaBlendFactor = source_blend_factors[blend_mode];
	blend_attachment_state->dstAlphaBlendFactor = source_blend_factors[blend_mode];
	blend_attachment_state->alphaBlendOp = VK_BLEND_OP_ADD;
}

static bool R_CreateFTEParticlesPipelines ()
{
	static const char *fte_particle_pipeline_names[16] = {"fte_particles_blend_tris",
														  "fte_particles_blend_color_tris",
														  "fte_particles_add_color_tris",
														  "fte_particles_add_alpha_tris",
														  "fte_particles_subtract_tris",
														  "fte_particles_inv_modulate_alpha_tris",
														  "fte_particles_inv_modulate_color_tris",
														  "fte_particles_premultiplied_tris",
														  "fte_particles_blend_lines",
														  "fte_particles_blend_color_lines",
														  "fte_particles_add_color_lines",
														  "fte_particles_add_alpha_lines",
														  "fte_particles_subtract_lines",
														  "fte_particles_inv_modulate_alpha_lines",
														  "fte_particles_inv_modulate_color_lines",
														  "fte_particles_premultiplied_lines"};

	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.rasterization_state.cullMode = VK_CULL_MODE_NONE;
	base.rasterization_state.depthBiasEnable = VK_TRUE;
	base.rasterization_state.depthBiasConstantFactor = OFFSET_DECAL;
	base.rasterization_state.depthBiasSlopeFactor = 1.0f;
	base.depth_stencil_state.depthTestEnable = VK_TRUE;

	pipeline_create_infos_t mode_base, infos;
	for (int i = 0; i < 8; ++i)
	{
		const uint32_t				   specialization_data[] = {vulkan_globals.sample_count, i};
		const VkSpecializationMapEntry specialization_entries[] = {{0, 0, sizeof (uint32_t)}, {1, sizeof (uint32_t), sizeof (uint32_t)}};
		const VkSpecializationInfo	   specialization = {
				.mapEntryCount = countof (specialization_entries),
				.pMapEntries = specialization_entries,
				.dataSize = sizeof (specialization_data),
				.pData = specialization_data,
		};
		const int num_topologies = vulkan_globals.non_solid_fill ? 2 : 1;
		for (int lines = 0; lines < num_topologies; ++lines)
		{
			const int mode = i + lines * 8;

			R_CopyPipelineCreateInfos (&mode_base, &base);
			if (lines)
			{
				mode_base.input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
				mode_base.rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
			}

			for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
			{
				R_CopyPipelineCreateInfos (&infos, &mode_base);
				R_SetPipelineRenderPassVariant (&infos, SUBPASS_FTE_PARTICLES, variant);
				R_SetFTEParticleBlend (&infos.blend_attachment_states[0], i);
				infos.shader_stages[1].module =
					vulkan_globals.sample_count > VK_SAMPLE_COUNT_1_BIT ? fte_particles_msaa_frag_module : fte_particles_frag_module;
				infos.shader_stages[1].pSpecializationInfo = &specialization;
				R_CreateGraphicsPipeline (
					&vulkan_globals.fte_particle_pipelines[variant][mode], &infos, vulkan_globals.fte_particle_pipeline_layout,
					variant ? va ("%s_main_oit", fte_particle_pipeline_names[mode]) : fte_particle_pipeline_names[mode]);
			}
		}
	}
	return true;
}

/*
===============
R_CreateSpritesPipelines
===============
*/
static bool R_CreateSpritesPipelines ()
{
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.shader_stages[1].module = basic_alphatest_frag_module;
	base.depth_stencil_state.depthTestEnable = VK_TRUE;
	base.depth_stencil_state.depthWriteEnable = VK_TRUE;
	base.dynamic_states[base.dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;

	pipeline_create_infos_t infos;
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
		infos.multisample_state.sampleShadingEnable = vulkan_globals.supersampling;
		R_CreateGraphicsPipeline (
			&vulkan_globals.sprite_pipeline[variant], &infos, vulkan_globals.basic_pipeline_layout, variant ? "sprite_main_oit" : "sprite");
	}

	R_CopyPipelineCreateInfos (&infos, &base);
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_WBOIT, MAIN_RENDER_PASS_OIT);
	infos.color_blend_state.attachmentCount = WBOIT_COLOR_ATTACHMENT_COUNT;
	infos.shader_stages[1].module = basic_oit_frag_module;
	R_SetWBOITBlend (infos.blend_attachment_states);
	R_CreateGraphicsPipeline (&vulkan_globals.sprite_oit_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "sprite_oit");

	R_CopyPipelineCreateInfos (&infos, &base);
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_MOMENTS, MAIN_RENDER_PASS_MBOIT);
	infos.color_blend_state.attachmentCount = MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT;
	infos.shader_stages[1].module = basic_mboit_moment_frag_module;
	R_SetMBOITMomentBlend (infos.blend_attachment_states);
	R_CreateGraphicsPipeline (&vulkan_globals.sprite_mboit_moment_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "sprite_mboit_moment");

	R_CopyPipelineCreateInfos (&infos, &base);
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_COMPOSITE, MAIN_RENDER_PASS_MBOIT);
	infos.color_blend_state.attachmentCount = MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT;
	infos.shader_stages[1].module =
		(vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT) ? basic_mboit_composite_frag_module : basic_mboit_composite_msaa_frag_module;
	R_SetMBOITCompositeBlend (infos.blend_attachment_states);
	R_CreateGraphicsPipeline (&vulkan_globals.sprite_mboit_composite_pipeline, &infos, vulkan_globals.basic_pipeline_layout, "sprite_mboit_composite");
	return true;
}

/*
===============
R_CreateSkyPipelines
===============
*/
static bool R_CreateSkyPipelines ()
{
	for (int i = 0; i < 2; i++)
	{
		pipeline_create_infos_t base;
		R_InitDefaultStates (&base);

		if (i) // indirect vertex buffer has stride 28 (world) instead of 24 (basic)
		{
			base.vertex_input_state.vertexAttributeDescriptionCount = 3;
			base.vertex_input_state.pVertexAttributeDescriptions = world_vertex_input_attribute_descriptions;
			base.vertex_input_state.vertexBindingDescriptionCount = 1;
			base.vertex_input_state.pVertexBindingDescriptions = &world_vertex_binding_description;
		}

		base.depth_stencil_state.depthTestEnable = VK_TRUE;
		base.depth_stencil_state.depthWriteEnable = VK_TRUE;

		pipeline_create_infos_t infos;
		for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{

			const char *pass_suffix = (variant == MAIN_RENDER_PASS_MBOIT) ? "_main_mboit" : ((variant == MAIN_RENDER_PASS_OIT) ? "_main_oit" : "");

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
			infos.graphics_pipeline.stageCount = 1;
			infos.shader_stages[1].module = VK_NULL_HANDLE;
			infos.depth_stencil_state.stencilTestEnable = VK_TRUE;
			infos.depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
			infos.depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
			infos.depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_KEEP;
			infos.depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
			infos.depth_stencil_state.front.compareMask = 0xFF;
			infos.depth_stencil_state.front.writeMask = 0xFF;
			infos.depth_stencil_state.front.reference = 0x1;
			infos.blend_attachment_states[0].colorWriteMask = 0; // We only want to write stencil
			R_CreateGraphicsPipeline (
				&vulkan_globals.sky_stencil_pipeline[variant][i], &infos, vulkan_globals.sky_pipeline_layout[0],
				va (i ? "sky_stencil_indirect%s" : "sky_stencil%s", pass_suffix));

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
			infos.shader_stages[1].module = basic_notex_frag_module;
			R_CreateGraphicsPipeline (
				&vulkan_globals.sky_color_pipeline[variant][i], &infos, vulkan_globals.sky_pipeline_layout[0],
				va (i ? "sky_color_indirect%s" : "sky_color%s", pass_suffix));

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
			infos.shader_stages[0].module = sky_cube_vert_module;
			infos.vertex_input_state.vertexAttributeDescriptionCount = 1;
			infos.shader_stages[1].module = sky_cube_frag_module;
			infos.multisample_state.sampleShadingEnable = vulkan_globals.supersampling;
			R_CreateGraphicsPipeline (
				&vulkan_globals.sky_cube_pipeline[variant][i], &infos, vulkan_globals.sky_pipeline_layout[0],
				va (i ? "sky_cube_indirect%s" : "sky_cube%s", pass_suffix));

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
			infos.shader_stages[0].module = sky_layer_vert_module;
			infos.vertex_input_state.vertexAttributeDescriptionCount = 1;
			infos.shader_stages[1].module = sky_layer_frag_module;
			infos.multisample_state.sampleShadingEnable = vulkan_globals.supersampling;
			R_CreateGraphicsPipeline (
				&vulkan_globals.sky_layer_pipeline[variant][i], &infos, vulkan_globals.sky_pipeline_layout[1],
				va (i ? "sky_layer_indirect%s" : "sky_layer%s", pass_suffix));

			if (i == 0)
			{
				R_CopyPipelineCreateInfos (&infos, &base);
				R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
				infos.depth_stencil_state.depthTestEnable = VK_FALSE;
				infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
				infos.depth_stencil_state.stencilTestEnable = VK_TRUE;
				infos.depth_stencil_state.front.compareOp = VK_COMPARE_OP_EQUAL;
				infos.depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
				infos.depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_KEEP;
				infos.depth_stencil_state.front.passOp = VK_STENCIL_OP_KEEP;
				infos.depth_stencil_state.front.compareMask = 0xFF;
				infos.depth_stencil_state.front.writeMask = 0x0;
				infos.depth_stencil_state.front.reference = 0x1;
				infos.shader_stages[1].module = sky_box_frag_module;
				infos.multisample_state.sampleShadingEnable = vulkan_globals.supersampling;
				R_CreateGraphicsPipeline (
					&vulkan_globals.sky_box_pipeline[variant], &infos, vulkan_globals.sky_pipeline_layout[0], va ("sky_box%s", pass_suffix));
			}
		}
	}
	return true;
}

/*
===============
R_CreateShowTrisPipelines
===============
*/
static bool R_CreateShowTrisPipelines ()
{
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.vertex_input_state.vertexAttributeDescriptionCount = countof (basic_vertex_input_attribute_descriptions);
	base.vertex_input_state.pVertexAttributeDescriptions = basic_vertex_input_attribute_descriptions;
	base.vertex_input_state.vertexBindingDescriptionCount = 1;
	base.vertex_input_state.pVertexBindingDescriptions = &basic_vertex_binding_description;
	base.input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	base.shader_stages[0].module = basic_vert_module;
	base.shader_stages[1].module = basic_notex_frag_module;

	pipeline_create_infos_t infos;
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{

		const char *pass_suffix = (variant == MAIN_RENDER_PASS_MBOIT) ? "_main_mboit" : ((variant == MAIN_RENDER_PASS_OIT) ? "_main_oit" : "");

		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
		infos.blend_attachment_states[0].blendEnable = VK_TRUE;
		R_CreateGraphicsPipeline (
			&vulkan_globals.debug_lines_pipeline[variant], &infos, vulkan_globals.basic_pipeline_layout, va ("debug_lines%s", pass_suffix));

		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
		infos.vertex_input_state.vertexAttributeDescriptionCount = 0;
		infos.vertex_input_state.pVertexAttributeDescriptions = NULL;
		infos.vertex_input_state.vertexBindingDescriptionCount = 0;
		infos.vertex_input_state.pVertexBindingDescriptions = NULL;
		infos.shader_stages[0].module = md5_debug_vert_module;
		infos.blend_attachment_states[0].blendEnable = VK_TRUE;
		R_CreateGraphicsPipeline (
			&vulkan_globals.md5_debug_pipeline[variant], &infos, vulkan_globals.md5_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout,
			va ("md5_debug%s", pass_suffix));
	}

	if (!vulkan_globals.non_solid_fill)
		return true;

	base.rasterization_state.cullMode = VK_CULL_MODE_NONE;
	base.rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
	base.input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkVertexInputAttributeDescription showtris_vertex_input_attribute_descriptions;
	showtris_vertex_input_attribute_descriptions.binding = 0;
	showtris_vertex_input_attribute_descriptions.format = VK_FORMAT_R32G32B32_SFLOAT;
	showtris_vertex_input_attribute_descriptions.location = 0;
	showtris_vertex_input_attribute_descriptions.offset = 0;

	VkVertexInputBindingDescription showtris_vertex_binding_description;
	showtris_vertex_binding_description.binding = 0;
	showtris_vertex_binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	showtris_vertex_binding_description.stride = 24;

	base.vertex_input_state.vertexAttributeDescriptionCount = 1;
	base.vertex_input_state.pVertexAttributeDescriptions = &showtris_vertex_input_attribute_descriptions;
	base.vertex_input_state.vertexBindingDescriptionCount = 1;
	base.vertex_input_state.pVertexBindingDescriptions = &showtris_vertex_binding_description;

	base.shader_stages[0].module = showtris_vert_module;
	base.shader_stages[1].module = showtris_frag_module;

	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{

		const char *pass_suffix = (variant == MAIN_RENDER_PASS_MBOIT) ? "_main_mboit" : ((variant == MAIN_RENDER_PASS_OIT) ? "_main_oit" : "");

		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
		R_CreateGraphicsPipeline (&vulkan_globals.showtris_pipeline[variant], &infos, vulkan_globals.basic_pipeline_layout, va ("showtris%s", pass_suffix));

		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
		infos.depth_stencil_state.depthTestEnable = VK_TRUE;
		infos.rasterization_state.depthBiasEnable = VK_TRUE;
		infos.rasterization_state.depthBiasConstantFactor = 500.0f;
		infos.rasterization_state.depthBiasSlopeFactor = 0.0f;
		R_CreateGraphicsPipeline (
			&vulkan_globals.showtris_depth_test_pipeline[variant], &infos, vulkan_globals.basic_pipeline_layout, va ("showtris_depth_test%s", pass_suffix));

		// indirect showtris uses the world vertex shader so moved submodels get their instance transform applied
		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
		infos.shader_stages[0].module = world_vert_module;
		infos.vertex_input_state.vertexAttributeDescriptionCount = 3;
		infos.vertex_input_state.pVertexAttributeDescriptions = world_vertex_input_attribute_descriptions;
		infos.vertex_input_state.vertexBindingDescriptionCount = 1;
		infos.vertex_input_state.pVertexBindingDescriptions = &world_vertex_binding_description;
		R_CreateGraphicsPipeline (
			&vulkan_globals.showtris_indirect_pipeline[variant], &infos, vulkan_globals.world_pipeline_layout, va ("showtris_indirect%s", pass_suffix));

		R_CopyPipelineCreateInfos (&infos, &base);
		R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
		infos.shader_stages[0].module = world_vert_module;
		infos.vertex_input_state.vertexAttributeDescriptionCount = 3;
		infos.vertex_input_state.pVertexAttributeDescriptions = world_vertex_input_attribute_descriptions;
		infos.vertex_input_state.vertexBindingDescriptionCount = 1;
		infos.vertex_input_state.pVertexBindingDescriptions = &world_vertex_binding_description;
		infos.depth_stencil_state.depthTestEnable = VK_TRUE;
		infos.rasterization_state.depthBiasEnable = VK_TRUE;
		infos.rasterization_state.depthBiasConstantFactor = 500.0f;
		infos.rasterization_state.depthBiasSlopeFactor = 0.0f;
		R_CreateGraphicsPipeline (
			&vulkan_globals.showtris_indirect_depth_test_pipeline[variant], &infos, vulkan_globals.world_pipeline_layout,
			va ("showtris_indirect_depth_test%s", pass_suffix));
	}
	return true;
}

/*
===============
R_CreateWorldPipelines
===============
*/
static bool R_CreateWorldPipelines ()
{
#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
	bool mesh_ready = vulkan_globals.mesh_shader && vulkan_globals.world_mesh_pipeline_layout.handle && world_mesh_mesh_module;
#if defined(HAVE_WORLD_TASK_SHADER)
	bool task_ready = vulkan_globals.task_shader && vulkan_globals.world_task_pipeline_layout.handle && world_task_task_module && world_task_mesh_mesh_module;
#endif
#endif
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.depth_stencil_state.depthTestEnable = VK_TRUE;
	base.depth_stencil_state.depthWriteEnable = VK_TRUE;
	base.rasterization_state.depthBiasEnable = VK_TRUE;
	base.dynamic_states[base.dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;

	base.vertex_input_state.vertexAttributeDescriptionCount = 3;
	base.vertex_input_state.pVertexAttributeDescriptions = world_vertex_input_attribute_descriptions;
	base.vertex_input_state.vertexBindingDescriptionCount = 1;
	base.vertex_input_state.pVertexBindingDescriptions = &world_vertex_binding_description;

	VkSpecializationMapEntry specialization_entries[5];
	specialization_entries[0].constantID = 0;
	specialization_entries[0].offset = 0;
	specialization_entries[0].size = 4;
	specialization_entries[1].constantID = 1;
	specialization_entries[1].offset = 4;
	specialization_entries[1].size = 4;
	specialization_entries[2].constantID = 2;
	specialization_entries[2].offset = 8;
	specialization_entries[2].size = 4;
	specialization_entries[3].constantID = 3;
	specialization_entries[3].offset = 12;
	specialization_entries[3].size = 4;
	specialization_entries[4].constantID = 4;
	specialization_entries[4].offset = 16;
	specialization_entries[4].size = 4;
	uint32_t specialization_data[5];
	specialization_data[0] = 0;
	specialization_data[1] = 0;
	specialization_data[2] = 0;
	specialization_data[3] = 0;
	specialization_data[4] = vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32; // 10-bit lightmap

	VkSpecializationInfo specialization_info;
	specialization_info.mapEntryCount = countof (specialization_entries);
	specialization_info.pMapEntries = specialization_entries;
	specialization_info.dataSize = sizeof (specialization_data);
	specialization_info.pData = specialization_data;

	base.shader_stages[0].module = world_vert_module;
	base.shader_stages[1].pSpecializationInfo = &specialization_info;

	pipeline_create_infos_t infos;
	for (int alpha_blend = 0; alpha_blend < 2; ++alpha_blend)
	{
		for (int alpha_test = 0; alpha_test < 2; ++alpha_test)
		{
			for (int fullbright_enabled = 0; fullbright_enabled < 2; ++fullbright_enabled)
			{
				for (int quantize_lm = 0; quantize_lm < 2; ++quantize_lm)
				{
					const int pipeline_index = fullbright_enabled + (alpha_test * 2) + (alpha_blend * 4) + (quantize_lm * 8);

					specialization_data[0] = fullbright_enabled;
					specialization_data[1] = alpha_test;
					specialization_data[2] = alpha_blend;
					specialization_data[3] = quantize_lm;

					for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
					{
						R_CopyPipelineCreateInfos (&infos, &base);
						R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
						infos.shader_stages[1].module = world_frag_module;
						infos.blend_attachment_states[0].blendEnable = alpha_blend ? VK_TRUE : VK_FALSE;
						infos.multisample_state.sampleShadingEnable = (!alpha_blend && vulkan_globals.supersampling) ? VK_TRUE : VK_FALSE;
						infos.depth_stencil_state.depthWriteEnable = alpha_blend ? VK_FALSE : VK_TRUE;
						R_CreateGraphicsPipeline (
							&vulkan_globals.world_pipelines[variant][pipeline_index], &infos, vulkan_globals.world_pipeline_layout,
							va (variant ? "world_main_oit %d" : "world %d", pipeline_index));

#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
						if (mesh_ready && !alpha_blend && !alpha_test)
						{
							R_CopyPipelineCreateInfos (&infos, &base);
							R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
							infos.shader_stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
							infos.shader_stages[0].module = world_mesh_mesh_module;
							infos.shader_stages[1].module = world_frag_module;
							infos.graphics_pipeline.pVertexInputState = NULL;
							infos.graphics_pipeline.pInputAssemblyState = NULL;
							infos.multisample_state.sampleShadingEnable = vulkan_globals.supersampling;
							if (!R_TryCreateGraphicsPipeline (
									&vulkan_globals.world_mesh_pipelines[variant][pipeline_index], &infos, vulkan_globals.world_mesh_pipeline_layout,
									va (variant ? "world_mesh_main_oit %d" : "world_mesh %d", pipeline_index)))
							{
								Con_Warning ("Mesh world pipeline unavailable; using indexed meshlets\n");
								mesh_ready = false;
							}
						}
#if defined(HAVE_WORLD_TASK_SHADER)
						if (task_ready && mesh_ready && !alpha_blend && !alpha_test)
						{
							R_CopyPipelineCreateInfos (&infos, &base);
							R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
							infos.shader_stages[2] = infos.shader_stages[1];
							infos.shader_stages[2].module = world_frag_module;
							infos.shader_stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
							infos.shader_stages[0].module = world_task_task_module;
							infos.shader_stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
							infos.shader_stages[1].module = world_task_mesh_mesh_module;
							infos.shader_stages[1].pSpecializationInfo = NULL;
							infos.graphics_pipeline.stageCount = 3;
							infos.graphics_pipeline.pVertexInputState = NULL;
							infos.graphics_pipeline.pInputAssemblyState = NULL;
							infos.multisample_state.sampleShadingEnable = vulkan_globals.supersampling;
							if (!R_TryCreateGraphicsPipeline (
									&vulkan_globals.world_task_pipelines[variant][pipeline_index], &infos, vulkan_globals.world_task_pipeline_layout,
									va (variant ? "world_task_main_oit %d" : "world_task %d", pipeline_index)))
							{
								Con_Warning ("Task world pipeline unavailable; using mesh shaders\n");
								task_ready = false;
							}
						}
#endif
#endif
					}

					if (alpha_blend)
					{
						R_CopyPipelineCreateInfos (&infos, &base);
						R_SetPipelineRenderPassVariant (&infos, SUBPASS_WBOIT, MAIN_RENDER_PASS_OIT);
						infos.color_blend_state.attachmentCount = WBOIT_COLOR_ATTACHMENT_COUNT;
						infos.shader_stages[1].module = world_oit_frag_module;
						infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
						R_SetWBOITBlend (infos.blend_attachment_states);
						R_CreateGraphicsPipeline (
							&vulkan_globals.world_wboit_pipelines[pipeline_index], &infos, vulkan_globals.world_pipeline_layout,
							va ("world_wboit %d", pipeline_index));

						R_CopyPipelineCreateInfos (&infos, &base);
						R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_MOMENTS, MAIN_RENDER_PASS_MBOIT);
						infos.color_blend_state.attachmentCount = MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT;
						infos.shader_stages[1].module = world_mboit_moment_frag_module;
						infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
						R_SetMBOITMomentBlend (infos.blend_attachment_states);
						R_CreateGraphicsPipeline (
							&vulkan_globals.world_mboit_moment_pipelines[pipeline_index], &infos, vulkan_globals.world_pipeline_layout,
							va ("world_mboit_moment %d", pipeline_index));

						R_CopyPipelineCreateInfos (&infos, &base);
						R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_COMPOSITE, MAIN_RENDER_PASS_MBOIT);
						infos.color_blend_state.attachmentCount = MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT;
						infos.shader_stages[1].module =
							(vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT) ? world_mboit_composite_frag_module : world_mboit_composite_msaa_frag_module;
						infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
						R_SetMBOITCompositeBlend (infos.blend_attachment_states);
						R_CreateGraphicsPipeline (
							&vulkan_globals.world_mboit_composite_pipelines[pipeline_index], &infos, vulkan_globals.world_pipeline_layout,
							va ("world_mboit_composite %d", pipeline_index));
					}
				}
			}
		}
	}
#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
	vulkan_globals.world_mesh_ready = mesh_ready;
#if defined(HAVE_WORLD_TASK_SHADER)
	vulkan_globals.world_task_ready = task_ready && mesh_ready;
#endif
#endif
	return true;
}

/*
===============
R_CreateAliasPipelines
===============
*/
static bool R_CreateEditorModelPipelines (const pipeline_create_infos_t *base,
	vulkan_pipeline_layout_t layout, graphics_pipeline_t first, const char *name)
{
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		for (int cutout = 0; cutout < 2; ++cutout)
		{
			pipeline_create_infos_t infos; R_CopyPipelineCreateInfos (&infos, base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_UI, variant);
			infos.shader_stages[1].module = cutout ? alias_alphatest_frag_module : alias_frag_module;
			infos.rasterization_state.cullMode = VK_CULL_MODE_NONE;
			infos.multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			infos.multisample_state.sampleShadingEnable = VK_FALSE;
			infos.color_blend_state.attachmentCount = 1;
			infos.blend_attachment_states[0].blendEnable = VK_TRUE;
			infos.depth_stencil_state.depthTestEnable = VK_TRUE;
			infos.depth_stencil_state.depthWriteEnable = VK_TRUE;
			infos.skip_ssao_stencil = true;
			R_CreateGraphicsPipeline (&graphics_pipelines[first + cutout][SUBPASS_UI][variant], &infos, layout,
				va ("editor_%s_%d_%d", name, variant, cutout));
		}
	return true;
}

static bool R_CreateAliasPipelines ()
{
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.depth_stencil_state.depthTestEnable = VK_TRUE;
	base.depth_stencil_state.depthWriteEnable = VK_TRUE;

	base.vertex_input_state.vertexAttributeDescriptionCount = 5;
	base.vertex_input_state.pVertexAttributeDescriptions = alias_vertex_input_attribute_descriptions;
	base.vertex_input_state.vertexBindingDescriptionCount = 3;
	base.vertex_input_state.pVertexBindingDescriptions = alias_vertex_binding_descriptions;

	base.shader_stages[0].module = alias_vert_module;

	const vulkan_pipeline_layout_t layout = vulkan_globals.alias_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout;
	if (!R_CreateEditorModelPipelines (&base, layout, PIPELINE_EDITOR_ALIAS, "alias")) return false;

	/* The stress visualization contains many copies of a single opaque alias
	 * model. Keep the ordinary alias shader/layout and feed origin and lighting
	 * as a per-instance vertex stream, reducing them to one indexed draw. */
	VkVertexInputAttributeDescription instanced_attributes[7];
	VkVertexInputBindingDescription instanced_bindings[4];
	memcpy (instanced_attributes, alias_vertex_input_attribute_descriptions, sizeof (alias_vertex_input_attribute_descriptions));
	memcpy (instanced_bindings, alias_vertex_binding_descriptions, sizeof (alias_vertex_binding_descriptions));
	instanced_attributes[5] = (VkVertexInputAttributeDescription){5, 3, VK_FORMAT_R32G32B32_SFLOAT, 0};
	instanced_attributes[6] = (VkVertexInputAttributeDescription){6, 3, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof (float)};
	instanced_bindings[3] = (VkVertexInputBindingDescription){3, 6 * sizeof (float), VK_VERTEX_INPUT_RATE_INSTANCE};
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		pipeline_create_infos_t instances;
		R_CopyPipelineCreateInfos (&instances, &base);
		R_SetPipelineRenderPassVariant (&instances, SUBPASS_MAIN, variant);
		instances.vertex_input_state.vertexAttributeDescriptionCount = countof (instanced_attributes);
		instances.vertex_input_state.pVertexAttributeDescriptions = instanced_attributes;
		instances.vertex_input_state.vertexBindingDescriptionCount = countof (instanced_bindings);
		instances.vertex_input_state.pVertexBindingDescriptions = instanced_bindings;
		instances.shader_stages[0].module = alias_instanced_vert_module;
		instances.shader_stages[1].module = alias_frag_module;
		instances.multisample_state.sampleShadingEnable = vulkan_globals.supersampling ? VK_TRUE : VK_FALSE;
		R_TryCreateGraphicsPipeline (&vulkan_globals.alias_instanced_pipelines[variant], &instances, layout,
			va ("alias_instanced %d", variant));
	}

	pipeline_create_infos_t infos;
	for (int pipeline_index = 0; pipeline_index < MODEL_PIPELINE_SHOWTRIS; ++pipeline_index)
	{
		const qboolean alpha_test = pipeline_index & MODEL_PIPELINE_ALPHA_TEST_BIT;
		const qboolean alpha_blend = pipeline_index & MODEL_PIPELINE_ALPHA_BLEND_BIT;

		for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{
			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
			infos.shader_stages[1].module = alpha_test ? alias_alphatest_frag_module : alias_frag_module;
			infos.blend_attachment_states[0].blendEnable = alpha_blend ? VK_TRUE : VK_FALSE;
			infos.multisample_state.sampleShadingEnable = (!alpha_blend && vulkan_globals.supersampling) ? VK_TRUE : VK_FALSE;
			infos.depth_stencil_state.depthWriteEnable = alpha_blend ? VK_FALSE : VK_TRUE;
			R_CreateGraphicsPipeline (
				&vulkan_globals.alias_pipelines[variant][pipeline_index], &infos, layout, va (variant ? "alias_main_oit %d" : "alias %d", pipeline_index));
		}

		if (alpha_blend)
		{
			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_WBOIT, MAIN_RENDER_PASS_OIT);
			infos.color_blend_state.attachmentCount = WBOIT_COLOR_ATTACHMENT_COUNT;
			infos.shader_stages[1].module = alpha_test ? alias_alphatest_oit_frag_module : alias_oit_frag_module;
			infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
			R_SetWBOITBlend (infos.blend_attachment_states);
			R_CreateGraphicsPipeline (&vulkan_globals.alias_wboit_pipelines[pipeline_index], &infos, layout, va ("alias_wboit %d", pipeline_index));

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_MOMENTS, MAIN_RENDER_PASS_MBOIT);
			infos.color_blend_state.attachmentCount = MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT;
			infos.shader_stages[1].module = alpha_test ? alias_alphatest_mboit_moment_frag_module : alias_mboit_moment_frag_module;
			infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
			R_SetMBOITMomentBlend (infos.blend_attachment_states);
			R_CreateGraphicsPipeline (
				&vulkan_globals.alias_mboit_moment_pipelines[pipeline_index], &infos, layout, va ("alias_mboit_moment %d", pipeline_index));

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_COMPOSITE, MAIN_RENDER_PASS_MBOIT);
			infos.color_blend_state.attachmentCount = MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT;
			infos.shader_stages[1].module = (vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT)
												? (alpha_test ? alias_alphatest_mboit_composite_frag_module : alias_mboit_composite_frag_module)
												: (alpha_test ? alias_alphatest_mboit_composite_msaa_frag_module : alias_mboit_composite_msaa_frag_module);
			infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
			R_SetMBOITCompositeBlend (infos.blend_attachment_states);
			R_CreateGraphicsPipeline (
				&vulkan_globals.alias_mboit_composite_pipelines[pipeline_index], &infos, layout, va ("alias_mboit_composite %d", pipeline_index));
		}
	}

	if (vulkan_globals.non_solid_fill)
	{
		for (int pipeline_index = MODEL_PIPELINE_SHOWTRIS; pipeline_index <= MODEL_PIPELINE_SHOWTRIS_DEPTH_TEST; ++pipeline_index)
		{
			const qboolean depth_test = pipeline_index == MODEL_PIPELINE_SHOWTRIS_DEPTH_TEST;
			for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
			{
				R_CopyPipelineCreateInfos (&infos, &base);
				R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
				infos.rasterization_state.cullMode = VK_CULL_MODE_NONE;
				infos.rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
				infos.depth_stencil_state.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
				infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
				infos.rasterization_state.depthBiasEnable = depth_test ? VK_TRUE : VK_FALSE;
				infos.rasterization_state.depthBiasConstantFactor = 500.0f;
				infos.rasterization_state.depthBiasSlopeFactor = 0.0f;
				infos.shader_stages[1].module = showtris_frag_module;
				R_CreateGraphicsPipeline (
					&vulkan_globals.alias_pipelines[variant][pipeline_index], &infos, layout,
					va (variant ? "alias_showtris_main_oit %d" : "alias_showtris %d", pipeline_index));
			}
		}
	}
	return true;
}

/*
===============
R_CreateMD5Pipelines
===============
*/
static bool R_TryCreateMD5PipelineSet (
	vulkan_pipeline_t pipelines[MAIN_RENDER_PASS_VARIANT_COUNT][MODEL_PIPELINE_COUNT], vulkan_pipeline_t wboit_pipelines[MODEL_PIPELINE_COUNT],
	vulkan_pipeline_t mboit_moment_pipelines[MODEL_PIPELINE_COUNT], vulkan_pipeline_t mboit_composite_pipelines[MODEL_PIPELINE_COUNT],
	VkVertexInputAttributeDescription *vertex_attributes, uint32_t vertex_attribute_count, VkVertexInputBindingDescription *vertex_binding,
	VkShaderModule vertex_module, const char *name)
{
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.depth_stencil_state.depthTestEnable = VK_TRUE;
	base.depth_stencil_state.depthWriteEnable = VK_TRUE;

	base.vertex_input_state.vertexAttributeDescriptionCount = vertex_attribute_count;
	base.vertex_input_state.pVertexAttributeDescriptions = vertex_attributes;
	base.vertex_input_state.vertexBindingDescriptionCount = 1;
	base.vertex_input_state.pVertexBindingDescriptions = vertex_binding;

	base.shader_stages[0].module = vertex_module;

	const vulkan_pipeline_layout_t layout = vulkan_globals.md5_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout;
	if (!R_CreateEditorModelPipelines (&base, layout,
		vertex_module == md5_8_vert_module ? PIPELINE_EDITOR_MD5_8 : PIPELINE_EDITOR_MD5, name)) return false;

	pipeline_create_infos_t infos;
	for (int pipeline_index = 0; pipeline_index < MODEL_PIPELINE_SHOWTRIS; ++pipeline_index)
	{
		const qboolean alpha_test = pipeline_index & MODEL_PIPELINE_ALPHA_TEST_BIT;
		const qboolean alpha_blend = pipeline_index & MODEL_PIPELINE_ALPHA_BLEND_BIT;

		for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{
			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
			infos.shader_stages[1].module = alpha_test ? alias_alphatest_frag_module : alias_frag_module;
			infos.blend_attachment_states[0].blendEnable = alpha_blend ? VK_TRUE : VK_FALSE;
			infos.multisample_state.sampleShadingEnable = (!alpha_blend && vulkan_globals.supersampling) ? VK_TRUE : VK_FALSE;
			infos.depth_stencil_state.depthWriteEnable = alpha_blend ? VK_FALSE : VK_TRUE;
			R_CreateGraphicsPipeline (&pipelines[variant][pipeline_index], &infos, layout, va (variant ? "%s_main_oit %d" : "%s %d", name, pipeline_index));
		}

		if (alpha_blend)
		{
			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_WBOIT, MAIN_RENDER_PASS_OIT);
			infos.color_blend_state.attachmentCount = WBOIT_COLOR_ATTACHMENT_COUNT;
			infos.shader_stages[1].module = alpha_test ? alias_alphatest_oit_frag_module : alias_oit_frag_module;
			infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
			R_SetWBOITBlend (infos.blend_attachment_states);
			R_CreateGraphicsPipeline (&wboit_pipelines[pipeline_index], &infos, layout, va ("%s_wboit %d", name, pipeline_index));

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_MOMENTS, MAIN_RENDER_PASS_MBOIT);
			infos.color_blend_state.attachmentCount = MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT;
			infos.shader_stages[1].module = alpha_test ? alias_alphatest_mboit_moment_frag_module : alias_mboit_moment_frag_module;
			infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
			R_SetMBOITMomentBlend (infos.blend_attachment_states);
			R_CreateGraphicsPipeline (&mboit_moment_pipelines[pipeline_index], &infos, layout, va ("%s_mboit_moment %d", name, pipeline_index));

			R_CopyPipelineCreateInfos (&infos, &base);
			R_SetPipelineRenderPassVariant (&infos, SUBPASS_MBOIT_COMPOSITE, MAIN_RENDER_PASS_MBOIT);
			infos.color_blend_state.attachmentCount = MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT;
			// the composite shader reads the moment input attachments, which the md5 layout binds
			// at set 4 because the joints occupy set 3, so it needs its own MBOIT_INPUT_SET variant
			infos.shader_stages[1].module = (vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT)
												? (alpha_test ? md5_alphatest_mboit_composite_frag_module : md5_mboit_composite_frag_module)
												: (alpha_test ? md5_alphatest_mboit_composite_msaa_frag_module : md5_mboit_composite_msaa_frag_module);
			infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
			R_SetMBOITCompositeBlend (infos.blend_attachment_states);
			R_CreateGraphicsPipeline (&mboit_composite_pipelines[pipeline_index], &infos, layout, va ("%s_mboit_composite %d", name, pipeline_index));
		}
	}

	if (vulkan_globals.non_solid_fill)
	{
		for (int pipeline_index = MODEL_PIPELINE_SHOWTRIS; pipeline_index <= MODEL_PIPELINE_SHOWTRIS_DEPTH_TEST; ++pipeline_index)
		{
			const qboolean depth_test = pipeline_index == MODEL_PIPELINE_SHOWTRIS_DEPTH_TEST;
			for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
			{
				R_CopyPipelineCreateInfos (&infos, &base);
				R_SetPipelineRenderPassVariant (&infos, SUBPASS_MAIN, variant);
				infos.rasterization_state.cullMode = VK_CULL_MODE_NONE;
				infos.rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
				infos.depth_stencil_state.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
				infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
				infos.rasterization_state.depthBiasEnable = depth_test ? VK_TRUE : VK_FALSE;
				infos.rasterization_state.depthBiasConstantFactor = 500.0f;
				infos.rasterization_state.depthBiasSlopeFactor = 0.0f;
				infos.shader_stages[1].module = showtris_frag_module;
				R_CreateGraphicsPipeline (
					&pipelines[variant][pipeline_index], &infos, layout, va (variant ? "%s_showtris_main_oit %d" : "%s_showtris %d", name, pipeline_index));
			}
		}
	}
	return true;
}

#define R_CreateMD5PipelineSet(...) do { if (!R_TryCreateMD5PipelineSet (__VA_ARGS__)) return false; } while (0)

static bool R_CreateMD5Pipelines ()
{
	R_CreateMD5PipelineSet (
		vulkan_globals.md5_pipelines, vulkan_globals.md5_wboit_pipelines, vulkan_globals.md5_mboit_moment_pipelines,
		vulkan_globals.md5_mboit_composite_pipelines, md5_vertex_input_attribute_descriptions, countof (md5_vertex_input_attribute_descriptions),
		&md5_vertex_binding_description, md5_vert_module, "md5");
	R_CreateMD5PipelineSet (
		vulkan_globals.md5_8_pipelines, vulkan_globals.md5_8_wboit_pipelines, vulkan_globals.md5_8_mboit_moment_pipelines,
		vulkan_globals.md5_8_mboit_composite_pipelines, md5_8_vertex_input_attribute_descriptions, countof (md5_8_vertex_input_attribute_descriptions),
		&md5_8_vertex_binding_description, md5_8_vert_module, "md5_8");
	return true;
}

#include "radiance_graphics.inc"

/*
===============
R_CreatePostprocessPipelines
===============
*/
static bool R_CreatePostprocessPipelines ()
{
	pipeline_create_infos_t base;
	R_InitDefaultStates (&base);

	base.rasterization_state.cullMode = VK_CULL_MODE_NONE;
	base.color_blend_state.attachmentCount = 1;

	base.vertex_input_state.vertexAttributeDescriptionCount = 0;
	base.vertex_input_state.pVertexAttributeDescriptions = NULL;
	base.vertex_input_state.vertexBindingDescriptionCount = 0;
	base.vertex_input_state.pVertexBindingDescriptions = NULL;

	base.shader_stages[0].module = postprocess_vert_module;
	if (r_ssao.value > 0 && !vulkan_globals.ssao_broken)
	{
		pipeline_create_infos_t ssao;
		R_CreateComputePipeline (
			&ssao_prepare_pipeline, vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT ? ssao_prepare_comp_module : ssao_prepare_msaa_comp_module, 0, NULL,
			"ssao_prepare", false);
		const VkSpecializationMapEntry quality_entry = {.constantID = 0, .offset = 0, .size = sizeof (uint32_t)};
		for (uint32_t quality = 1; quality <= countof (ssao_evaluate_pipelines); ++quality)
		{
			const VkSpecializationInfo specialization = {.mapEntryCount = 1, .pMapEntries = &quality_entry, .dataSize = sizeof (quality), .pData = &quality};
			R_CreateComputePipeline (
				&ssao_evaluate_pipelines[quality - 1], vulkan_globals.shader_float16 ? ssao_evaluate_fp16_comp_module : ssao_evaluate_comp_module, 0,
				&specialization, "ssao_evaluate", false);
		}
		R_CreateComputePipeline (
			&ssao_mip_pipeline,
			vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT ? (vulkan_globals.shader_float16 ? ssao_mip_fp16_comp_module : ssao_mip_comp_module)
																 : (vulkan_globals.shader_float16 ? ssao_mip_msaa_fp16_comp_module : ssao_mip_msaa_comp_module),
			VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT | VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT, NULL,
			"ssao_mip", false);
		R_CreateComputePipeline (
			&ssao_filter_pipeline, vulkan_globals.shader_float16 ? ssao_filter_fp16_comp_module : ssao_filter_comp_module, 0, NULL, "ssao_filter", false);
		vulkan_globals.ssao_broken = !ssao_prepare_pipeline.handle || !ssao_mip_pipeline.handle || !ssao_filter_pipeline.handle;
		for (int i = 0; i < countof (ssao_evaluate_pipelines); ++i)
			vulkan_globals.ssao_broken |= !ssao_evaluate_pipelines[i].handle;
		if (vulkan_globals.ssao_broken)
		{
			Cvar_SetValueQuick (&r_ssao, 0);
			Con_Warning ("Disabling SSAO due to pipeline creation failure\n");
		}
		R_CopyPipelineCreateInfos (&ssao, &base);
		ssao.depth_stencil_state.depthTestEnable = VK_FALSE;
		ssao.depth_stencil_state.depthWriteEnable = VK_FALSE;
		ssao.blend_attachment_states[0].blendEnable = VK_TRUE;
		ssao.blend_attachment_states[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ssao.blend_attachment_states[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ssao.blend_attachment_states[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
		ssao.shader_stages[1].module = vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT ? ssao_composite_frag_module : ssao_composite_msaa_frag_module;
		for (int variant = 0; !vulkan_globals.ssao_broken && variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{
			R_SetPipelineRenderPassVariant (&ssao, SUBPASS_ENTITY_SSAO, variant);
			R_CreateGraphicsPipeline (&ssao_pipelines[variant], &ssao, ssao_layout, "ssao");
		}
	}

	if (vulkan_globals.scene_hdr_active || vid.render_width != vid.width || vid.render_height != vid.height)
	{
		pipeline_create_infos_t upscale;
		R_CopyPipelineCreateInfos (&upscale, &base);
		upscale.multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		upscale.depth_stencil_state.depthTestEnable = VK_FALSE;
		upscale.depth_stencil_state.depthWriteEnable = VK_FALSE;
		upscale.blend_attachment_states[0].blendEnable = VK_FALSE;
		upscale.shader_stages[1].module = scene_upscale_frag_module;
		for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{
			R_SetPipelineRenderPassVariant (&upscale, SUBPASS_UI, variant);
			R_CreateGraphicsPipeline (
				&graphics_pipelines[PIPELINE_SCENE_UPSCALE][SUBPASS_UI][variant], &upscale, vulkan_globals.basic_pipeline_layout, "scene_upscale");
		}
	}

	pipeline_create_infos_t infos;
	R_CopyPipelineCreateInfos (&infos, &base);
	infos.multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	infos.depth_stencil_state.depthTestEnable = VK_TRUE;
	infos.depth_stencil_state.depthWriteEnable = VK_TRUE;
	infos.shader_stages[1].module = postprocess_frag_module;
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_POST_PROCESS, MAIN_RENDER_PASS_STANDARD);
	R_CreateGraphicsPipeline (&vulkan_globals.postprocess_pipeline, &infos, vulkan_globals.postprocess_pipeline.layout, "postprocess");

	R_CopyPipelineCreateInfos (&infos, &base);
	infos.blend_attachment_states[0].blendEnable = VK_TRUE;
	infos.blend_attachment_states[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	infos.blend_attachment_states[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	infos.blend_attachment_states[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	infos.blend_attachment_states[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	uint32_t hdr_scene = vulkan_globals.scene_hdr_active ? 1u : 0u;
	const VkSpecializationMapEntry hdr_entry = {0, 0, sizeof (hdr_scene)};
	const VkSpecializationInfo hdr_specialization = {1, &hdr_entry, sizeof (hdr_scene), &hdr_scene};
	infos.shader_stages[1].pSpecializationInfo = &hdr_specialization;
	infos.shader_stages[1].module = (vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT) ? wboit_resolve_frag_module : wboit_resolve_msaa_frag_module;
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_OIT_RESOLVE, MAIN_RENDER_PASS_OIT);
	R_CreateGraphicsPipeline (&vulkan_globals.wboit_resolve_pipeline, &infos, vulkan_globals.wboit_resolve_pipeline.layout, "wboit_resolve");

	R_CopyPipelineCreateInfos (&infos, &base);
	infos.blend_attachment_states[0].blendEnable = VK_TRUE;
	infos.blend_attachment_states[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	infos.blend_attachment_states[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	infos.blend_attachment_states[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	infos.blend_attachment_states[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	infos.shader_stages[1].pSpecializationInfo = &hdr_specialization;
	infos.shader_stages[1].module = (vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT) ? mboit_resolve_frag_module : mboit_resolve_msaa_frag_module;
	R_SetPipelineRenderPassVariant (&infos, SUBPASS_OIT_RESOLVE, MAIN_RENDER_PASS_MBOIT);
	R_CreateGraphicsPipeline (&vulkan_globals.mboit_resolve_pipeline, &infos, vulkan_globals.mboit_resolve_pipeline.layout, "mboit_resolve");
	return true;
}

/*
===============
R_CreateScreenEffectsPipelines
===============
*/
static bool R_CreateScreenEffectsPipelines ()
{
	const qboolean ten_bit = vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;

	R_CreateComputePipeline (
		&vulkan_globals.screen_effects_pipeline, vulkan_globals.scene_hdr_active ? screen_effects_hdr_comp_module : ten_bit ? screen_effects_10bit_comp_module : screen_effects_8bit_comp_module, 0, NULL, "screen_effects", true);
	return true;
}

/*
===============
R_CreateUpdateLightmapPipelines
===============
*/
static bool R_CreateUpdateLightmapPipelines ()
{
	const qboolean ten_bit = vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;

	VkSpecializationMapEntry specialization_entry;
	specialization_entry.constantID = 0;
	specialization_entry.offset = 0;
	specialization_entry.size = 4;
	uint32_t			 specialization_data = ten_bit; // 10-bit lightmap
	VkSpecializationInfo specialization_info;
	specialization_info.mapEntryCount = 1;
	specialization_info.pMapEntries = &specialization_entry;
	specialization_info.dataSize = 4;
	specialization_info.pData = &specialization_data;

	R_CreateComputePipeline (
		&vulkan_globals.update_lightmap_pipeline, ten_bit ? update_lightmap_10bit_comp_module : update_lightmap_8bit_comp_module, 0, &specialization_info,
		"update_lightmap", true);

	if (vulkan_globals.ray_query)
		R_CreateComputePipeline (
			&vulkan_globals.update_lightmap_rt_pipeline, ten_bit ? update_lightmap_10bit_rt_comp_module : update_lightmap_8bit_rt_comp_module, 0,
			&specialization_info, "update_lightmap_rt", true);
	return true;
}

/*
===============
R_CreateIndirectComputePipelines
===============
*/
static bool R_CreateIndirectComputePipelines ()
{
	R_CreateComputePipeline (&vulkan_globals.indirect_draw_pipeline, indirect_comp_module, 0, NULL, "indirect_draw", true);
	R_CreateComputePipeline (&vulkan_globals.indirect_clear_pipeline, indirect_clear_comp_module, 0, NULL, "indirect_clear", true);
	if (meshlet_cull_comp_module)
		R_CreateComputePipeline (&vulkan_globals.meshlet_cull_pipeline, meshlet_cull_comp_module, 0, NULL, "meshlet_cull", false);
	if (alias_ecs_cull_comp_module)
		R_CreateComputePipeline (&vulkan_globals.ecs_cull_pipeline, alias_ecs_cull_comp_module, 0, NULL, "alias_ecs_cull", false);
	if (meshlet_hiz_comp_module)
		R_CreateComputePipeline (&vulkan_globals.meshlet_hiz_pipeline, meshlet_hiz_comp_module, 0, NULL, "meshlet_hiz", false);
	return true;
}

/*
===============
R_CreateShaderModules
===============
*/
static bool R_TryCreateShaderModules ()
{
	VkShaderModule *created[128];
	shader_transaction_t transaction = {created, 0, countof (created), R_CreateShaderModuleForTransaction,
		R_DestroyShaderModuleForTransaction, NULL};
	CREATE_SHADER_MODULE (basic_vert);
	CREATE_SHADER_MODULE (basic_frag);
	CREATE_SHADER_MODULE (fte_particles_frag);
	CREATE_SHADER_MODULE (fte_particles_msaa_frag);
	CREATE_SHADER_MODULE (basic_oit_frag);
	CREATE_SHADER_MODULE (basic_mboit_moment_frag);
	CREATE_SHADER_MODULE (basic_mboit_composite_frag);
	CREATE_SHADER_MODULE_COND (basic_mboit_composite_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (basic_alphatest_frag);
	CREATE_SHADER_MODULE (basic_notex_frag);
	CREATE_SHADER_MODULE (draw_pic_frag);
	CREATE_SHADER_MODULE (draw_pic_alphatest_frag);
	CREATE_SHADER_MODULE (draw_pic_xbr_frag);
	CREATE_SHADER_MODULE (draw_pic_xbr_alphatest_frag);
	CREATE_SHADER_MODULE (draw_pic_xbr_vert);
	CREATE_SHADER_MODULE (world_vert);
	CREATE_SHADER_MODULE (world_frag);
	CREATE_SHADER_MODULE (world_oit_frag);
	CREATE_SHADER_MODULE (world_mboit_moment_frag);
	CREATE_SHADER_MODULE (world_mboit_composite_frag);
	CREATE_SHADER_MODULE_COND (world_mboit_composite_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (alias_vert);
	CREATE_SHADER_MODULE (alias_instanced_vert);
	CREATE_SHADER_MODULE (alias_frag);
	CREATE_SHADER_MODULE (alias_alphatest_frag);
	CREATE_SHADER_MODULE (alias_oit_frag);
	CREATE_SHADER_MODULE (alias_alphatest_oit_frag);
	CREATE_SHADER_MODULE (alias_mboit_moment_frag);
	CREATE_SHADER_MODULE (alias_mboit_composite_frag);
	CREATE_SHADER_MODULE_COND (alias_mboit_composite_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (alias_alphatest_mboit_moment_frag);
	CREATE_SHADER_MODULE (alias_alphatest_mboit_composite_frag);
	CREATE_SHADER_MODULE_COND (alias_alphatest_mboit_composite_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (md5_mboit_composite_frag);
	CREATE_SHADER_MODULE_COND (md5_mboit_composite_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (md5_alphatest_mboit_composite_frag);
	CREATE_SHADER_MODULE_COND (md5_alphatest_mboit_composite_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (md5_vert);
	CREATE_SHADER_MODULE (md5_8_vert);
	CREATE_SHADER_MODULE (md5_debug_vert);
	CREATE_SHADER_MODULE (sky_layer_vert);
	CREATE_SHADER_MODULE (sky_layer_frag);
	CREATE_SHADER_MODULE (sky_box_frag);
	CREATE_SHADER_MODULE (sky_cube_vert);
	CREATE_SHADER_MODULE (sky_cube_frag);
	CREATE_SHADER_MODULE (postprocess_vert);
	CREATE_SHADER_MODULE (postprocess_frag);
	CREATE_SHADER_MODULE (scene_upscale_frag);
#ifdef _DEBUG
	if (r_ssao.value > 0)
	{
		if (vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT)
			CREATE_SHADER_MODULE_CODE (ssao_composite_frag, ssao_composite_debug_frag, true);
		else
			CREATE_SHADER_MODULE_CODE (ssao_composite_msaa_frag, ssao_composite_msaa_debug_frag, true);
	}
#else
	CREATE_SHADER_MODULE_COND (ssao_composite_frag, r_ssao.value > 0 && vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE_COND (ssao_composite_msaa_frag, r_ssao.value > 0 && vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
#endif
	CREATE_SHADER_MODULE_COND (ssao_prepare_comp, r_ssao.value > 0 && vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE_COND (ssao_prepare_msaa_comp, r_ssao.value > 0 && vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE_COND (ssao_evaluate_comp, r_ssao.value > 0 && !vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE_COND (ssao_evaluate_fp16_comp, r_ssao.value > 0 && vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE_COND (ssao_mip_comp, r_ssao.value > 0 && vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT && !vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE_COND (ssao_mip_fp16_comp, r_ssao.value > 0 && vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT && vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE_COND (ssao_mip_msaa_comp, r_ssao.value > 0 && vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT && !vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE_COND (
		ssao_mip_msaa_fp16_comp, r_ssao.value > 0 && vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT && vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE_COND (ssao_filter_comp, r_ssao.value > 0 && !vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE_COND (ssao_filter_fp16_comp, r_ssao.value > 0 && vulkan_globals.shader_float16);
	CREATE_SHADER_MODULE (wboit_resolve_frag);
	CREATE_SHADER_MODULE_COND (wboit_resolve_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (mboit_resolve_frag);
	CREATE_SHADER_MODULE_COND (mboit_resolve_msaa_frag, vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT);
	CREATE_SHADER_MODULE (screen_effects_8bit_comp);
	CREATE_SHADER_MODULE (screen_effects_10bit_comp);
	CREATE_SHADER_MODULE_COND (screen_effects_hdr_comp, vulkan_globals.scene_hdr_active);
	CREATE_SHADER_MODULE (cs_tex_warp_comp);
	CREATE_SHADER_MODULE (indirect_comp);
	CREATE_SHADER_MODULE (indirect_clear_comp);
	CREATE_SHADER_MODULE_OPTIONAL (meshlet_cull_comp, vulkan_globals.meshlet_cull_pipeline.layout.handle);
	CREATE_SHADER_MODULE_OPTIONAL (alias_ecs_cull_comp, vulkan_globals.ecs_cull_pipeline.layout.handle);
	CREATE_SHADER_MODULE_OPTIONAL (meshlet_hiz_comp, vulkan_globals.meshlet_hiz_pipeline.layout.handle);
#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
	CREATE_SHADER_MODULE_OPTIONAL (world_mesh_mesh, vulkan_globals.world_mesh_pipeline_layout.handle);
#if defined(HAVE_WORLD_TASK_SHADER)
	CREATE_SHADER_MODULE_OPTIONAL (world_task_task, vulkan_globals.world_task_pipeline_layout.handle);
	CREATE_SHADER_MODULE_OPTIONAL (world_task_mesh_mesh, vulkan_globals.world_task_pipeline_layout.handle);
#endif
#endif
	CREATE_SHADER_MODULE (showtris_vert);
	CREATE_SHADER_MODULE (showtris_frag);
	CREATE_SHADER_MODULE (update_lightmap_8bit_comp);
	CREATE_SHADER_MODULE (update_lightmap_10bit_comp);
	CREATE_SHADER_MODULE_COND (update_lightmap_8bit_rt_comp, vulkan_globals.ray_query);
	CREATE_SHADER_MODULE_COND (update_lightmap_10bit_rt_comp, vulkan_globals.ray_query);
#ifdef _DEBUG
	CREATE_SHADER_MODULE_COND (ray_debug_comp, vulkan_globals.ray_query);
	CREATE_SHADER_MODULE_COND (ray_debug_hdr_comp, vulkan_globals.ray_query && vulkan_globals.scene_hdr_active);
#endif
	CREATE_SHADER_MODULE_COND (mesh_interpolate_comp, vulkan_globals.ray_query);
	CREATE_SHADER_MODULE_COND (skinning_comp, vulkan_globals.ray_query);
	CREATE_SHADER_MODULE_COND (skinning_8_comp, vulkan_globals.ray_query);
	return true;

fail:
	ShaderTransaction_Rollback (&transaction);
	return false;
}

/*
===============
R_DestroyShaderModules
===============
*/
static void R_DestroyShaderModules ()
{
	DESTROY_SHADER_MODULE (basic_vert);
	DESTROY_SHADER_MODULE (basic_frag);
	DESTROY_SHADER_MODULE (fte_particles_frag);
	DESTROY_SHADER_MODULE (fte_particles_msaa_frag);
	DESTROY_SHADER_MODULE (basic_oit_frag);
	DESTROY_SHADER_MODULE (basic_mboit_moment_frag);
	DESTROY_SHADER_MODULE (basic_mboit_composite_frag);
	DESTROY_SHADER_MODULE (basic_mboit_composite_msaa_frag);
	DESTROY_SHADER_MODULE (basic_alphatest_frag);
	DESTROY_SHADER_MODULE (basic_notex_frag);
	DESTROY_SHADER_MODULE (draw_pic_frag);
	DESTROY_SHADER_MODULE (draw_pic_alphatest_frag);
	DESTROY_SHADER_MODULE (draw_pic_xbr_frag);
	DESTROY_SHADER_MODULE (draw_pic_xbr_alphatest_frag);
	DESTROY_SHADER_MODULE (draw_pic_xbr_vert);
	DESTROY_SHADER_MODULE (world_vert);
	DESTROY_SHADER_MODULE (world_frag);
	DESTROY_SHADER_MODULE (world_oit_frag);
	DESTROY_SHADER_MODULE (world_mboit_moment_frag);
	DESTROY_SHADER_MODULE (world_mboit_composite_frag);
	DESTROY_SHADER_MODULE (world_mboit_composite_msaa_frag);
	DESTROY_SHADER_MODULE (alias_vert);
	DESTROY_SHADER_MODULE (alias_instanced_vert);
	DESTROY_SHADER_MODULE (alias_frag);
	DESTROY_SHADER_MODULE (alias_alphatest_frag);
	DESTROY_SHADER_MODULE (alias_oit_frag);
	DESTROY_SHADER_MODULE (alias_alphatest_oit_frag);
	DESTROY_SHADER_MODULE (alias_mboit_moment_frag);
	DESTROY_SHADER_MODULE (alias_mboit_composite_frag);
	DESTROY_SHADER_MODULE (alias_mboit_composite_msaa_frag);
	DESTROY_SHADER_MODULE (alias_alphatest_mboit_moment_frag);
	DESTROY_SHADER_MODULE (alias_alphatest_mboit_composite_frag);
	DESTROY_SHADER_MODULE (alias_alphatest_mboit_composite_msaa_frag);
	DESTROY_SHADER_MODULE (md5_mboit_composite_frag);
	DESTROY_SHADER_MODULE (md5_mboit_composite_msaa_frag);
	DESTROY_SHADER_MODULE (md5_alphatest_mboit_composite_frag);
	DESTROY_SHADER_MODULE (md5_alphatest_mboit_composite_msaa_frag);
	DESTROY_SHADER_MODULE (md5_vert);
	DESTROY_SHADER_MODULE (md5_8_vert);
	DESTROY_SHADER_MODULE (md5_debug_vert);
	DESTROY_SHADER_MODULE (sky_layer_vert);
	DESTROY_SHADER_MODULE (sky_layer_frag);
	DESTROY_SHADER_MODULE (sky_box_frag);
	DESTROY_SHADER_MODULE (sky_cube_vert);
	DESTROY_SHADER_MODULE (sky_cube_frag);
	DESTROY_SHADER_MODULE (postprocess_vert);
	DESTROY_SHADER_MODULE (postprocess_frag);
	DESTROY_SHADER_MODULE (scene_upscale_frag);
	DESTROY_SHADER_MODULE (ssao_composite_frag);
	DESTROY_SHADER_MODULE (ssao_composite_msaa_frag);
	DESTROY_SHADER_MODULE (ssao_prepare_comp);
	DESTROY_SHADER_MODULE (ssao_prepare_msaa_comp);
	DESTROY_SHADER_MODULE (ssao_evaluate_comp);
	DESTROY_SHADER_MODULE (ssao_evaluate_fp16_comp);
	DESTROY_SHADER_MODULE (ssao_mip_comp);
	DESTROY_SHADER_MODULE (ssao_mip_fp16_comp);
	DESTROY_SHADER_MODULE (ssao_mip_msaa_comp);
	DESTROY_SHADER_MODULE (ssao_mip_msaa_fp16_comp);
	DESTROY_SHADER_MODULE (ssao_filter_comp);
	DESTROY_SHADER_MODULE (ssao_filter_fp16_comp);
	DESTROY_SHADER_MODULE (wboit_resolve_frag);
	DESTROY_SHADER_MODULE (wboit_resolve_msaa_frag);
	DESTROY_SHADER_MODULE (mboit_resolve_frag);
	DESTROY_SHADER_MODULE (mboit_resolve_msaa_frag);
	DESTROY_SHADER_MODULE (screen_effects_8bit_comp);
	DESTROY_SHADER_MODULE (screen_effects_10bit_comp);
	DESTROY_SHADER_MODULE (screen_effects_hdr_comp);
	DESTROY_SHADER_MODULE (cs_tex_warp_comp);
	DESTROY_SHADER_MODULE (indirect_comp);
	DESTROY_SHADER_MODULE (indirect_clear_comp);
	DESTROY_SHADER_MODULE (meshlet_cull_comp);
	DESTROY_SHADER_MODULE (alias_ecs_cull_comp);
	DESTROY_SHADER_MODULE (meshlet_hiz_comp);
#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
	DESTROY_SHADER_MODULE (world_mesh_mesh);
#if defined(HAVE_WORLD_TASK_SHADER)
	DESTROY_SHADER_MODULE (world_task_task);
	DESTROY_SHADER_MODULE (world_task_mesh_mesh);
#endif
#endif
	DESTROY_SHADER_MODULE (showtris_vert);
	DESTROY_SHADER_MODULE (showtris_frag);
	DESTROY_SHADER_MODULE (update_lightmap_8bit_comp);
	DESTROY_SHADER_MODULE (update_lightmap_8bit_rt_comp);
	DESTROY_SHADER_MODULE (update_lightmap_10bit_comp);
	DESTROY_SHADER_MODULE (update_lightmap_10bit_rt_comp);
	DESTROY_SHADER_MODULE (ray_debug_comp);
	DESTROY_SHADER_MODULE (ray_debug_hdr_comp);
	DESTROY_SHADER_MODULE (mesh_interpolate_comp);
	DESTROY_SHADER_MODULE (skinning_comp);
	DESTROY_SHADER_MODULE (skinning_8_comp);
}

/*
===============
R_CreatePipelines
===============
*/
bool R_TryCreatePipelines (void)
{
	if (renderer_pipelines_created || active_pipeline_transaction) return false;
	pipeline_transaction_t transaction = {0};
	const float previous_ssao = r_ssao.value;
	const qboolean previous_ssao_broken = vulkan_globals.ssao_broken;
#if defined(VK_EXT_mesh_shader)
	const qboolean previous_mesh_ready = vulkan_globals.world_mesh_ready;
	const qboolean previous_task_ready = vulkan_globals.world_task_ready;
#endif
	Sys_Printf ("Creating pipelines\n");

	if (!R_TryCreateShaderModules ()) return false;
	active_pipeline_transaction = &transaction;
	R_InitVertexAttributes ();

	if (!R_CreateBasicPipelines () || !R_CreateWarpPipelines () || !R_CreateParticlesPipelines () ||
		!R_CreateFTEParticlesPipelines () || !R_CreateSpritesPipelines () || !R_CreateSkyPipelines () ||
		!R_CreateShowTrisPipelines () || !R_CreateWorldPipelines () || !R_CreateAliasPipelines () ||
		!R_CreateMD5Pipelines () || !R_CreatePostprocessPipelines () || !R_CreateScreenEffectsPipelines () ||
		!R_CreateUpdateLightmapPipelines () || !R_CreateIndirectComputePipelines () ||
		!R_CreateRayDebugPipelines () || !R_CreateAnimComputePipelines ())
	{
		R_PipelineTransactionRollback (&transaction);
		active_pipeline_transaction = NULL;
		free (transaction.created);
		R_DestroyShaderModules ();
		if (r_ssao.value != previous_ssao) Cvar_SetValueQuick (&r_ssao, previous_ssao);
		vulkan_globals.ssao_broken = previous_ssao_broken;
#if defined(VK_EXT_mesh_shader)
		vulkan_globals.world_mesh_ready = previous_mesh_ready;
		vulkan_globals.world_task_ready = previous_task_ready;
#endif
		return false;
	}

	active_pipeline_transaction = NULL;
	free (transaction.created);
	R_DestroyShaderModules ();
	renderer_pipelines_created = true;
	return true;
}

void R_CreatePipelines (void)
{
	if (!R_TryCreatePipelines ()) Sys_Error ("Could not create renderer pipelines");
}

/*
===============
R_DestroyPipelines
===============
*/
void R_DestroyPipelines (void)
{
	renderer_pipelines_created = false;
	R_PipelineCacheRetryFailed ();
	R_RadianceSetReceiverUse (1, false, false);
	R_RadianceReset ("renderer resources being recreated");
	/* The caller has waited for the device before destroying frame resources. */
	R_ECSGPUShutdown ();
	vkDestroyPipeline (vulkan_globals.device, ssao_mip_pipeline.handle, NULL);
	ssao_mip_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, ssao_prepare_pipeline.handle, NULL);
	for (int i = 0; i < countof (ssao_evaluate_pipelines); ++i)
	{
		vkDestroyPipeline (vulkan_globals.device, ssao_evaluate_pipelines[i].handle, NULL);
		ssao_evaluate_pipelines[i].handle = VK_NULL_HANDLE;
	}
	vkDestroyPipeline (vulkan_globals.device, ssao_filter_pipeline.handle, NULL);
	ssao_prepare_pipeline.handle = ssao_filter_pipeline.handle = VK_NULL_HANDLE;
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		vkDestroyPipeline (vulkan_globals.device, ssao_pipelines[variant].handle, NULL);
		ssao_pipelines[variant].handle = VK_NULL_HANDLE;
	}
	while (pipeline_instances)
	{
		pipeline_instance_t *instance = pipeline_instances;
		pipeline_instances = instance->allocation_next;
		vkDestroyPipeline (vulkan_globals.device, instance->handle, NULL);
		Mem_Free (instance);
	}
	int i;
	for (int pipeline = 0; pipeline < GRAPHICS_PIPELINE_COUNT; ++pipeline)
		for (int stage = 0; stage < SUBPASS_COUNT; ++stage)
			for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
			{
				vulkan_pipeline_t *instance = &graphics_pipelines[pipeline][stage][variant];
				vkDestroyPipeline (vulkan_globals.device, instance->handle, NULL);
				instance->handle = VK_NULL_HANDLE;
			}
	for (i = 0; i < WORLD_PIPELINE_COUNT; ++i)
	{
		for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.world_pipelines[variant][i].handle, NULL);
			vulkan_globals.world_pipelines[variant][i].handle = VK_NULL_HANDLE;
#if defined(VK_EXT_mesh_shader)
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.world_mesh_pipelines[variant][i].handle, NULL);
			vulkan_globals.world_mesh_pipelines[variant][i].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.world_task_pipelines[variant][i].handle, NULL);
			vulkan_globals.world_task_pipelines[variant][i].handle = VK_NULL_HANDLE;
#endif
		}
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.world_wboit_pipelines[i].handle, NULL);
		vulkan_globals.world_wboit_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.world_mboit_moment_pipelines[i].handle, NULL);
		vulkan_globals.world_mboit_moment_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.world_mboit_composite_pipelines[i].handle, NULL);
		vulkan_globals.world_mboit_composite_pipelines[i].handle = VK_NULL_HANDLE;
	}
#if defined(VK_EXT_mesh_shader)
	vulkan_globals.world_mesh_ready = false;
	vulkan_globals.world_task_ready = false;
#endif
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.raster_tex_warp_pipeline.handle, NULL);
	vulkan_globals.raster_tex_warp_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.particle_pipeline.handle, NULL);
	vulkan_globals.particle_pipeline.handle = VK_NULL_HANDLE;
	if (vulkan_globals.particle_oit_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.particle_oit_pipeline.handle, NULL);
		vulkan_globals.particle_oit_pipeline.handle = VK_NULL_HANDLE;
	}
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.particle_post_oit_pipeline[variant].handle, NULL);
		vulkan_globals.particle_post_oit_pipeline[variant].handle = VK_NULL_HANDLE;
	}
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.particle_mboit_moment_pipeline.handle, NULL);
	vulkan_globals.particle_mboit_moment_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.particle_mboit_composite_pipeline.handle, NULL);
	vulkan_globals.particle_mboit_composite_pipeline.handle = VK_NULL_HANDLE;
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		for (i = 0; i < (vulkan_globals.non_solid_fill ? 16 : 8); ++i)
		{
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.fte_particle_pipelines[variant][i].handle, NULL);
			vulkan_globals.fte_particle_pipelines[variant][i].handle = VK_NULL_HANDLE;
		}
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sprite_pipeline[variant].handle, NULL);
		vulkan_globals.sprite_pipeline[variant].handle = VK_NULL_HANDLE;
	}
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sprite_oit_pipeline.handle, NULL);
	vulkan_globals.sprite_oit_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sprite_mboit_moment_pipeline.handle, NULL);
	vulkan_globals.sprite_mboit_moment_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sprite_mboit_composite_pipeline.handle, NULL);
	vulkan_globals.sprite_mboit_composite_pipeline.handle = VK_NULL_HANDLE;

	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		for (i = 0; i < 2; ++i)
		{
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sky_stencil_pipeline[variant][i].handle, NULL);
			vulkan_globals.sky_stencil_pipeline[variant][i].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sky_color_pipeline[variant][i].handle, NULL);
			vulkan_globals.sky_color_pipeline[variant][i].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sky_cube_pipeline[variant][i].handle, NULL);
			vulkan_globals.sky_cube_pipeline[variant][i].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sky_layer_pipeline[variant][i].handle, NULL);
			vulkan_globals.sky_layer_pipeline[variant][i].handle = VK_NULL_HANDLE;
		}
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.sky_box_pipeline[variant].handle, NULL);
		vulkan_globals.sky_box_pipeline[variant].handle = VK_NULL_HANDLE;
	}
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.alias_instanced_pipelines[variant].handle, NULL);
		vulkan_globals.alias_instanced_pipelines[variant].handle = VK_NULL_HANDLE;
	}
	for (i = 0; i < MODEL_PIPELINE_COUNT; ++i)
	{
		for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.alias_pipelines[variant][i].handle, NULL);
			vulkan_globals.alias_pipelines[variant][i].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_pipelines[variant][i].handle, NULL);
			vulkan_globals.md5_pipelines[variant][i].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_8_pipelines[variant][i].handle, NULL);
			vulkan_globals.md5_8_pipelines[variant][i].handle = VK_NULL_HANDLE;
		}
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.alias_wboit_pipelines[i].handle, NULL);
		vulkan_globals.alias_wboit_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.alias_mboit_moment_pipelines[i].handle, NULL);
		vulkan_globals.alias_mboit_moment_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.alias_mboit_composite_pipelines[i].handle, NULL);
		vulkan_globals.alias_mboit_composite_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_wboit_pipelines[i].handle, NULL);
		vulkan_globals.md5_wboit_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_mboit_moment_pipelines[i].handle, NULL);
		vulkan_globals.md5_mboit_moment_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_mboit_composite_pipelines[i].handle, NULL);
		vulkan_globals.md5_mboit_composite_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_8_wboit_pipelines[i].handle, NULL);
		vulkan_globals.md5_8_wboit_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_8_mboit_moment_pipelines[i].handle, NULL);
		vulkan_globals.md5_8_mboit_moment_pipelines[i].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_8_mboit_composite_pipelines[i].handle, NULL);
		vulkan_globals.md5_8_mboit_composite_pipelines[i].handle = VK_NULL_HANDLE;
	}
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.postprocess_pipeline.handle, NULL);
	vulkan_globals.postprocess_pipeline.handle = VK_NULL_HANDLE;
	if (vulkan_globals.wboit_resolve_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.wboit_resolve_pipeline.handle, NULL);
		vulkan_globals.wboit_resolve_pipeline.handle = VK_NULL_HANDLE;
	}
	if (vulkan_globals.mboit_resolve_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.mboit_resolve_pipeline.handle, NULL);
		vulkan_globals.mboit_resolve_pipeline.handle = VK_NULL_HANDLE;
	}
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.screen_effects_pipeline.handle, NULL);
	vulkan_globals.screen_effects_pipeline.handle = VK_NULL_HANDLE;

	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.cs_tex_warp_pipeline.handle, NULL);
	vulkan_globals.cs_tex_warp_pipeline.handle = VK_NULL_HANDLE;
	if (vulkan_globals.showtris_pipeline[MAIN_RENDER_PASS_STANDARD].handle != VK_NULL_HANDLE)
	{
		for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		{
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.showtris_pipeline[variant].handle, NULL);
			vulkan_globals.showtris_pipeline[variant].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.showtris_indirect_pipeline[variant].handle, NULL);
			vulkan_globals.showtris_indirect_pipeline[variant].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.showtris_depth_test_pipeline[variant].handle, NULL);
			vulkan_globals.showtris_depth_test_pipeline[variant].handle = VK_NULL_HANDLE;
			vkDestroyPipeline (vulkan_globals.device, vulkan_globals.showtris_indirect_depth_test_pipeline[variant].handle, NULL);
			vulkan_globals.showtris_indirect_depth_test_pipeline[variant].handle = VK_NULL_HANDLE;
		}
	}
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.debug_lines_pipeline[variant].handle, NULL);
		vulkan_globals.debug_lines_pipeline[variant].handle = VK_NULL_HANDLE;
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.md5_debug_pipeline[variant].handle, NULL);
		vulkan_globals.md5_debug_pipeline[variant].handle = VK_NULL_HANDLE;
	}
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.update_lightmap_pipeline.handle, NULL);
	vulkan_globals.update_lightmap_pipeline.handle = VK_NULL_HANDLE;
	if (vulkan_globals.update_lightmap_rt_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.update_lightmap_rt_pipeline.handle, NULL);
		vulkan_globals.update_lightmap_rt_pipeline.handle = VK_NULL_HANDLE;
	}
	if (vulkan_globals.ray_debug_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.ray_debug_pipeline.handle, NULL);
		vulkan_globals.ray_debug_pipeline.handle = VK_NULL_HANDLE;
	}
	if (vulkan_globals.mesh_interpolate_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.mesh_interpolate_pipeline.handle, NULL);
		vulkan_globals.mesh_interpolate_pipeline.handle = VK_NULL_HANDLE;
	}
	if (vulkan_globals.skinning_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.skinning_pipeline.handle, NULL);
		vulkan_globals.skinning_pipeline.handle = VK_NULL_HANDLE;
	}
	if (vulkan_globals.skinning_8_pipeline.handle != VK_NULL_HANDLE)
	{
		vkDestroyPipeline (vulkan_globals.device, vulkan_globals.skinning_8_pipeline.handle, NULL);
		vulkan_globals.skinning_8_pipeline.handle = VK_NULL_HANDLE;
	}
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.indirect_draw_pipeline.handle, NULL);
	vulkan_globals.indirect_draw_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.indirect_clear_pipeline.handle, NULL);
	vulkan_globals.indirect_clear_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.meshlet_cull_pipeline.handle, NULL);
	vulkan_globals.meshlet_cull_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.ecs_cull_pipeline.handle, NULL);
	vulkan_globals.ecs_cull_pipeline.handle = VK_NULL_HANDLE;
	vkDestroyPipeline (vulkan_globals.device, vulkan_globals.meshlet_hiz_pipeline.handle, NULL);
	vulkan_globals.meshlet_hiz_pipeline.handle = VK_NULL_HANDLE;
}

/*
===================
R_LodBiasChanged_f
===================
*/
static void R_LodBiasChanged_f (cvar_t *var)
{
	R_InitSamplers ();
}

/*
===============
R_Init
===============
*/
void R_Init (void)
{
	cmd_function_t *cmd;
	R_RadianceInit ();

	Cmd_AddCommand ("timerefresh", R_TimeRefresh_f);
	Cmd_AddCommand ("meshlet_stats", R_MeshletStats_f);
	Cmd_AddCommand ("ecs_gpu_stats", R_ECSGPUStats_f);
	Cmd_AddCommand ("pointfile", R_ReadPointFile_f);

	cmd = Cmd_AddCommand ("r_showbboxes_filter", R_ShowbboxesFilter_f);
	if (cmd)
		cmd->completion = R_ShowbboxesFilter_Completion_f;

	Cmd_AddCommand ("r_showbboxes_filter_clear", R_ShowbboxesFilterClear_f);

	Cmd_AddCommand ("vkmemstats", R_VulkanMemStats_f);

	Cvar_RegisterVariable (&r_fullbright);
	Cvar_RegisterVariable (&r_lightmap);
	Cvar_RegisterVariable (&r_drawentities);
	Cvar_RegisterVariable (&r_drawviewmodel);
	Cvar_RegisterVariable (&r_wateralpha);
	Cvar_RegisterVariable (&r_oit);
	Cvar_SetCallback (&r_wateralpha, R_SetWateralpha_f);
	Cvar_RegisterVariable (&r_dynamic);
	Cvar_RegisterVariable (&r_novis);
#if defined(USE_SIMD)
	Cvar_RegisterVariable (&r_simd);
	Cvar_SetCallback (&r_simd, R_SIMD_f);
	R_SIMD_f (&r_simd);
#endif
	Cvar_RegisterVariable (&r_alphasort);
	Cvar_RegisterVariable (&scr_speeds);
	Cvar_RegisterVariable (&r_pos);
	Cvar_RegisterVariable (&gl_polyblend);
	Cvar_RegisterVariable (&gl_nocolors);
	Cvar_SetCallback (&gl_nocolors, Mod_RefreshSkins_f);

	// johnfitz -- new cvars
	Cvar_RegisterVariable (&r_clearcolor);
	Cvar_SetCallback (&r_clearcolor, R_SetClearColor_f);
	Cvar_RegisterVariable (&r_fastclear);
	Cvar_SetCallback (&r_fastclear, R_SetFastClear_f);
	Cvar_RegisterVariable (&r_waterquality);
	Cvar_RegisterVariable (&r_waterwarp);
	Cvar_RegisterVariable (&r_waterwarpcompute);
	Cvar_RegisterVariable (&r_flatlightstyles);
	Cvar_RegisterVariable (&r_lerplightstyles);
	Cvar_RegisterVariable (&r_entdlightscale);
	Cvar_RegisterVariable (&r_oldskyleaf);
	Cvar_RegisterVariable (&r_drawworld);
	Cvar_RegisterVariable (&r_showtris);
	Cvar_RegisterVariable (&r_ecs_batch);
	Cvar_RegisterVariable (&r_ecs_gpu_cull);
	Cvar_RegisterVariable (&r_showskel);
	Cvar_RegisterVariable (&r_showbboxes);
	Cvar_RegisterVariable (&r_showbboxes_think);
	Cvar_RegisterVariable (&r_showbboxes_health);
	Cvar_RegisterVariable (&r_showbboxes_links);
	Cvar_RegisterVariable (&r_showbboxes_targets);
	Cvar_RegisterVariable (&r_showfields);
	Cvar_RegisterVariable (&r_showfields_align);
	Cvar_RegisterVariable (&gl_farclip);
	Cvar_RegisterVariable (&gl_fullbrights);
	Cvar_SetCallback (&gl_fullbrights, GL_Fullbrights_f);
	Cvar_RegisterVariable (&r_lerpmodels);
	Cvar_RegisterVariable (&r_lerpmove);
	Cvar_RegisterVariable (&r_lerpturn);
	Cvar_RegisterVariable (&r_nolerp_list);
	Cvar_SetCallback (&r_nolerp_list, R_Model_ExtraFlags_List_f);
	// johnfitz

	Cvar_RegisterVariable (&gl_zfix); // QuakeSpasm z-fighting fix
	Cvar_RegisterVariable (&r_lavaalpha);
	Cvar_RegisterVariable (&r_telealpha);
	Cvar_RegisterVariable (&r_slimealpha);
	Cvar_RegisterVariable (&r_lodbias);
	Cvar_RegisterVariable (&gl_lodbias);
	Cvar_SetCallback (&r_lodbias, R_LodBiasChanged_f);
	Cvar_SetCallback (&gl_lodbias, R_LodBiasChanged_f);
	Cvar_SetCallback (&r_lavaalpha, R_SetLavaalpha_f);
	Cvar_SetCallback (&r_telealpha, R_SetTelealpha_f);
	Cvar_SetCallback (&r_slimealpha, R_SetSlimealpha_f);

	Cvar_RegisterVariable (&r_gpulightmapupdate);
	Cvar_RegisterVariable (&r_rtshadows);
	Cvar_SetCallback (&r_rtshadows, R_SetRTShadows_f);
	Cvar_RegisterVariable (&r_indirect);
	Cvar_RegisterVariable (&r_meshlets);
	Cvar_RegisterVariable (&r_meshlet_occlusion);
	Cvar_RegisterVariable (&r_meshlet_occlusion_stats);
	Cvar_RegisterVariable (&r_meshlet_task);
	Cvar_RegisterVariable (&r_tasks);
	R_InitSSAO ();
	Cvar_RegisterVariable (&r_parallelmark);

	R_InitParticles ();
	SetClearColor (); // johnfitz

	Sky_Init (); // johnfitz
	Fog_Init (); // johnfitz

	R_AllocateLightmapComputeBuffers ();

	staging_mutex = SDL_CreateMutex ();
}

/*
===============
R_AllocateDescriptorSet
===============
*/
VkResult R_TryAllocateDescriptorSet (vulkan_desc_set_layout_t *layout, VkDescriptorSet *out)
{
	if (!layout || !out || *out != VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
	ZEROED_STRUCT (VkDescriptorSetAllocateInfo, descriptor_set_allocate_info);
	descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptor_set_allocate_info.descriptorPool = vulkan_globals.descriptor_pool;
	descriptor_set_allocate_info.descriptorSetCount = 1;
	descriptor_set_allocate_info.pSetLayouts = &layout->handle;

	VkDescriptorSet handle = VK_NULL_HANDLE;
	VkResult err = vkAllocateDescriptorSets (vulkan_globals.device, &descriptor_set_allocate_info, &handle);
	if (err != VK_SUCCESS) return err;

	Atomic_AddUInt32 (&num_vulkan_combined_image_samplers, layout->num_combined_image_samplers);
	Atomic_AddUInt32 (&num_vulkan_samplers, layout->num_samplers);
	Atomic_AddUInt32 (&num_vulkan_ubos_dynamic, layout->num_ubos_dynamic);
	Atomic_AddUInt32 (&num_vulkan_ubos, layout->num_ubos);
	Atomic_AddUInt32 (&num_vulkan_storage_buffers, layout->num_storage_buffers);
	Atomic_AddUInt32 (&num_vulkan_input_attachments, layout->num_input_attachments);
	Atomic_AddUInt32 (&num_vulkan_storage_images, layout->num_storage_images);
	Atomic_AddUInt32 (&num_vulkan_sampled_images, layout->num_sampled_images);
	Atomic_AddUInt32 (&num_acceleration_structures, layout->num_acceleration_structures);

	*out = handle;
	return VK_SUCCESS;
}

VkDescriptorSet R_AllocateDescriptorSet (vulkan_desc_set_layout_t *layout)
{
	VkDescriptorSet handle = VK_NULL_HANDLE;
	VkResult err = R_TryAllocateDescriptorSet (layout, &handle);
	if (err != VK_SUCCESS) Sys_Error ("Couldn't allocate descriptor set: %d", (int)err);
	return handle;
}

/*
===============
R_FreeDescriptorSet
===============
*/

void R_FreeDescriptorSet (VkDescriptorSet desc_set, vulkan_desc_set_layout_t *layout)
{
	vkFreeDescriptorSets (vulkan_globals.device, vulkan_globals.descriptor_pool, 1, &desc_set);

	Atomic_SubUInt32 (&num_vulkan_combined_image_samplers, layout->num_combined_image_samplers);
	Atomic_SubUInt32 (&num_vulkan_samplers, layout->num_samplers);
	Atomic_SubUInt32 (&num_vulkan_ubos_dynamic, layout->num_ubos_dynamic);
	Atomic_SubUInt32 (&num_vulkan_ubos, layout->num_ubos);
	Atomic_SubUInt32 (&num_vulkan_storage_buffers, layout->num_storage_buffers);
	Atomic_SubUInt32 (&num_vulkan_input_attachments, layout->num_input_attachments);
	Atomic_SubUInt32 (&num_vulkan_storage_images, layout->num_storage_images);
	Atomic_SubUInt32 (&num_vulkan_sampled_images, layout->num_sampled_images);
	Atomic_SubUInt32 (&num_acceleration_structures, layout->num_acceleration_structures);
}

/*
===============
R_TranslatePlayerSkin -- johnfitz -- rewritten.  also, only handles new colors, not new skins
===============
*/
void R_TranslatePlayerSkin (int playernum)
{
	int top, bottom;

	top = (cl.scores[playernum].colors & 0xf0) >> 4;
	bottom = cl.scores[playernum].colors & 15;

	if (!gl_nocolors.value)
		if (playertextures[playernum])
			TexMgr_ReloadImage (playertextures[playernum], top, bottom);
}

/*
===============
R_TranslateNewPlayerSkin -- johnfitz -- split off of TranslatePlayerSkin -- this is called when
the skin or model actually changes, instead of just new colors
added bug fix from bengt jardup
===============
*/
void R_TranslateNewPlayerSkin (int playernum)
{
	char		name[64];
	byte	   *pixels;
	aliashdr_t *paliashdr;
	int			skinnum;

	// get correct texture pixels
	entity_t *currententity = &cl.entities[1 + playernum];

	if (!currententity->model || currententity->model->type != mod_alias)
		return;

	paliashdr = (aliashdr_t *)Mod_Extradata (currententity->model);
	skinnum = currententity->skinnum;

	// TODO: move these tests to the place where skinnum gets received from the server
	if (skinnum < 0 || skinnum >= paliashdr->numskins)
	{
		Con_DPrintf ("(%d): Invalid player skin #%d\n", playernum, skinnum);
		skinnum = 0;
	}

	pixels = (byte *)paliashdr->texels[skinnum];
	if (!pixels)
	{
		static qboolean warned = false;
		if (!warned)
		{
			warned = true;
			Con_Warning ("can't recolor non-indexed player skin\n");
		}
		playertextures[playernum] = NULL;
		return;
	}

	// upload new image
	q_snprintf (name, sizeof (name), "player_%i", playernum);
	playertextures[playernum] = TexMgr_LoadImage (
		currententity->model, name, paliashdr->skinwidth, paliashdr->skinheight, SRC_INDEXED, pixels, paliashdr->gltextures[skinnum][0]->source_file,
		paliashdr->gltextures[skinnum][0]->source_offset, TEXPREF_PAD | TEXPREF_OVERWRITE);

	// now recolor it
	R_TranslatePlayerSkin (playernum);
}

/*
===============
R_NewGame -- johnfitz -- handle a game switch
===============
*/
void R_NewGame (void)
{
	int i;

	// clear playertexture pointers (the textures themselves were freed by texmgr_newgame)
	for (i = 0; i < MAX_SCOREBOARD; i++)
		playertextures[i] = NULL;
}

/*
=============
R_ParseWorldspawn

called at map load
=============
*/
static void R_ParseWorldspawn (void)
{
	char		key[128], value[4096];
	const char *data;

	map_fallbackalpha = r_wateralpha.value;
	map_wateralpha = (cl.worldmodel->contentstransparent & SURF_DRAWWATER) ? r_wateralpha.value : 1;
	map_lavaalpha = (cl.worldmodel->contentstransparent & SURF_DRAWLAVA) ? r_lavaalpha.value : 1;
	map_telealpha = (cl.worldmodel->contentstransparent & SURF_DRAWTELE) ? r_telealpha.value : 1;
	map_slimealpha = (cl.worldmodel->contentstransparent & SURF_DRAWSLIME) ? r_slimealpha.value : 1;

	data = COM_Parse (cl.worldmodel->entities);
	if (!data)
		return; // error
	if (com_token[0] != '{')
		return; // error
	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_ParseEx (data, CPE_ALLOWTRUNC);
		if (!data)
			return; // error
		q_strlcpy (value, com_token, sizeof (value));

		if (!strcmp ("wateralpha", key))
			map_wateralpha = atof (value);

		if (!strcmp ("lavaalpha", key))
			map_lavaalpha = atof (value);

		if (!strcmp ("telealpha", key))
			map_telealpha = atof (value);

		if (!strcmp ("slimealpha", key))
			map_slimealpha = atof (value);
	}
}

/*
===============
R_NewMap
===============
*/
void R_NewMap (void)
{
	int i;
	R_RadianceFrameNewMap ();
	R_RadianceSetReceiverUse (1, false, false);
	R_RadianceReset ("map resources not ready");

	for (i = 0; i < MAX_LIGHTSTYLES; i++)
		d_lightstylevalue[i] = 264; // normal light value

	// clear out efrags in case the level hasn't been reloaded
	// numleafs excludes the solid leaf at index 0.
	for (i = 0; i <= cl.worldmodel->numleafs; i++)
		cl.worldmodel->leafs[i].efrags = NULL;

	r_viewleaf = NULL;
	R_ClearParticles ();
	PScript_ClearParticles (true);
	r_framecount = 1;
	R_RebuildBrushModels (true);
	GL_PrepareSIMDAndParallelData ();
	// ericw -- no longer load alias models into a VBO here, it's done in Mod_LoadAliasModel

	r_framecount = 0;	 // johnfitz -- paranoid?
	r_visframecount = 0; // johnfitz -- paranoid?

	Sky_NewMap ();			 // johnfitz -- skybox in worldspawn
	Fog_NewMap ();			 // johnfitz -- global fog in worldspawn
	R_ParseWorldspawn ();	 // ericw -- wateralpha, lavaalpha, telealpha, slimealpha in worldspawn
	R_ParseEntityDlights (); // 2021 rerelease shadow casting light entities

	VEC_CLEAR (r_pointfile);

	if (developer.value || map_checks.value)
		if (!cl.worldmodel->visdata && COM_FileExists (va ("maps/%s.pts", cl.mapname), NULL))
			Cbuf_AddText ("pointfile leak\n");

	GL_UpdateDescriptorSets ();
}

/*
====================
R_TimeRefresh_f

For program optimization
====================
*/
void R_TimeRefresh_f (void)
{
	int	  i;
	float start, stop, time;

	if (cls.state != ca_connected)
	{
		Con_Printf ("Not connected to a server\n");
		return;
	}

	GL_SynchronizeEndRenderingTask ();

	start = Sys_DoubleTime ();
	for (i = 0; i < 128; i++)
	{
		GL_BeginRendering (false, NULL, &glwidth, &glheight);
		r_refdef.viewangles[1] = i / 128.0 * 360.0;
		R_RenderView (false, INVALID_TASK_HANDLE, INVALID_TASK_HANDLE, INVALID_TASK_HANDLE, INVALID_TASK_HANDLE);
		GL_EndRendering (false, false);
	}

	// glFinish ();
	stop = Sys_DoubleTime ();
	time = stop - start;
	Con_Printf ("%f seconds (%f fps)\n", time, 128 / time);
}

/*
====================
R_AllocateVulkanMemory
====================
*/
VkResult R_TryAllocateVulkanMemory (vulkan_memory_t *memory, const VkMemoryAllocateInfo *memory_allocate_info, vulkan_memory_type_t type, atomic_uint32_t *num_allocations)
{
	VkDeviceMemory handle = VK_NULL_HANDLE;
	if (type != VULKAN_MEMORY_TYPE_NONE)
	{
		VkResult err = vkAllocateMemory (vulkan_globals.device, memory_allocate_info, NULL, &handle);
		if (err != VK_SUCCESS)
			return err;
		if (num_allocations)
			Atomic_IncrementUInt32 (num_allocations);
	}
	memory->handle = handle;
	memory->type = type;
	memory->size = memory_allocate_info->allocationSize;
	if (memory->type == VULKAN_MEMORY_TYPE_DEVICE)
		Atomic_AddUInt64 (&total_device_vulkan_allocation_size, memory->size);
	else if (memory->type == VULKAN_MEMORY_TYPE_HOST)
		Atomic_AddUInt64 (&total_host_vulkan_allocation_size, memory->size);
	return VK_SUCCESS;
}

void R_AllocateVulkanMemory (vulkan_memory_t *memory, VkMemoryAllocateInfo *memory_allocate_info, vulkan_memory_type_t type, atomic_uint32_t *num_allocations)
{
	VkResult err = R_TryAllocateVulkanMemory (memory, memory_allocate_info, type, num_allocations);
	if (err != VK_SUCCESS)
		Sys_Error ("vkAllocateMemory failed with code %i", (int)err);
}

/*
====================
R_FreeVulkanMemory
====================
*/
void R_FreeVulkanMemory (vulkan_memory_t *memory, atomic_uint32_t *num_allocations)
{
	if (memory->type == VULKAN_MEMORY_TYPE_DEVICE)
		Atomic_SubUInt64 (&total_device_vulkan_allocation_size, memory->size);
	else if (memory->type == VULKAN_MEMORY_TYPE_HOST)
		Atomic_SubUInt64 (&total_host_vulkan_allocation_size, memory->size);
	if (memory->type != VULKAN_MEMORY_TYPE_NONE)
	{
		vkFreeMemory (vulkan_globals.device, memory->handle, NULL);
		if (num_allocations)
			Atomic_DecrementUInt32 (num_allocations);
	}
	memory->handle = VK_NULL_HANDLE;
	memory->size = 0;
	memory->type = VULKAN_MEMORY_TYPE_NONE;
}

/*
====================
R_CreateBuffer
====================
*/
void R_CreateBuffer (
	VkBuffer *buffer, vulkan_memory_t *memory, const size_t size, VkBufferUsageFlags usage, const VkFlags mem_requirements_mask,
	const VkFlags mem_preferred_mask, atomic_uint32_t *num_allocations, VkDeviceAddress *device_address, const char *name)
{
	VkResult err;
	qboolean get_device_address = vulkan_globals.vk_get_buffer_device_address && device_address;

	if (get_device_address)
		usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;

	VkBufferCreateInfo buffer_create_info;
	memset (&buffer_create_info, 0, sizeof (buffer_create_info));
	buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_create_info.size = size;
	buffer_create_info.usage = usage;
	err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, buffer);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateBuffer failed with code %i", (int)err);

	GL_SetObjectName ((uint64_t)*buffer, VK_OBJECT_TYPE_BUFFER, va ("%s buffer", name));

	VkMemoryRequirements memory_requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, *buffer, &memory_requirements);

	ZEROED_STRUCT (VkMemoryAllocateFlagsInfo, memory_allocate_flags_info);
	if (get_device_address)
	{
		memory_allocate_flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR;
		memory_allocate_flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	}

	ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.pNext = get_device_address ? &memory_allocate_flags_info : NULL;
	memory_allocate_info.allocationSize = memory_requirements.size;
	memory_allocate_info.memoryTypeIndex = GL_MemoryTypeFromProperties (memory_requirements.memoryTypeBits, mem_requirements_mask, mem_preferred_mask);

	R_AllocateVulkanMemory (memory, &memory_allocate_info, VULKAN_MEMORY_TYPE_DEVICE, num_allocations);
	GL_SetObjectName ((uint64_t)memory->handle, VK_OBJECT_TYPE_DEVICE_MEMORY, va ("%s memory", name));

	err = vkBindBufferMemory (vulkan_globals.device, *buffer, memory->handle, 0);
	if (err != VK_SUCCESS)
		Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);

	if (get_device_address)
	{
		VkBufferDeviceAddressInfoKHR buffer_device_address_info;
		memset (&buffer_device_address_info, 0, sizeof (buffer_device_address_info));
		buffer_device_address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR;
		buffer_device_address_info.buffer = *buffer;
		*device_address = vulkan_globals.vk_get_buffer_device_address (vulkan_globals.device, &buffer_device_address_info);
	}
}

/*
====================
R_FreeBuffer
====================
*/
void R_FreeBuffer (const VkBuffer buffer, vulkan_memory_t *memory, atomic_uint32_t *num_allocations)
{
	if (buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer (vulkan_globals.device, buffer, NULL);
		R_FreeVulkanMemory (memory, num_allocations);
	}
}

/*
====================
R_CreateBuffers
====================
*/
size_t R_CreateBuffers (
	const int num_buffers, buffer_create_info_t *create_infos, vulkan_memory_t *memory, const VkFlags mem_requirements_mask, const VkFlags mem_preferred_mask,
	atomic_uint32_t *num_allocations, const char *memory_name)
{
	VkResult		   err;
	VkBufferUsageFlags usage_union = 0;

	qboolean get_device_address = false;
	for (int i = 0; i < num_buffers; ++i)
	{
		if (vulkan_globals.vk_get_buffer_device_address)
		{
			if (create_infos[i].address)
			{
				get_device_address = true;
				create_infos[i].usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
			}
		}
		usage_union |= create_infos[i].usage;
	}

	qboolean map_memory = false;
	size_t	 total_size = 0;
	for (int i = 0; i < num_buffers; ++i)
	{
		ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
		buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_create_info.size = create_infos[i].size;
		buffer_create_info.usage = create_infos[i].usage;
		err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, create_infos[i].buffer);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateBuffer failed with code %i", (int)err);

		GL_SetObjectName ((uint64_t)*create_infos[i].buffer, VK_OBJECT_TYPE_BUFFER, va ("%s buffer", create_infos[i].name));

		VkMemoryRequirements memory_requirements;
		vkGetBufferMemoryRequirements (vulkan_globals.device, *create_infos[i].buffer, &memory_requirements);
		const size_t alignment = q_max (memory_requirements.alignment, create_infos[i].alignment);
		total_size = q_align (total_size, alignment);
		total_size += memory_requirements.size;
		map_memory = map_memory || create_infos[i].mapped;
	}

	ZEROED_STRUCT (VkMemoryAllocateFlagsInfo, memory_allocate_flags_info);
	if (get_device_address)
	{
		memory_allocate_flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR;
		memory_allocate_flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	}

	uint32_t memory_type_bits = 0;
	{
		ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
		buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_create_info.size = total_size;
		buffer_create_info.usage = usage_union;
		VkBuffer dummy_buffer;
		err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, &dummy_buffer);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateBuffer failed with code %i", (int)err);
		VkMemoryRequirements memory_requirements;
		// Vulkan spec:
		// The memoryTypeBits member is identical for all VkBuffer objects created with the same value for the flags and usage members in the VkBufferCreateInfo
		// structure passed to vkCreateBuffer. Further, if usage1 and usage2 of type VkBufferUsageFlags are such that the bits set in usage2 are a subset of the
		// bits set in usage1, then the bits set in memoryTypeBits returned for usage1 must be a subset of the bits set in memoryTypeBits returned for usage2,
		// for all values of flags.
		vkGetBufferMemoryRequirements (vulkan_globals.device, dummy_buffer, &memory_requirements);
		memory_type_bits = memory_requirements.memoryTypeBits;
		vkDestroyBuffer (vulkan_globals.device, dummy_buffer, NULL);
	}

	ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.pNext = get_device_address ? &memory_allocate_flags_info : NULL;
	memory_allocate_info.allocationSize = total_size;
	memory_allocate_info.memoryTypeIndex = GL_MemoryTypeFromProperties (memory_type_bits, mem_requirements_mask, mem_preferred_mask);

	R_AllocateVulkanMemory (memory, &memory_allocate_info, VULKAN_MEMORY_TYPE_DEVICE, num_allocations);
	GL_SetObjectName ((uint64_t)memory->handle, VK_OBJECT_TYPE_DEVICE_MEMORY, memory_name);

	byte *mapped_base = NULL;
	if (map_memory)
	{
		err = vkMapMemory (vulkan_globals.device, memory->handle, 0, total_size, 0, (void **)&mapped_base);
		if (err != VK_SUCCESS)
			Sys_Error ("vkMapMemory failed with code %i", (int)err);
	}

	size_t current_offset = 0;
	for (int i = 0; i < num_buffers; ++i)
	{
		VkMemoryRequirements memory_requirements;
		vkGetBufferMemoryRequirements (vulkan_globals.device, *create_infos[i].buffer, &memory_requirements);
		const size_t alignment = q_max (memory_requirements.alignment, create_infos[i].alignment);
		current_offset = q_align (current_offset, alignment);

		err = vkBindBufferMemory (vulkan_globals.device, *create_infos[i].buffer, memory->handle, current_offset);
		if (err != VK_SUCCESS)
			Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);

		if (create_infos[i].mapped)
			*create_infos[i].mapped = mapped_base + current_offset;

		current_offset += memory_requirements.size;

		if (get_device_address && create_infos[i].address)
		{
			VkBufferDeviceAddressInfoKHR buffer_device_address_info;
			memset (&buffer_device_address_info, 0, sizeof (buffer_device_address_info));
			buffer_device_address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR;
			buffer_device_address_info.buffer = *create_infos[i].buffer;
			*create_infos[i].address = vulkan_globals.vk_get_buffer_device_address (vulkan_globals.device, &buffer_device_address_info);
		}
	}

	return total_size;
}

/*
====================
R_FreeBuffers
====================
*/
void R_FreeBuffers (const int num_buffers, VkBuffer *buffers, vulkan_memory_t *memory, atomic_uint32_t *num_allocations)
{
	for (int i = 0; i < num_buffers; ++i)
	{
		if (buffers[i] != VK_NULL_HANDLE)
			vkDestroyBuffer (vulkan_globals.device, buffers[i], NULL);
	}
	R_FreeVulkanMemory (memory, num_allocations);
}

/*
====================
R_PrintHeapStats
====================
*/
static void R_PrintHeapStats (const char *name, glheapstats_t *stats)
{
	Con_Printf (
		" %s:\n"
		"  segments: %" SDL_PRIu32 "\n"
		"  allocations: %" SDL_PRIu32 "\n"
		"  small allocations: %" SDL_PRIu32 "\n"
		"  block allocations: %" SDL_PRIu32 "\n"
		"  dedicated allocs: %" SDL_PRIu32 "\n"
		"  blocks used: %" SDL_PRIu32 "\n"
		"  blocks free: %" SDL_PRIu32 "\n"
		"  pages allocated: %" SDL_PRIu32 "\n"
		"  pages free: %" SDL_PRIu32 "\n"
		"  bytes allocated: %" SDL_PRIu64 "\n"
		"  bytes free: %" SDL_PRIu64 "\n"
		"  bytes wasted: %" SDL_PRIu64 " (%.3g%%)\n",
		name, stats->num_segments, stats->num_allocations, stats->num_small_allocations, stats->num_block_allocations, stats->num_dedicated_allocations,
		stats->num_blocks_used, stats->num_blocks_free, stats->num_pages_allocated, stats->num_pages_free, stats->num_bytes_allocated, stats->num_bytes_free,
		stats->num_bytes_wasted, ((double)stats->num_bytes_wasted / (double)stats->num_bytes_allocated) * 100.0f);
}

/*
====================
R_VulkanMemStats_f
====================
*/
void R_VulkanMemStats_f (void)
{
	const uint32_t num_tex_allocations = Atomic_LoadUInt32 (&num_vulkan_tex_allocations);
	const uint32_t num_bmodel_allocations = Atomic_LoadUInt32 (&num_vulkan_bmodel_allocations);
	const uint32_t num_mesh_allocations = Atomic_LoadUInt32 (&num_vulkan_mesh_allocations);
	const uint32_t num_misc_allocations = Atomic_LoadUInt32 (&num_vulkan_misc_allocations);
	const uint32_t num_dynbuf_allocations = Atomic_LoadUInt32 (&num_vulkan_dynbuf_allocations);

	Con_Printf (
		"Vulkan allocations: %" SDL_PRIu32 "\n",
		num_tex_allocations + num_bmodel_allocations + num_mesh_allocations + num_misc_allocations + num_dynbuf_allocations);
	Con_Printf (" Tex:    %" SDL_PRIu32 "\n", num_tex_allocations);
	Con_Printf (" BModel: %" SDL_PRIu32 "\n", num_bmodel_allocations);
	Con_Printf (" Mesh:   %" SDL_PRIu32 "\n", num_mesh_allocations);
	Con_Printf (" Misc:   %" SDL_PRIu32 "\n", num_misc_allocations);
	Con_Printf (" DynBuf: %" SDL_PRIu32 "\n", num_dynbuf_allocations);

	Con_Printf ("Heaps:\n");
	glheapstats_t tex_stats = TexMgr_GetHeapStats ();
	R_PrintHeapStats ("Tex", &tex_stats);
	glheapstats_t mesh_stats = R_GetMeshHeapStats ();
	R_PrintHeapStats ("Mesh", &mesh_stats);

	Con_Printf ("Descriptors:\n");
	Con_Printf (" Samplers: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_samplers));
	Con_Printf (" Combined image samplers: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_combined_image_samplers));
	Con_Printf (" Dynamic UBOs: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_ubos_dynamic));
	Con_Printf (" UBOs: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_ubos));
	Con_Printf (" Storage buffers: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_ubos_dynamic));
	Con_Printf (" Input attachments: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_storage_buffers));
	Con_Printf (" Storage images: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_storage_images));
	Con_Printf (" Sampled images: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_vulkan_sampled_images));
	Con_Printf (" Acceleration structures: %" SDL_PRIu32 "\n", Atomic_LoadUInt32 (&num_acceleration_structures));
	Con_Printf ("Device %" SDL_PRIu64 " MiB total\n", Atomic_LoadUInt64 (&total_device_vulkan_allocation_size) / 1024 / 1024);
	Con_Printf ("Host %" SDL_PRIu64 " MiB total\n", Atomic_LoadUInt64 (&total_host_vulkan_allocation_size) / 1024 / 1024);
}
