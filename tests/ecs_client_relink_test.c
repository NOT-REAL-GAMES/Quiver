/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise production relinking with synthetic network state and stubs for
 * rendering and particles. This does not parse packets or render images. */
#include "cl_main.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

server_t   sv;
viddef_t   vid;
cvar_t	   r_lerpmove, r_lerpturn, r_lerpmodels, chase_active;
float	   host_netinterval;
int		   r_trace_line_cache_counter;
static int freed, allocated, effect_order[16], effect_count;
static uint32_t visual_fixture_count;
static qmodel_t visual_fixture_model;
static qboolean visual_batch_supported;
static uint64_t visual_fixture_revision = 1;
static int visual_snapshot_copies;
static float visual_fixture_offset;
uint64_t SV_ECSStressVisualRevision (void) { return visual_fixture_revision; }
qboolean R_ECSVisualBatchSupported (qmodel_t *model)
{
	assert (model == &visual_fixture_model);
	return visual_batch_supported;
}
uint32_t SV_ECSStressVisualSnapshot (vec3_t *positions, uint32_t capacity)
{
	if (positions) ++visual_snapshot_copies;
	for (uint32_t i = 0; positions && i < capacity && i < visual_fixture_count; ++i)
	{
		positions[i][0] = (float)i + visual_fixture_offset;
		positions[i][1] = 2;
		positions[i][2] = 3;
	}
	return visual_fixture_count;
}
qmodel_t *Mod_ForName (const char *name, qboolean crash)
{
	(void)crash;
	assert (!strcmp (name, "progs/quiver_ripple.mdl"));
	return &visual_fixture_model;
}
void Con_Warning (const char *fmt, ...) { (void)fmt; }
void *Mem_Alloc (size_t n) { void *p = calloc (1, n); assert (p); return p; }
void Mem_Free (const void *p) { free ((void *)p); }
void	  *Mem_Realloc (void *p, size_t n)
{
	void *r = realloc (p, n);
	assert (r);
	return r;
}
void SCR_UpdateZoom (void) {}
void R_FreeEntityBLAS (entity_t *e)
{
	(void)e;
	freed++;
}
void R_AllocateEntityBLAS (entity_t *e)
{
	(void)e;
	allocated++;
}
void R_UpdateEntityDlights (void) {}
void R_RocketTrail (vec3_t a, vec3_t b, int t)
{
	(void)a;
	(void)b;
	(void)t;
}
void R_EntityParticles (entity_t *e)
{
	effect_order[effect_count++] = (int)(e - cl.entities);
}
int32_t COM_Rand (void)
{
	return 7;
}
int PScript_ParticleTrail (vec3_t a, vec3_t b, int t, float f, int k, vec3_t axis[3], struct trailstate_s **s)
{
	(void)a;
	(void)b;
	(void)t;
	(void)f;
	(void)k;
	(void)axis;
	(void)s;
	return 0;
}
int PScript_RunParticleEffectState (vec3_t a, vec3_t b, float f, int t, struct trailstate_s **s)
{
	(void)a;
	(void)b;
	(void)f;
	(void)t;
	(void)s;
	return 0;
}
int PScript_EntParticleTrail (vec3_t a, entity_t *e, const char *n)
{
	(void)a;
	(void)e;
	(void)n;
	return 0;
}
FUNC_NORETURN void Host_Error (const char *fmt, ...)
{
	(void)fmt;
	abort ();
}

int main (void)
{
	cl.max_edicts = 8;
	cl.num_entities = 6;
	cl.entities = calloc (cl.max_edicts, sizeof (*cl.entities));
	assert (cl.entities);
	cl_network_world = ECS_CreateWorld (cl.max_edicts);
	assert (cl_network_world);
	cl_network_binding_type = ECS_Register (cl_network_world, sizeof (cl_network_binding_t));
	assert (cl_network_binding_type != ECS_INVALID_COMPONENT);
	for (int i = 5; i >= 0; --i)
		CL_ECSBindNetworkEntity (i);
	qmodel_t model = {0};
	model.traileffect = model.emiteffect = -1;
	cl.mtime[0] = 10;
	cl.mtime[1] = 9.9;
	cl.time = 10;
	cl.oldtime = 9.95;
	cls.timedemo = true;
	for (int i = 1; i < 6; ++i)
	{
		cl.entities[i].model = &model;
		cl.entities[i].msgtime = 10;
	}
	cl.entities[1].msgtime = 9; /* dropped entity */
	cl.entities[2].effects = EF_BRIGHTFIELD;
	cl.entities[3].effects = EF_BRIGHTFIELD;
	cl.entities[3].netstate.tagentity = 2; /* valid parent */
	cl.entities[4].effects = EF_BRIGHTLIGHT;
	cl.entities[4].alpha = 128;			   /* translucent, but must stay in visibility list */
	cl.entities[5].netstate.tagentity = 7; /* unknown parent */
	CL_RelinkEntities ();
	assert (freed == 1 && cl.entities[1].model == NULL && r_trace_line_cache_counter == 1);
	assert (cl_numvisedicts == 3 && allocated == 3);
	assert (cl_visedicts[0] == &cl.entities[2] && cl_visedicts[1] == &cl.entities[3] && cl_visedicts[2] == &cl.entities[4]);
	assert (effect_count == 2 && effect_order[0] == 2 && effect_order[1] == 3);
	assert (cl.entities[4].alpha == 128);
	int light_found = 0;
	for (int i = 0; i < MAX_DLIGHTS; i++)
		if (cl_dlights[i].key == 4 && cl_dlights[i].radius > 0)
			light_found = 1;
	assert (light_found);
	/* An entity returning after an omitted update must reappear in slot order. */
	cl.entities[1].model = &model;
	cl.entities[1].msgtime = 10;
	cl.entities[1].forcelink = true;
	cl.entities[1].msg_origins[0][0] = 22;
	CL_RelinkEntities ();
	assert (cl_numvisedicts == 4 && cl_visedicts[0] == &cl.entities[1] && cl.entities[1].origin[0] == 22);
	/* A backward demo jump resets frame/origin interpolation history. */
	cl.time = 9;
	cl.oldtime = 10;
	cl.mtime[0] = 9;
	cl.mtime[1] = 8.9;
	for (int i = 1; i < 6; i++)
		cl.entities[i].msgtime = 9;
	cl.entities[2].frame = 7;
	cl.entities[2].lerp.prev_frame = 42;
	cl.entities[2].lerp.snap_frames = 2;
	cl.entities[2].msg_origins[0][0] = 5;
	uint32_t before_rewind_generation = cl.entities[2].render_generation;
	CL_RelinkEntities ();
	assert (cl.entities[2].lerp.prev_frame == 7 && cl.entities[2].lerp.snap_frames == 0 && cl.entities[2].lerp.prev_origin[0] == 5);
	assert (cl.entities[2].render_generation == before_rewind_generation + 1u);
	/* A complete local snapshot is added in one frame, including its last ID;
	 * it is cleared when the local server is not running. */
	qsocket_t local_socket = {0};
	sv.active = true; cls.demoplayback = false; cls.signon = SIGNONS; cls.netcon = &local_socket;
	visual_fixture_count = 32768;
	CL_RelinkEntities ();
	assert (cl_ecs_visual_count == 32768 && cl_maxvisedicts >= cl_numvisedicts);
	assert (cl_ecs_visual_entities[32767].origin[0] == 32767);
	assert (cl_ecs_visual_entities[32767].netstate.scale == ENTSCALE_DEFAULT);
	assert (cl_visedicts[cl_numvisedicts - 1] == &cl_ecs_visual_entities[32767]);
	entity_t *batch = NULL;
	assert (CL_ECSVisualBatch (&batch) == 0 && batch == NULL);
	visual_batch_supported = true;
	CL_RelinkEntities ();
	assert (cl_ecs_visual_count == 32768 && cl_numvisedicts < 32768);
	assert (CL_ECSVisualBatch (&batch) == 32768 && batch == cl_ecs_visual_entities);
	assert (batch[32767].origin[0] == 32767 && batch[32767].netstate.scale == ENTSCALE_DEFAULT);
	int copies = visual_snapshot_copies;
	CL_RelinkEntities ();
	assert (visual_snapshot_copies == copies); /* no server tick: reuse snapshot */
	visual_fixture_offset = 7; ++visual_fixture_revision;
	CL_RelinkEntities ();
	assert (visual_snapshot_copies == copies + 1 && batch[32767].origin[0] == 32774);
	visual_batch_supported = false;
	local_socket.driver = 1;
	CL_RelinkEntities ();
	assert (cl_ecs_visual_count == 0);
	local_socket.driver = 0;
	sv.active = false;
	CL_RelinkEntities ();
	assert (cl_ecs_visual_count == 0 && cl_numvisedicts < 32768);
	CL_FreeECSVisuals ();
	ECS_DestroyWorld (cl_network_world);
	free (cl.entities);
	free (cl_visedicts);
	free (cl_visedicts_alpha);
	puts ("PASS: client ECS relink loss, attachment, effects, alpha, rewind");
	return 0;
}
