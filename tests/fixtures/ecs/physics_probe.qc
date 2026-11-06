/* Regression probe appended after the stock QuakeC source by the test runner. */
.float ecs_probe_token;
entity ecs_probe_a, ecs_probe_b, ecs_probe_slot, ecs_probe_new;
float ecs_probe_started, ecs_probe_frame_id;

void() ecs_probe_reused_think =
{
    if (self.ecs_probe_token == 91 && ecs_probe_frame_id == framecount)
        dprint("ECS_PROBE_REUSED_SAME_FRAME\n");
    else
        dprint("ECS_PROBE_FAIL_REUSED\n");
};

void() ecs_probe_deferred_think =
{
    if (self.ecs_probe_token == 105 && framecount == ecs_probe_frame_id + 1)
        dprint("ECS_PROBE_NEW_NEXT_FRAME\n");
    else
        dprint("ECS_PROBE_FAIL_NEW_FRAME\n");
};

void() ecs_probe_later_think =
{
    if (self.ecs_probe_token == 73 && self.movetype == MOVETYPE_NOCLIP && ecs_probe_frame_id == framecount)
        dprint("ECS_PROBE_MUTATION_SAME_FRAME\n");
    else
        dprint("ECS_PROBE_FAIL_MUTATION\n");
};

void() ecs_probe_first_think =
{
    local entity oldslot;
    ecs_probe_frame_id = framecount;
    ecs_probe_b.ecs_probe_token = 73;
    ecs_probe_b.movetype = MOVETYPE_NOCLIP;
    oldslot = ecs_probe_slot;
    remove(oldslot);
    remove(self);
    ecs_probe_slot = spawn();
    if (ecs_probe_slot != oldslot || ecs_probe_slot == self)
        dprint("ECS_PROBE_FAIL_SLOT_REUSE\n");
    ecs_probe_slot.ecs_probe_token = 91;
    ecs_probe_slot.movetype = MOVETYPE_NONE;
    ecs_probe_slot.think = ecs_probe_reused_think;
    ecs_probe_slot.nextthink = time + 0.001;
    ecs_probe_new = spawn();
    if (ecs_probe_new == self || ecs_probe_new == oldslot)
        dprint("ECS_PROBE_FAIL_RETAINED_SLOT\n");
    ecs_probe_new.ecs_probe_token = 105;
    ecs_probe_new.movetype = MOVETYPE_NONE;
    ecs_probe_new.think = ecs_probe_deferred_think;
    ecs_probe_new.nextthink = time + 0.001;
};

void() ecs_probe_frame =
{
    if (ecs_probe_started)
        return;
    ecs_probe_started = 1;
    ecs_probe_a = spawn();
    ecs_probe_b = spawn();
    ecs_probe_slot = spawn();
    ecs_probe_a.movetype = MOVETYPE_NONE;
    ecs_probe_a.think = ecs_probe_first_think;
    ecs_probe_a.nextthink = time + 0.001;
    ecs_probe_b.movetype = MOVETYPE_NONE;
    ecs_probe_b.think = ecs_probe_later_think;
    ecs_probe_b.nextthink = time + 0.001;
};
