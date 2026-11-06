/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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

// r_alias.c -- alias model rendering

#include "quakedef.h"
#include "atomics.h"
#include "radiance_graphics.h"
#include "ecs_gpu_cull.h"
#include "editor_model_preview.h"
#include "radiance_receiver_model.h"

extern cvar_t r_drawflat, gl_fullbrights, r_lerpmodels, r_lerpmove, r_showtris; // johnfitz
extern cvar_t r_lerpturn;
extern cvar_t cl_gun_fovscale, cl_gun_x, cl_gun_y, cl_gun_z;
cvar_t r_ecs_batch = {"r_ecs_batch", "1", CVAR_NONE};
/* GPU culling remains opt-in until moving more of lighting off the CPU makes
 * it competitive with the existing CPU-culling path at high entity counts. */
cvar_t r_ecs_gpu_cull = {"r_ecs_gpu_cull", "0", CVAR_NONE};
extern cvar_t scr_speeds;

// up to 16 color translated skins
gltexture_t *playertextures[MAX_SCOREBOARD]; // johnfitz -- changed to an array of pointers

#define NUMVERTEXNORMALS 162

float r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT 16

typedef struct
{
	float	 model_matrix[16];
	float	 shade_vector[3];
	float	 blend_factor;
	float	 light_color[3];
	float	 entalpha;
	uint32_t flags;
} aliasubo_t;

typedef struct
{
	vec3_t origin;
	vec3_t light;
} alias_instance_t;

COMPILE_TIME_ASSERT (alias_instance_size, sizeof (alias_instance_t) == 24);

/* Written by the preparation task and consumed after its dependency releases
 * the entity-draw task. GPU resources themselves are owned by the frame slot. */
static qboolean ecs_gpu_ready;
static VkBuffer ecs_gpu_instances, ecs_gpu_indirect;
static VkDeviceSize ecs_gpu_instance_offset, ecs_gpu_indirect_offset;
static vec3_t ecs_gpu_shadevector;
static alias_instance_t *ecs_gpu_input;
static entity_t *ecs_gpu_entities;
static uint32_t ecs_gpu_count;
static int ecs_gpu_active_lights[MAX_DLIGHTS], ecs_gpu_active_count;

void R_DisableECSVisualGPU (void)
{
	ecs_gpu_ready = false;
}

qboolean R_ECSGPUPathActive (void)
{
	return ecs_gpu_ready;
}

/* Query on the main thread before constructing the frame task graph. */
qboolean R_ECSGPUShouldPrepare (void)
{
	return r_ecs_gpu_cull.value && !scr_speeds.value && r_drawentities.value &&
		R_ECSGPUAvailable () && CL_ECSVisualBatch (NULL) != 0;
}

void R_ECSGPUStats_f (void)
{
	Con_Printf ("ECS GPU cull: enabled=%u available=%u active=%u dispatches=%llu\n",
		(unsigned)(r_ecs_gpu_cull.value != 0), (unsigned)R_ECSGPUAvailable (), (unsigned)ecs_gpu_ready,
		(unsigned long long)R_ECSGPUDispatchCount ());
}

typedef struct
{
	float	 model_matrix[16];
	float	 shade_vector[3];
	float	 blend_factor;
	float	 light_color[3];
	float	 entalpha;
	uint32_t flags;
	uint32_t joints_offsets[2];
	uint32_t joints_bind_offset; // inverse bind pose, after the frames' poses (MD5_SkinFrames)
} md5ubo_t;

/*
=============
GLARB_GetXYZOffset

Returns the offset of the first vertex's meshxyz_t.xyz in the vbo for the given
model and pose.
=============
*/
static VkDeviceSize GLARB_GetXYZOffset (entity_t *e, aliashdr_t *hdr, int pose)
{
	const int xyzoffs = offsetof (meshxyz_t, xyz);
	return hdr->numverts_vbo * pose * sizeof (meshxyz_t) + xyzoffs;
}

/* Pointer ordering is only defined within one C array. Viewmodels and static
 * entities need not belong to cl.entities, so test slot identity using the
 * address offset and alignment before treating one as a player. */
static qboolean R_IsClientPlayerEntity (const entity_t *entity)
{
	if (!cl.entities || cl.maxclients < 1 || !entity) return false;
	const uintptr_t base = (uintptr_t)(const void *)cl.entities;
	const uintptr_t address = (uintptr_t)(const void *)entity;
	if (address <= base) return false;
	const uintptr_t offset = address - base;
	return offset % sizeof (*entity) == 0 && offset / sizeof (*entity) <= (uintptr_t)cl.maxclients;
}

/*
=============
GL_DrawAliasFrame -- ericw

Optimized alias model drawing codepath. This makes 1 draw call,
no vertex data is uploaded (it's already in the r_meshvbo and r_meshindexesvbo
static VBOs), and lerping and lighting is done in the vertex shader.

Supports optional fullbright pixels.

Based on code by MH from RMQEngine
=============
*/
static void R_CaptureReceiverModel (rc_receiver_model_capture_t *capture,const cb_context_t *cbx,
	const aliashdr_t *header,const VkBuffer *vertices,const VkDeviceSize *offsets,uint32_t vertex_count,
	const VkDescriptorSet *sets,uint32_t set_count,uint32_t uniform_offset,uint32_t flags,float alpha,qboolean alpha_test,bool reuse_eligible)
{
	if(!capture)return;
	rc_receiver_model_draw_t draw={.kind=header->poseverttype==PV_MD5?1u:header->poseverttype==PV_MD5_8?2u:0u,
		.index_buffer=header->index_buffer,.index_count=(uint32_t)header->numindexes,.vertex_count=vertex_count,
		.set_count=set_count,.uniform_offset=uniform_offset,.shader_flags=flags,.alpha=alpha,.alpha_test=alpha_test,.reuse_eligible=reuse_eligible,
		.fallback_pipeline=cbx->current_pipeline.handle,.fallback_layout=cbx->current_pipeline.layout.handle,
		.lighting_set=RCGraphics_FrameDescriptor()};
	memcpy(draw.vertices,vertices,vertex_count*sizeof(*vertices));memcpy(draw.vertex_offsets,offsets,vertex_count*sizeof(*offsets));
	memcpy(draw.sets,sets,set_count*sizeof(*sets));memcpy(draw.constants,vulkan_globals.view_projection_matrix,16*sizeof(float));
	float fog[4];Fog_GetColor(fog);memcpy(draw.constants+16,fog,3*sizeof(float));draw.constants[19]=Fog_GetDensity()/64.f;
	RCReceiverModelCapture_Add(capture,&draw);
}

static void GL_DrawAliasFrame (
	cb_context_t *cbx, entity_t *e, aliashdr_t *paliashdr, lerpdata_t lerpdata, gltexture_t *tx, gltexture_t *fb, float model_matrix[16], float entity_alpha,
	qboolean alphatest, vec3_t shadevector, vec3_t lightcolor, int showtris, const float *editor_projection)
{
	vulkan_pipeline_t pipeline;

	// only enable alpha management if entity have alpha or the surface texture has effective
	// non-opaque pixels:
	const bool has_alpha = (entity_alpha < 1.0f) || (tx->flags & TEXPREF_ALPHAPIXELS);
	const rc_graphics_t *radiance = (showtris || editor_projection) ? NULL : RCGraphics_Frame ();
	rc_receiver_model_capture_t *model_capture=NULL;
	const bool reuse_eligible=!has_alpha && entity_alpha==1.f && !r_fullbright_cheatsafe && !(r_lightmap_cheatsafe && r_fullbright.value);
	/* Keep every surface of this host-opaque model in its original stream.
	 * Native blended/debug surfaces retain their ordinary pipeline between
	 * eligible opaque draws, instead of being overwritten by a later batch. */
	if(radiance && e!=&cl.viewent && !showtris && !editor_projection &&
		cbx->subpass_type==SUBPASS_MAIN && cbx->receiver_model_capture && cbx->receiver_model_context)
	{
		model_capture=cbx->receiver_model_capture;cbx=cbx->receiver_model_context;
	}

	int pipeline_index;
	if (showtris == 0)
		pipeline_index = (has_alpha ? MODEL_PIPELINE_ALPHA_BLEND_BIT : 0) | (alphatest ? MODEL_PIPELINE_ALPHA_TEST_BIT : 0);
	else
		pipeline_index = (showtris >= 2) ? MODEL_PIPELINE_SHOWTRIS_DEPTH_TEST : MODEL_PIPELINE_SHOWTRIS;

	const qboolean oit_pass = cbx->subpass_type == SUBPASS_WBOIT || cbx->subpass_type == SUBPASS_MBOIT_MOMENTS || cbx->subpass_type == SUBPASS_MBOIT_COMPOSITE;
	if (oit_pass && (showtris != 0 || !has_alpha))
		return;

	if (paliashdr->poseverttype == PV_MD5 || paliashdr->poseverttype == PV_MD5_8)
	{
		vulkan_pipeline_t (*pipelines)[MODEL_PIPELINE_COUNT] =
			(paliashdr->poseverttype == PV_MD5_8) ? vulkan_globals.md5_8_pipelines : vulkan_globals.md5_pipelines;
		vulkan_pipeline_t *wboit_pipelines = (paliashdr->poseverttype == PV_MD5_8) ? vulkan_globals.md5_8_wboit_pipelines : vulkan_globals.md5_wboit_pipelines;
		vulkan_pipeline_t *mboit_moment_pipelines =
			(paliashdr->poseverttype == PV_MD5_8) ? vulkan_globals.md5_8_mboit_moment_pipelines : vulkan_globals.md5_mboit_moment_pipelines;
		vulkan_pipeline_t *mboit_composite_pipelines =
			(paliashdr->poseverttype == PV_MD5_8) ? vulkan_globals.md5_8_mboit_composite_pipelines : vulkan_globals.md5_mboit_composite_pipelines;

		pipeline = R_PipelineForSubpassType (
			cbx->subpass_type, pipelines[cbx->pipeline_variant][pipeline_index], wboit_pipelines[pipeline_index], mboit_moment_pipelines[pipeline_index],
			mboit_composite_pipelines[pipeline_index]);
	}
	else
		pipeline = R_PipelineForSubpassType (
			cbx->subpass_type, vulkan_globals.alias_pipelines[cbx->pipeline_variant][pipeline_index], vulkan_globals.alias_wboit_pipelines[pipeline_index],
			vulkan_globals.alias_mboit_moment_pipelines[pipeline_index], vulkan_globals.alias_mboit_composite_pipelines[pipeline_index]);

	if (radiance)
	{
		const int kind = paliashdr->poseverttype == PV_MD5 ? 1 : paliashdr->poseverttype == PV_MD5_8 ? 2 : 0;
		pipeline = RCGraphics_ModelPipeline (radiance, cbx->subpass_type, cbx->pipeline_variant, pipeline_index, kind);
	}
	if (editor_projection)
	{
		graphics_pipeline_t kind = paliashdr->poseverttype == PV_MD5 ? PIPELINE_EDITOR_MD5 :
			paliashdr->poseverttype == PV_MD5_8 ? PIPELINE_EDITOR_MD5_8 : PIPELINE_EDITOR_ALIAS;
		if (alphatest) kind = (graphics_pipeline_t)(kind + 1);
		R_BindGraphicsPipeline (cbx, kind); pipeline = cbx->current_pipeline;
		float constants[20] = {0}; memcpy (constants, editor_projection, 16 * sizeof (float));
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (constants), constants);
	}
	else R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	if (radiance)
		RCGraphics_BindFrame (cbx);

	float blend;

	if (lerpdata.pose1 != lerpdata.pose2)
		blend = lerpdata.blend;
	else // poses the same means either 1. the entity has paused its animation, or 2. r_lerpmodels is disabled
		blend = 0;

	switch (paliashdr->poseverttype)
	{
	case PV_QUAKE1:
	case PV_QUAKE3:
	{
		VkBuffer		uniform_buffer;
		uint32_t		uniform_offset;
		VkDescriptorSet ubo_set;
		aliasubo_t	   *ubo = (aliasubo_t *)R_UniformAllocate (sizeof (aliasubo_t), &uniform_buffer, &uniform_offset, &ubo_set);

		memcpy (ubo->model_matrix, model_matrix, 16 * sizeof (float));
		memcpy (ubo->shade_vector, shadevector, 3 * sizeof (float));
		ubo->blend_factor = blend;
		memcpy (ubo->light_color, lightcolor, 3 * sizeof (float));
		ubo->flags = (fb != NULL) ? 0x1 : 0x0;
		if (has_alpha && !editor_projection && showtris == 0) ubo->flags |= 0x20;
		if (radiance && e == &cl.viewent) ubo->flags |= 0x8;
		if (radiance && R_IsClientPlayerEntity (e)) ubo->flags |= 0x10;

		if (!editor_projection && (r_fullbright_cheatsafe || (r_lightmap_cheatsafe && r_fullbright.value)))
			ubo->flags |= 0x2;

		if (paliashdr->poseverttype == PV_QUAKE3)
			ubo->flags |= 0x4;

		ubo->entalpha = entity_alpha;

		VkDescriptorSet descriptor_sets[3] = {tx->descriptor_set, (fb != NULL) ? fb->descriptor_set : tx->descriptor_set, ubo_set};
		vulkan_globals.vk_cmd_bind_descriptor_sets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout.handle, 0, 3, descriptor_sets, 1, &uniform_offset);

		VkBuffer	 vertex_buffers[3] = {paliashdr->vertex_buffer, paliashdr->vertex_buffer, paliashdr->vertex_buffer};
		VkDeviceSize vertex_offsets[3] = {
			(unsigned)paliashdr->vbostofs, GLARB_GetXYZOffset (e, paliashdr, lerpdata.pose1), GLARB_GetXYZOffset (e, paliashdr, lerpdata.pose2)};
		vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 3, vertex_buffers, vertex_offsets);
		vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, paliashdr->index_buffer, 0, VK_INDEX_TYPE_UINT16);

		vulkan_globals.vk_cmd_draw_indexed (cbx->cb, paliashdr->numindexes, 1, 0, 0, 0);
		R_CaptureReceiverModel(model_capture,cbx,paliashdr,vertex_buffers,vertex_offsets,3,descriptor_sets,3,
			uniform_offset,ubo->flags,entity_alpha,alphatest,reuse_eligible);
		break;
	}
	case PV_MD5:
	case PV_MD5_8:
	{
		VkBuffer		uniform_buffer;
		uint32_t		uniform_offset;
		VkDescriptorSet ubo_set;
		md5ubo_t	   *ubo = (md5ubo_t *)R_UniformAllocate (sizeof (md5ubo_t), &uniform_buffer, &uniform_offset, &ubo_set);

		memcpy (ubo->model_matrix, model_matrix, 16 * sizeof (float));
		memcpy (ubo->shade_vector, shadevector, 3 * sizeof (float));
		ubo->blend_factor = blend;
		memcpy (ubo->light_color, lightcolor, 3 * sizeof (float));
		ubo->flags = (fb != NULL) ? 0x1 : 0x0;
		if (has_alpha && !editor_projection && showtris == 0) ubo->flags |= 0x20;
		if (radiance && e == &cl.viewent) ubo->flags |= 0x8;
		if (radiance && R_IsClientPlayerEntity (e)) ubo->flags |= 0x10;
		if (!editor_projection && (r_fullbright_cheatsafe || (r_lightmap_cheatsafe && r_fullbright.value)))
			ubo->flags |= 0x2;
		ubo->entalpha = entity_alpha;
		ubo->joints_offsets[0] = lerpdata.pose1 * paliashdr->numjoints;
		ubo->joints_offsets[1] = lerpdata.pose2 * paliashdr->numjoints;
		ubo->joints_bind_offset = paliashdr->numframes * paliashdr->numjoints;

		VkDescriptorSet descriptor_sets[4] = {tx->descriptor_set, (fb != NULL) ? fb->descriptor_set : tx->descriptor_set, ubo_set, paliashdr->joints_set};
		vulkan_globals.vk_cmd_bind_descriptor_sets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout.handle, 0, 4, descriptor_sets, 1, &uniform_offset);

		VkBuffer	 vertex_buffers[1] = {paliashdr->vertex_buffer};
		VkDeviceSize vertex_offsets[1] = {0};
		vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 1, vertex_buffers, vertex_offsets);
		vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, paliashdr->index_buffer, 0, VK_INDEX_TYPE_UINT16);

		//
		vulkan_globals.vk_cmd_draw_indexed (cbx->cb, paliashdr->numindexes, 1, 0, 0, 0);
		R_CaptureReceiverModel(model_capture,cbx,paliashdr,vertex_buffers,vertex_offsets,1,descriptor_sets,4,
			uniform_offset,ubo->flags,entity_alpha,alphatest,reuse_eligible);
		break;
	}
	default:
		assert (false);
	}
}

/*
=================
R_EntityPoseAt

Pose displayed by the given model frame at the given time (framegroup poses
advance with time).
=================
*/
static int R_EntityPoseAt (aliashdr_t *paliashdr, int frame, double time)
{
	if ((frame >= paliashdr->numframes) || (frame < 0))
		frame = 0;

	int posenum = paliashdr->frames[frame].firstpose;
	int numposes = paliashdr->frames[frame].numposes;
	if (numposes > 1)
		posenum += (int)(time / paliashdr->frames[frame].interval) % numposes;
	return posenum;
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping

Computes pose1/pose2/blend from the parse-side interpolation state and
cl.time. Does not modify the entity.
=================
*/
void R_SetupAliasFrame (const entity_t *e, aliashdr_t *paliashdr, lerpdata_t *lerpdata)
{
	int frame = e->frame;
	if ((frame >= paliashdr->numframes) || (frame < 0))
		frame = 0;

	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		int	   numposes = paliashdr->frames[frame].numposes;
		double change_time = e->lerp.frame_change_time;

		if (numposes > 1)
		{
			// framegroup: poses advance with cl.time; lerp from the previous
			// group pose unless the entity entered this frame more recently
			double interval = paliashdr->frames[frame].interval;
			int	   idx = (int)(cl.time / interval);
			double boundary = idx * interval;

			lerpdata->pose2 = paliashdr->frames[frame].firstpose + idx % numposes;
			if (change_time > boundary)
			{
				lerpdata->pose1 = R_EntityPoseAt (paliashdr, e->lerp.prev_frame, change_time);
				lerpdata->blend = CLAMP (0, (cl.time - change_time) / interval, 1);
			}
			else
			{
				lerpdata->pose1 = paliashdr->frames[frame].firstpose + (idx + numposes - 1) % numposes;
				lerpdata->blend = CLAMP (0, (cl.time - boundary) / interval, 1);
			}
		}
		else if (change_time > 0)
		{
			double duration = (e->lerp.frame_duration > 0) ? e->lerp.frame_duration : 0.1;
			lerpdata->pose2 = paliashdr->frames[frame].firstpose;
			lerpdata->pose1 = R_EntityPoseAt (paliashdr, e->lerp.prev_frame, change_time);
			lerpdata->blend = CLAMP (0, (cl.time - change_time) / duration, 1);
		}
		else
		{
			lerpdata->pose2 = paliashdr->frames[frame].firstpose;
			lerpdata->pose1 = lerpdata->pose2;
			lerpdata->blend = 1;
		}

		// Clamp poses (safety check for Quake1 models)
		if (paliashdr->poseverttype == PV_QUAKE1)
		{
			if (lerpdata->pose2 >= paliashdr->numposes || lerpdata->pose2 < 0)
			{
				Con_DPrintf ("R_AliasSetupFrame: invalid current pose %d (%d total) for '%s'\n", lerpdata->pose2, paliashdr->numposes, e->model->name);
				lerpdata->pose2 = 0;
			}
			if (lerpdata->pose1 >= paliashdr->numposes || lerpdata->pose1 < 0)
			{
				Con_DPrintf ("R_AliasSetupFrame: invalid prev pose %d (%d total) for '%s'\n", lerpdata->pose1, paliashdr->numposes, e->model->name);
				lerpdata->pose1 = lerpdata->pose2;
			}
		}
		else if (paliashdr->poseverttype == PV_MD5 || paliashdr->poseverttype == PV_MD5_8)
		{
			// MD5 uses numframes for joint matrices
			if (lerpdata->pose1 >= paliashdr->numframes || lerpdata->pose1 < 0)
				lerpdata->pose1 = 0;
			if (lerpdata->pose2 >= paliashdr->numframes || lerpdata->pose2 < 0)
				lerpdata->pose2 = 0;
		}
	}
	else
	{
		lerpdata->blend = 1;
		lerpdata->pose1 = R_EntityPoseAt (paliashdr, frame, cl.time);
		lerpdata->pose2 = lerpdata->pose1;
	}
}

/*
=================
R_GetEntityLerpedTransform

Computes lerped origin/angles from the parse-side interpolation state and
cl.time. Does not modify the entity.

Attached entities (tagentity) use their post-attachment origin/angles, which
only exist on the entity itself.
=================
*/
void R_GetEntityLerpedTransform (const entity_t *e, vec3_t out_origin, vec3_t out_angles)
{
	if (r_lerpmove.value && e != &cl.viewent && e->lerp.movestep && !e->netstate.tagentity && e->lerp.move_change_time > 0)
	{
		double change_time = e->lerp.move_change_time;
		double duration = (e->lerp.move_duration > 0) ? e->lerp.move_duration : 0.1;
		float  blend = CLAMP (0, (cl.time - change_time) / duration, 1);

		// translation
		vec3_t d;
		VectorSubtract (e->msg_origins[0], e->lerp.prev_origin, d);
		out_origin[0] = e->lerp.prev_origin[0] + d[0] * blend;
		out_origin[1] = e->lerp.prev_origin[1] + d[1] * blend;
		out_origin[2] = e->lerp.prev_origin[2] + d[2] * blend;

		// rotation (if enabled); EF_ROTATE angles are client-side and only exist on the entity
		if (r_lerpturn.value && !(e->model->flags & EF_ROTATE))
		{
			VectorSubtract (e->msg_angles[0], e->lerp.prev_angles, d);
			for (int i = 0; i < 3; i++)
			{
				if (d[i] > 180)
					d[i] -= 360;
				if (d[i] < -180)
					d[i] += 360;
			}
			out_angles[0] = e->lerp.prev_angles[0] + d[0] * blend;
			out_angles[1] = e->lerp.prev_angles[1] + d[1] * blend;
			out_angles[2] = e->lerp.prev_angles[2] + d[2] * blend;
		}
		else
		{
			VectorCopy (e->angles, out_angles);
		}
	}
	else // don't lerp
	{
		VectorCopy (e->origin, out_origin);
		VectorCopy (e->angles, out_angles);
	}
}

/*
=================
R_SetupAliasLighting -- johnfitz -- broken out from R_DrawAliasModel and rewritten
=================
*/
static void R_SetupAliasLightingFiltered (entity_t *e, vec3_t *shadevector, vec3_t *lightcolor, const int *active_lights, int active_count)
{
	vec3_t dist;
	float  add;
	int	   i;
	int	   quantizedangle;
	float  radiansangle;

	// if the initial trace is completely black, try again from above
	// this helps with models whose origin is slightly below ground level
	// (e.g. some of the candles in the DOTM start map)
	if (!R_LightPoint (e->origin, 0.f, &e->lightcache, lightcolor))
		R_LightPoint (e->origin, e->model->maxs[2] * 0.5f, &e->lightcache, lightcolor);

	// add dlights
	for (int light_index = 0; light_index < (active_lights ? active_count : MAX_DLIGHTS); light_index++)
	{
		i = active_lights ? active_lights[light_index] : light_index;
		if (cl_dlights[i].die >= cl.time)
		{
			VectorSubtract (e->origin, cl_dlights[i].origin, dist);
			add = cl_dlights[i].radius - VectorLength (dist);
			if (add > 0)
			{
				if (cl_dlights[i].cone_cos > -1.0f)
				{
					vec3_t dir;
					VectorCopy (dist, dir);
					VectorNormalize (dir);
					const float cone_cos = cl_dlights[i].cone_cos;
					const float cone_dot = DotProduct (dir, cl_dlights[i].cone_dir);
					float		cone_scale;
					if (cl_dlights[i].kex_intensity > 0.0f)
					{
						// linear falloff from cone axis to edge, matches update_lightmap.inc
						if (cone_dot < cone_cos)
							continue;
						cone_scale = 1.0f - (1.0f - cone_dot) / (1.0f - cone_cos);
					}
					else
					{
						// soft edged spotlight falloff, matches update_lightmap.inc
						const float cone_soft = cone_cos + ((1.0f - cone_cos) * 0.25f);
						cone_scale = CLAMP (0.0f, (cone_dot - cone_cos) / q_max (cone_soft - cone_cos, 0.0001f), 1.0f);
					}
					add *= cone_scale;
					if (add <= 0.0f)
						continue;
				}
				if (cl_dlights[i].kex_intensity > 0.0f)
				{
					// KEX falloff: range-normalized, scaled by intensity (matches update_lightmap.inc,
					// sans the Lambert term since alias models use their own shading)
					add *= cl_dlights[i].kex_intensity * 0.5f * (256.0f / cl_dlights[i].radius);
				}
				VectorMA (*lightcolor, add, cl_dlights[i].color, *lightcolor);
			}
		}
	}

	// minimum light value on gun (24)
	if (e == &cl.viewent)
	{
		add = 72.0f - ((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]);
		if (add > 0.0f)
		{
			(*lightcolor)[0] += add / 3.0f;
			(*lightcolor)[1] += add / 3.0f;
			(*lightcolor)[2] += add / 3.0f;
		}
	}

	// minimum light value on players (8)
	if (R_IsClientPlayerEntity (e))
	{
		add = 24.0f - ((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]);
		if (add > 0.0f)
		{
			(*lightcolor)[0] += add / 3.0f;
			(*lightcolor)[1] += add / 3.0f;
			(*lightcolor)[2] += add / 3.0f;
		}
	}

	// clamp lighting so it doesn't overbright as much (96)
	add = 288.0f / ((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]);
	if (add < 1.0f)
		VectorScale ((*lightcolor), add, (*lightcolor));

	quantizedangle = ((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);

	// ericw -- shadevector is passed to the shader to compute shadedots inside the
	// shader, see GLAlias_CreateShaders()
	radiansangle = (quantizedangle / 16.0) * 2.0 * 3.14159;
	(*shadevector)[0] = cos (-radiansangle);
	(*shadevector)[1] = sin (-radiansangle);
	(*shadevector)[2] = 1;
	VectorNormalize (*shadevector);
	// ericw --

	VectorScale ((*lightcolor), 1.0f / 200.0f, (*lightcolor));
}

static void R_SetupAliasLighting (entity_t *e, vec3_t *shadevector, vec3_t *lightcolor)
{
	R_SetupAliasLightingFiltered (e, shadevector, lightcolor, NULL, 0);
}

/*
=================
R_DrawAliasModel -- johnfitz -- almost completely rewritten
=================
*/
static void R_AliasModelMatrix (entity_t *e, const aliashdr_t *paliashdr, lerpdata_t *lerpdata, float model_matrix[16])
{
	IdentityMatrix (model_matrix);

	float fovscale = 1.0f;
	if (e == &cl.viewent && r_refdef.basefov > 90.f && cl_gun_fovscale.value)
	{
		fovscale = tan (r_refdef.basefov * (0.5f * M_PI / 180.f));
		fovscale = 1.f + (fovscale - 1.f) * cl_gun_fovscale.value;
	}

	vec3_t origin;
	VectorCopy (lerpdata->origin, origin);
	if (e == &cl.viewent)
	{
		VectorMA (origin, cl_gun_x.value * paliashdr->scale[0] * fovscale, vright, origin);
		VectorMA (origin, cl_gun_y.value * paliashdr->scale[1] * fovscale, vup, origin);
		VectorMA (origin, cl_gun_z.value * paliashdr->scale[2], vpn, origin);
	}
	R_RotateForEntityScale (model_matrix, origin, lerpdata->angles, R_EntityModelScale (e));

	float translation_matrix[16];
	TranslationMatrix (translation_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscale, paliashdr->scale_origin[2] * fovscale);
	MatrixMultiply (model_matrix, translation_matrix);

	float scale_matrix[16];
	ScaleMatrix (scale_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscale, paliashdr->scale[2] * fovscale);
	MatrixMultiply (model_matrix, scale_matrix);
}

qboolean R_DrawEditorAlias (cb_context_t *cbx, qmodel_t *model, aliashdr_t *header,
	const editor_map_model_t *placement, const float projection[16])
{
	if (!model || !header || !placement || !projection || model->needload ||
		!isfinite (placement->modelscale) || placement->modelscale <= 0 || placement->modelscale > 1024) return false;
	for (const aliashdr_t *surface = header; surface; surface = surface->nextsurface)
	{
		graphics_pipeline_t kind = surface->poseverttype == PV_MD5 ? PIPELINE_EDITOR_MD5 :
			surface->poseverttype == PV_MD5_8 ? PIPELINE_EDITOR_MD5_8 : PIPELINE_EDITOR_ALIAS;
		if (!R_HasGraphicsPipeline (cbx, kind) || !R_HasGraphicsPipeline (cbx, (graphics_pipeline_t)(kind + 1))) return false;
	}
	entity_t entity = {0}; entity.model = model; entity.skinnum = placement->skin;
	entity.modelscale_override = (float)placement->modelscale;
	lerpdata_t pose = {0};
	for (unsigned a = 0; a < 3; ++a) { pose.origin[a] = (float)placement->origin[a]; pose.angles[a] = (float)placement->angles[a]; }
	float rotation[16]; IdentityMatrix (rotation); vec3_t zero = {0};
	R_RotateForEntityScale (rotation, zero, pose.angles, 1);
	const vec3_t world_light = {.30f, -.40f, .70f};
	vec3_t shade, color = {.36f, .36f, .36f};
	for (unsigned a = 0; a < 3; ++a)
		shade[a] = rotation[a * 4] * world_light[0] + rotation[a * 4 + 1] * world_light[1] + rotation[a * 4 + 2] * world_light[2];
	for (aliashdr_t *surface = header; surface; surface = surface->nextsurface)
	{
		gltexture_t *texture = surface->gltextures[placement->skin][0];
		if (!texture) texture = nulltexture;
		gltexture_t *fullbright = gl_fullbrights.value ? surface->fbtextures[placement->skin][0] : NULL;
		float matrix[16]; R_AliasModelMatrix (&entity, surface, &pose, matrix);
		qboolean cutout = (model->flags & MF_HOLEY) || (texture->flags & TEXPREF_ALPHAPIXELS);
		GL_DrawAliasFrame (cbx, &entity, surface, pose, texture, fullbright, matrix, 1, cutout, shade, color, 0, projection);
	}
	return true;
}

void R_DrawAliasModel (cb_context_t *cbx, entity_t *e, int *aliaspolys)
{
	aliashdr_t	*paliashdr;
	int			 anim, skinnum = e->skinnum;
	gltexture_t *tx, *fb;
	lerpdata_t	 lerpdata;

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = e->compiled_instance ? Mod_Extradata_Exact (e->model, skinnum) : Mod_Extradata_CheckSkin (e->model, skinnum);
	if (!paliashdr) return;

	qboolean alphatest = !!(e->model->flags & MF_HOLEY);

	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_GetEntityLerpedTransform (e, lerpdata.origin, lerpdata.angles);

	//
	// cull it
	//
	if (R_CullModelForEntity (e))
		return;

	//
	// transform it
	//
	float model_matrix[16];
	R_AliasModelMatrix (e, paliashdr, &lerpdata, model_matrix);

	//
	// set up for alpha blending
	//
	float entalpha;
	if (r_lightmap_cheatsafe)
		entalpha = 1;
	else
		entalpha = ENTALPHA_DECODE (e->alpha);
	if (entalpha == 0)
		return;

	//
	// set up lighting
	//
	vec3_t shadevector, lightcolor;
	R_SetupAliasLighting (e, &shadevector, &lightcolor);

	// Draw each surface of the model independently:
	for (aliashdr_t *hdr = paliashdr; hdr != NULL; hdr = hdr->nextsurface)
	{
		//
		// set up textures
		//
		anim = e->compiled_instance ? 0 : (int)(cl.time * 10) & 3;
		if ((skinnum >= hdr->numskins) || (skinnum < 0))
		{
			Con_DPrintf ("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, e->model->name);
			// ericw -- display skin 0 for winquake compatibility
			skinnum = 0;
		}
		tx = hdr->gltextures[skinnum][anim];
		fb = hdr->fbtextures[skinnum][anim];

		if (e->colormap != vid.colormap && !gl_nocolors.value)
			if ((uintptr_t)e >= (uintptr_t)&cl.entities[1] && (uintptr_t)e <= (uintptr_t)&cl.entities[cl.maxclients] && playertextures[e - cl.entities - 1])
				tx = playertextures[e - cl.entities - 1];

		// if there are no texture, force the grey one. (a.k.a lightmap).
		if (tx == NULL)
		{
			tx = greytexture;
			fb = NULL;
		}

		if (!gl_fullbrights.value)
			fb = NULL;

		if (r_fullbright_cheatsafe)
		{
			lightcolor[0] = 0.5f;
			lightcolor[1] = 0.5f;
			lightcolor[2] = 0.5f;
		}
		if (r_lightmap_cheatsafe)
		{
			tx = greytexture;
			fb = NULL;
			if (r_fullbright.value)
			{
				lightcolor[0] = 1.0f;
				lightcolor[1] = 1.0f;
				lightcolor[2] = 1.0f;
			}
		}

		//
		// draw it
		//
		GL_DrawAliasFrame (cbx, e, hdr, lerpdata, tx, fb, model_matrix, entalpha, alphatest, shadevector, lightcolor, false, NULL);

		// update polycounts
		*aliaspolys += hdr->numtris;
	} // e for each surface
}

/* Only the local ECS visual array uses this fast path. All entities in that
 * array have the same static model, skin, unit scale and zero rotation. Keep
 * unusual models and the editor's wireframe passes on the normal renderer. */
qboolean R_ECSVisualBatchSupported (qmodel_t *model)
{
	if (!r_ecs_batch.value || r_showtris.value || !model || model->type != mod_alias || (model->flags & MF_HOLEY))
		return false;
	for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
		if (!vulkan_globals.alias_instanced_pipelines[variant].handle) return false;
	aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata_CheckSkin (model, 0);
	if (!hdr || hdr->poseverttype != PV_QUAKE1 || hdr->numposes != 1 || hdr->numframes != 1 || hdr->nextsurface || hdr->numskins < 1)
		return false;
	for (int i = 0; i < MAX_FRAMEGROUPS; ++i)
		if (hdr->gltextures[0][i] && (hdr->gltextures[0][i]->flags & TEXPREF_ALPHAPIXELS)) return false;
	return true;
}

/* The renderer invokes this after frame setup, dynamic-light flushing, and the
 * frame-slot fence. BSP light samples remain on the CPU so every instance gets
 * exactly the same lighting as the existing instanced renderer. */
void R_PrepareECSVisualGPU (void *unused)
{
	entity_t *entities;
	uint32_t count;
	aliashdr_t *hdr;
	alias_instance_t *instances;
	float planes[4][4];
	(void)unused;
	ecs_gpu_ready = false;
	R_ECSGPUReset ();
	if (!r_ecs_gpu_cull.value || scr_speeds.value || !r_drawentities.value ||
		!(count = CL_ECSVisualBatch (&entities)) || !entities)
		return;
	hdr = (aliashdr_t *)Mod_Extradata_CheckSkin (entities[0].model, 0);
	if (!hdr || !R_ECSGPUAllocate (count, (uint32_t)hdr->numindexes, (void **)&instances,
		&ecs_gpu_instances, &ecs_gpu_instance_offset, &ecs_gpu_indirect, &ecs_gpu_indirect_offset))
		return;
	for (int i = 0; i < 4; ++i)
	{
		VectorCopy (frustum[i].normal, planes[i]);
		planes[i][3] = frustum[i].dist;
	}
	R_ECSGPUSetPlanes (planes, entities[0].model->mins, entities[0].model->maxs);
	ecs_gpu_active_count = 0;
	for (int i = 0; i < MAX_DLIGHTS; ++i)
		if (cl_dlights[i].die >= cl.time) ecs_gpu_active_lights[ecs_gpu_active_count++] = i;
	ecs_gpu_entities = entities;
	ecs_gpu_input = instances;
	ecs_gpu_count = count;
	/* Batched visuals have zero rotation. Compute the common shade direction
	 * independently of visibility: the first lighting partition may be wholly
	 * outside the frustum while another one remains visible. */
	ecs_gpu_shadevector[0] = cosf (0.0f);
	ecs_gpu_shadevector[1] = sinf (-0.0f);
	ecs_gpu_shadevector[2] = 1.0f;
	VectorNormalize (ecs_gpu_shadevector);
	ecs_gpu_ready = true;
}

/* Upload every origin. An outside test on the union of a block's AABBs only
 * avoids its CPU light sampling; the GPU still performs every instance's
 * visibility test and builds the draw. The maximum possible shader tolerance
 * within the union, multiplied by four plus a small absolute allowance, keeps
 * this test conservative even at large coordinates and frustum boundaries. */
static qboolean R_ECSGPUUploadLightingBlock (uint32_t begin, uint32_t end)
{
	vec3_t mins = {0}, maxs = {0};
	float planes[4][4];
	qboolean finite = true;
	for (uint32_t i = begin; i < end; ++i)
	{
		entity_t *entity = &ecs_gpu_entities[i];
		VectorCopy (entity->origin, ecs_gpu_input[i].origin);
		memset (ecs_gpu_input[i].light, 0, sizeof (ecs_gpu_input[i].light));
		for (int axis = 0; axis < 3; ++axis)
		{
			float lo = entity->origin[axis] + entity->model->mins[axis];
			float hi = entity->origin[axis] + entity->model->maxs[axis];
			if (!isfinite (lo) || !isfinite (hi)) finite = false;
			if (i == begin || lo < mins[axis]) mins[axis] = lo;
			if (i == begin || hi > maxs[axis]) maxs[axis] = hi;
		}
	}
	if (!finite) return false;
	for (int p = 0; p < 4; ++p)
	{
		VectorCopy (frustum[p].normal, planes[p]);
		planes[p][3] = frustum[p].dist;
	}
	return ECSGPULightingBoundsHidden (mins, maxs, planes);
}

/* Parallel partitions fill disjoint records; the task graph joins them before
 * recording the draw. The original worker stride and per-entity light caches
 * remain independent, just as in the CPU-culling batch path. */
void R_FillECSVisualGPU (int index, void *unused)
{
	vec3_t shadevector = {0, 0, 0};
	(void)unused;
	if (!ecs_gpu_ready || index < 0 || index >= NUM_ENTITIES_CBX)
		return;
	uint32_t begin = (uint32_t)((uint64_t)ecs_gpu_count * (unsigned)index / NUM_ENTITIES_CBX);
	uint32_t end = (uint32_t)((uint64_t)ecs_gpu_count * ((unsigned)index + 1) / NUM_ENTITIES_CBX);
	for (uint32_t block = begin; block < end; block += 64)
	{
		const uint32_t block_end = q_min (block + 64, end);
		if (R_ECSGPUUploadLightingBlock (block, block_end)) continue;
		for (uint32_t i = block; i < block_end; ++i)
		{
			R_SetupAliasLightingFiltered (&ecs_gpu_entities[i], &shadevector, &ecs_gpu_input[i].light,
				ecs_gpu_active_lights, ecs_gpu_active_count);
			if (r_fullbright_cheatsafe)
				ecs_gpu_input[i].light[0] = ecs_gpu_input[i].light[1] = ecs_gpu_input[i].light[2] = 0.5f;
			if (r_lightmap_cheatsafe && r_fullbright.value)
				ecs_gpu_input[i].light[0] = ecs_gpu_input[i].light[1] = ecs_gpu_input[i].light[2] = 1;
		}
	}
}

void R_DrawECSVisualBatch (cb_context_t *cbx, unsigned part, unsigned parts)
{
	entity_t *entities;
	uint32_t count = CL_ECSVisualBatch (&entities);
	if (!count || !parts || part >= parts || !r_drawentities.value || !entities || cbx->subpass_type != SUBPASS_MAIN) return;
	const qboolean gpu = ecs_gpu_ready;
	if (gpu && part != 0) return;
	uint32_t begin = gpu ? 0 : (uint32_t)((uint64_t)count * part / parts);
	uint32_t end = gpu ? count : (uint32_t)((uint64_t)count * (part + 1) / parts);
	if (begin == end) return;
	aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata_CheckSkin (entities[0].model, 0);
	if (!hdr) return;
	const int anim = (int)(cl.time * 10) & 3;
	gltexture_t *tx = hdr->gltextures[0][anim], *fb = hdr->fbtextures[0][anim];
	if (!tx) { tx = greytexture; fb = NULL; }
	if (!gl_fullbrights.value) fb = NULL;
	if (r_lightmap_cheatsafe) { tx = greytexture; fb = NULL; }

	VkBuffer instance_buffer; VkDeviceSize instance_offset;
	uint32_t visible = 0;
	vec3_t shadevector = {0, 0, 0};
	if (gpu)
	{
		instance_buffer = ecs_gpu_instances;
		instance_offset = ecs_gpu_instance_offset;
		VectorCopy (ecs_gpu_shadevector, shadevector);
	}
	else
	{
		alias_instance_t *instances = (alias_instance_t *)R_VertexAllocate ((int)((end - begin) * sizeof (*instances)), &instance_buffer, &instance_offset);
		int active_lights[MAX_DLIGHTS], active_count = 0;
		for (int i = 0; i < MAX_DLIGHTS; ++i)
			if (cl_dlights[i].die >= cl.time) active_lights[active_count++] = i;
		for (uint32_t i = begin; i < end; ++i)
		{
			entity_t *e = &entities[i];
			if (R_CullModelForEntity (e)) continue;
			alias_instance_t *instance = &instances[visible++];
			VectorCopy (e->origin, instance->origin);
			R_SetupAliasLightingFiltered (e, &shadevector, &instance->light, active_lights, active_count);
			if (r_fullbright_cheatsafe) instance->light[0] = instance->light[1] = instance->light[2] = 0.5f;
			if (r_lightmap_cheatsafe && r_fullbright.value) instance->light[0] = instance->light[1] = instance->light[2] = 1;
		}
		Atomic_AddUInt32 (&rs_aliaspasses, end - begin);
		Atomic_AddUInt32 (&rs_aliaspolys, visible * (uint32_t)hdr->numtris);
		if (!visible) return;
	}
	VkBuffer uniform_buffer; uint32_t uniform_offset; VkDescriptorSet ubo_set;
	aliasubo_t *ubo = (aliasubo_t *)R_UniformAllocate (sizeof (*ubo), &uniform_buffer, &uniform_offset, &ubo_set);
	IdentityMatrix (ubo->model_matrix);
	float matrix[16];
	TranslationMatrix (matrix, hdr->scale_origin[0], hdr->scale_origin[1], hdr->scale_origin[2]);
	MatrixMultiply (ubo->model_matrix, matrix);
	ScaleMatrix (matrix, hdr->scale[0], hdr->scale[1], hdr->scale[2]);
	MatrixMultiply (ubo->model_matrix, matrix);
	VectorCopy (shadevector, ubo->shade_vector);
	memset (ubo->light_color, 0, sizeof (ubo->light_color));
	ubo->blend_factor = 0;
	ubo->entalpha = 1;
	ubo->flags = (fb ? 0x1 : 0) | ((r_fullbright_cheatsafe || (r_lightmap_cheatsafe && r_fullbright.value)) ? 0x2 : 0);
	const rc_graphics_t *radiance = RCGraphics_Frame ();
	vulkan_pipeline_t pipeline = radiance ? radiance->instanced[cbx->pipeline_variant] : vulkan_globals.alias_instanced_pipelines[cbx->pipeline_variant];
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	if (radiance)
		RCGraphics_BindFrame (cbx);
	VkDescriptorSet sets[3] = {tx->descriptor_set, fb ? fb->descriptor_set : tx->descriptor_set, ubo_set};
	vulkan_globals.vk_cmd_bind_descriptor_sets (cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout.handle, 0, 3, sets, 1, &uniform_offset);
	VkBuffer buffers[4] = {hdr->vertex_buffer, hdr->vertex_buffer, hdr->vertex_buffer, instance_buffer};
	VkDeviceSize offsets[4] = {(unsigned)hdr->vbostofs, GLARB_GetXYZOffset (entities, hdr, 0), GLARB_GetXYZOffset (entities, hdr, 0), instance_offset};
	vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 4, buffers, offsets);
	vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, hdr->index_buffer, 0, VK_INDEX_TYPE_UINT16);
	if (gpu)
		vulkan_globals.vk_cmd_draw_indexed_indirect (cbx->cb, ecs_gpu_indirect, ecs_gpu_indirect_offset, 1, sizeof (VkDrawIndexedIndirectCommand));
	else
		vulkan_globals.vk_cmd_draw_indexed (cbx->cb, hdr->numindexes, visible, 0, 0, 0);
}

// johnfitz -- values for shadow matrix
#define SHADOW_SKEW_X -0.7 // skew along x axis. -0.7 to mimic glquake shadows
#define SHADOW_SKEW_Y 0	   // skew along y axis. 0 to mimic glquake shadows
#define SHADOW_VSCALE 0	   // 0=completely flat
#define SHADOW_HEIGHT 0.1  // how far above the floor to render the shadow
// johnfitz

/*
=================
R_DrawAliasModel_ShowTris -- johnfitz
=================
*/
void R_DrawAliasModel_ShowTris (cb_context_t *cbx, entity_t *e)
{
	aliashdr_t *paliashdr;
	lerpdata_t	lerpdata;

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = e->compiled_instance ? Mod_Extradata_Exact (e->model, e->skinnum) : Mod_Extradata_CheckSkin (e->model, e->skinnum);
	if (!paliashdr) return;

	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_GetEntityLerpedTransform (e, lerpdata.origin, lerpdata.angles);

	//
	// cull it
	//
	if (R_CullModelForEntity (e))
		return;

	//
	// transform it
	//
	float model_matrix[16];
	R_AliasModelMatrix (e, paliashdr, &lerpdata, model_matrix);

	vec3_t shadevector = {0.0f, 0.0f, 0.0f};
	vec3_t lightcolor = {0.0f, 0.0f, 0.0f};
	// Draw each surface of the model independently:
	for (aliashdr_t *hdr = paliashdr; hdr != NULL; hdr = hdr->nextsurface)
	{
		GL_DrawAliasFrame (cbx, e, hdr, lerpdata, nulltexture, nulltexture, model_matrix, 0.0f, false, shadevector, lightcolor, r_showtris.value, NULL);
	}
}

/*
=================
R_DrawAliasModel_ShowSkel
=================
*/
void R_DrawAliasModel_ShowSkel (cb_context_t *cbx, entity_t *e)
{
	aliashdr_t *paliashdr;
	lerpdata_t	lerpdata;

	paliashdr = e->compiled_instance ? Mod_Extradata_Exact (e->model, e->skinnum) : Mod_Extradata_CheckSkin (e->model, e->skinnum);
	if (!paliashdr) return;
	if ((paliashdr->poseverttype != PV_MD5 && paliashdr->poseverttype != PV_MD5_8) || paliashdr->skeleton_index_buffer == VK_NULL_HANDLE ||
		paliashdr->num_skeleton_indexes <= 0 || paliashdr->joints_set == VK_NULL_HANDLE)
		return;

	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_GetEntityLerpedTransform (e, lerpdata.origin, lerpdata.angles);

	if (R_CullModelForEntity (e))
		return;

	float model_matrix[16];
	R_AliasModelMatrix (e, paliashdr, &lerpdata, model_matrix);

	float blend = 0.0f;
	if (lerpdata.pose1 != lerpdata.pose2)
		blend = lerpdata.blend;

	VkBuffer		uniform_buffer;
	uint32_t		uniform_offset;
	VkDescriptorSet ubo_set;
	md5ubo_t	   *ubo = (md5ubo_t *)R_UniformAllocate (sizeof (md5ubo_t), &uniform_buffer, &uniform_offset, &ubo_set);

	memcpy (ubo->model_matrix, model_matrix, 16 * sizeof (float));
	ubo->shade_vector[0] = 0.0f;
	ubo->shade_vector[1] = 0.0f;
	ubo->shade_vector[2] = 0.0f;
	ubo->blend_factor = blend;
	ubo->light_color[0] = 1.0f;
	ubo->light_color[1] = 1.0f;
	ubo->light_color[2] = 0.0f;
	ubo->entalpha = 1.0f;
	ubo->flags = 0;
	ubo->joints_offsets[0] = lerpdata.pose1 * paliashdr->numjoints;
	ubo->joints_offsets[1] = lerpdata.pose2 * paliashdr->numjoints;
	ubo->joints_bind_offset = paliashdr->numframes * paliashdr->numjoints;

	vulkan_pipeline_t pipeline = vulkan_globals.md5_debug_pipeline[cbx->pipeline_variant];
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	VkDescriptorSet descriptor_sets[2] = {ubo_set, paliashdr->joints_set};
	vulkan_globals.vk_cmd_bind_descriptor_sets (cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout.handle, 2, 2, descriptor_sets, 1, &uniform_offset);
	vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, paliashdr->skeleton_index_buffer, 0, VK_INDEX_TYPE_UINT16);
	vulkan_globals.vk_cmd_draw_indexed (cbx->cb, paliashdr->num_skeleton_indexes, 1, 0, 0, 0);
}
