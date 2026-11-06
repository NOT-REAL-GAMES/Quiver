/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real edict/ECS adapter with two independent VM instances.
 * Linking discards unrelated engine functions, so no game assets or renderer
 * are needed. This tests the adapter, not execution of QuakeC bytecode. */
#include "quakedef.h"

#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

entity_state_t nullentitystate;
vec3_t vec3_origin;
static jmp_buf expected_error;
static bool expecting_error;

void SV_UnlinkEdict (edict_t *ent) { (void)ent; }
void *Mem_Alloc (const size_t size) { void *p = calloc (1, size); assert (p); return p; }
void Mem_Free (const void *p) { free ((void *)p); }
FUNC_NORETURN void Host_Error (const char *format, ...) { (void)format; if (expecting_error) longjmp (expected_error, 1); abort (); }
FUNC_NORETURN void Sys_Error (const char *format, ...) { (void)format; abort (); }

static void setup (qcvm_t *vm, dprograms_t *progs)
{
	qcvm = vm;
	vm->progs = progs;
	vm->edict_size = sizeof (edict_t);
	vm->max_edicts = 16;
	vm->num_edicts = 1;
	vm->edicts = calloc (vm->max_edicts, vm->edict_size);
	assert (vm->edicts);
#if defined(DEBUG) || defined(_DEBUG)
	for (int i = 0; i < vm->max_edicts; ++i)
	{
		edict_t *ed = EDICT_NUM_NO_CHECK (i);
		ed->qcvm_owner = vm;
		ed->edict_ptr = ed;
		ed->edict_num = i;
	}
#endif
	ED_ECSReconcile ();
	assert (ECS_Alive (vm->ecs_world, ECS_At (vm->ecs_world, 0)));
	edict_t *ed = ED_Alloc ();
	assert (ed == EDICT_NUM (1));
	ecs_entity_t handle = ECS_At (vm->ecs_world, 1);
	edict_ecs_binding_t *binding = ECS_Get (vm->ecs_world, handle, vm->ecs_edict_component);
	assert (binding && binding->edict == ed && binding->timing == &ed->timing);
}

int main (void)
{
	qcvm_t *server = calloc (1, sizeof (*server));
	qcvm_t *client = calloc (1, sizeof (*client));
	dprograms_t *progs = calloc (1, sizeof (*progs));
	assert (server && client && progs);
	progs->entityfields = sizeof (entvars_t) / 4;
	setup (server, progs);
	setup (client, progs);

	ecs_entity_t server_handle = ECS_At (server->ecs_world, 1);
	ecs_entity_t client_handle = ECS_At (client->ecs_world, 1);
	assert (!ECS_Alive (server->ecs_world, client_handle));
	assert (!ECS_Alive (client->ecs_world, server_handle));

	qcvm = client;
	edict_t *ed = EDICT_NUM (1);
	ED_Retain (ed);
	ED_Free (ed);
	assert (!ECS_Alive (client->ecs_world, client_handle));
	assert (ECS_Alive (server->ecs_world, server_handle));

	assert (ED_Alloc () != ed); /* retained, freed slot is not yet reusable */
	ED_Release (ed);
	assert (ED_Alloc () == ed);
	ecs_entity_t replacement = ECS_At (client->ecs_world, 1);
	assert (ECS_Alive (client->ecs_world, replacement));
	assert (replacement.generation != client_handle.generation);

	ED_ECSReset ();
	assert (!ECS_Alive (client->ecs_world, replacement));
	assert (ECS_Alive (client->ecs_world, ECS_At (client->ecs_world, 1)));
	assert (ECS_Alive (server->ecs_world, server_handle));

	/* Wrap the configured-capacity FIFO and verify that large indices do not
	 * truncate to the 16-bit entity numbers used by older protocols. */
	while (client->num_edicts < client->max_edicts)
		ED_Alloc ();
	for (int i = 1; i < client->num_edicts; ++i)
		ED_Free (EDICT_NUM (i));
	for (int i = 1; i < client->num_edicts; ++i)
		assert (ED_Alloc () == EDICT_NUM (i));
	ED_Free (EDICT_NUM (4));
	ED_Free (EDICT_NUM (7));
	uint32_t *const ring = client->free_list.circular_buffer;
	ED_RebuildFreeList (true);
	assert (client->free_list.circular_buffer == ring && client->free_list.size == 2);
	assert (ED_Alloc () == EDICT_NUM (4));
	assert (ED_Alloc () == EDICT_NUM (7));

	qcvm_t *large = calloc (1, sizeof (*large));
	assert (large);
	qcvm = large;
	large->progs = progs;
	large->edict_size = sizeof (edict_t);
	large->max_edicts = MAX_EDICTS;
	/* Reserve the address range without touching every intermediate edict. */
	large->num_edicts = large->max_edicts - 1;
	assert (ED_EdictAllocSize (large) == (size_t)large->max_edicts * sizeof (edict_t));
	large->edicts = calloc (1, ED_EdictAllocSize (large));
	assert (large->edicts);
#if defined(DEBUG) || defined(_DEBUG)
	edict_t *last_slot = EDICT_NUM_NO_CHECK (MAX_EDICTS - 1);
	last_slot->qcvm_owner = large;
	last_slot->edict_ptr = last_slot;
	last_slot->edict_num = MAX_EDICTS - 1;
#endif
	edict_t *high = ED_Alloc ();
	assert (NUM_FOR_EDICT (high) == MAX_EDICTS - 1);
	ED_Free (high);
	assert (ED_Alloc () == high && NUM_FOR_EDICT (high) == MAX_EDICTS - 1);
	expecting_error = true;
	if (setjmp (expected_error) == 0)
	{
		qcvm_t oversized = {0};
		oversized.max_edicts = MAX_EDICTS;
		oversized.edict_size = INT_MAX;
		(void) ED_EdictAllocSize (&oversized);
		abort ();
	}
	expecting_error = false;
	free (large->free_list.circular_buffer);
	free (large->edicts);
	free (large);
	qcvm_t *worlds[] = {server, client};
	for (int i = 0; i < 2; ++i)
	{
		ECS_DestroyWorld (worlds[i]->ecs_world);
		free (worlds[i]->free_list.circular_buffer);
		free (worlds[i]->edicts);
		free (worlds[i]);
	}
	free (progs);
	puts ("SSQC and CSQC edict ECS lifecycle passed");
	return 0;
}
