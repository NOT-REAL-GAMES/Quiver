/* Exercise the real demo seek and temporary-entity reset paths. */
#include "quakedef.h"
#include "sys.h"
#include <assert.h>
#include <stdio.h>

client_state_t cl;
client_static_t cls;
dlight_t cl_dlights[MAX_DLIGHTS];
entity_state_t nullentitystate;
int cl_numvisedicts, cl_maxvisedicts;
entity_t **cl_visedicts;
cmd_source_t cmd_source = src_command;
float scr_clock_off;
static const char *seek_arg = "-1";
static int seek_count;
int Cmd_Argc (void) { return 2; }
const char *Cmd_Argv (int arg) { return arg == 1 ? seek_arg : "seek"; }
void Con_Printf (const char *fmt, ...) { (void)fmt; }
int Sys_fseek (FILE *file, qfileofs_t offset, int origin)
{ (void)file; assert (offset == 1 && origin == SEEK_SET); ++seek_count; return 0; }
void V_ResetBlend (void) {}
void Fog_NewMap (void) {}
void Sky_NewMap (void) {}
void R_ClearParticles (void) {}
void PScript_ClearParticles (qboolean load) { (void)load; }
void SCR_CenterPrintClear (void) {}
void BGM_Stop (void) {}
void S_StopAllSounds (qboolean clear, qboolean keep) { (void)clear; (void)keep; }
void CL_Seek_f (void);
entity_t *CL_NewTempEntity (void);

int main (void)
{
	entity_t entities[3] = {0};
	entity_t *visible[1] = {0};
	cl.entities = entities; cl.num_entities = 3; cl.time = 10;
	cl_visedicts = visible; cl_maxvisedicts = 1;
	entities[1].render_generation = UINT32_MAX;
	entities[2].render_generation = 7;
	entity_t *temp = CL_NewTempEntity ();
	assert (temp && temp->render_generation == 1);
	cls.demoplayback = true; cls.demo_prespawn_end = 1;
	CL_Seek_f ();
	assert (seek_count == 1 && cls.demoseeking && entities[1].render_generation == 0 &&
		entities[2].render_generation == 8 && temp->render_generation == 2);
	cl_numvisedicts = 0;
	assert (CL_NewTempEntity () == temp && temp->render_generation == 3);
	seek_arg = "+1"; cl.time = 5; cls.demoseeking = false;
	CL_Seek_f ();
	assert (seek_count == 1 && cl.time == 6 && entities[2].render_generation == 8 && temp->render_generation == 3);
	puts ("PASS: demo seek preserves render-entity cache identity");
	return 0;
}
