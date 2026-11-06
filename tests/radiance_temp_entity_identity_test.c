/* Exercise the real temporary-entity allocator across consecutive frames. */
#include "quakedef.h"
#include <assert.h>
#include <stdio.h>

COMPILE_TIME_ASSERT (render_serial_is_uint32, sizeof (((entity_t *)0)->render_generation) == sizeof (uint32_t));
COMPILE_TIME_ASSERT (render_serial_is_outside_netstate,
	offsetof (entity_t, render_generation) >= offsetof (entity_t, netstate) + sizeof (entity_state_t));
#if UINTPTR_MAX == UINT64_MAX
/* On supported 64-bit ABIs, use the existing alignment gap rather than
 * increasing the allocation for up to a million client entity slots. */
COMPILE_TIME_ASSERT (render_serial_uses_padding,
	offsetof (entity_t, topnode) == offsetof (entity_t, dlightbits) + sizeof (int) + sizeof (uint32_t));
#endif

entity_state_t nullentitystate;
int cl_numvisedicts, cl_maxvisedicts;
entity_t **cl_visedicts;
extern int num_temp_entities;
entity_t *CL_NewTempEntity (void);

int main (void)
{
	entity_t *visible[2] = {0};
	cl_visedicts = visible;
	cl_maxvisedicts = 2;
	entity_t *first = CL_NewTempEntity ();
	assert (first && first == visible[0] && first->render_generation == 1);
	first->frame = 17;
	/* The same address is recycled after the per-frame list is reset. */
	cl_numvisedicts = num_temp_entities = 0;
	entity_t *second = CL_NewTempEntity ();
	assert (second == first && second->render_generation == 2 && second->frame == 0);
	CL_ClearTempEntities ();
	assert (first->render_generation == 3);
	cl_numvisedicts = 0;
	assert (CL_NewTempEntity () == first && first->render_generation == 4);
	second->render_generation = UINT32_MAX;
	cl_numvisedicts = num_temp_entities = 0;
	assert (CL_NewTempEntity ()->render_generation == 0); /* defined unsigned wrap */
	puts ("PASS: recycled temporary entity render identity");
	return 0;
}
