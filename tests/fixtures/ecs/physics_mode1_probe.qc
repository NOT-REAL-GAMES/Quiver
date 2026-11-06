/* DarkPlaces-compatible think-only simulation; initialized in worldspawn,
 * because physics_mode 1 deliberately does not call StartFrame. */
float physics_mode = 1;
.float ecs_mode1_token;
entity ecs_mode1_first, ecs_mode1_later;
float ecs_mode1_start;

void() ecs_mode1_later_think =
{
    if (self.ecs_mode1_token == 47 && time - ecs_mode1_start < 0.01)
        dprint("ECS_PROBE_MODE1_SAME_FRAME\n");
    else
    {
        dprint("ECS_PROBE_FAIL_MODE1_TIMING token=");
        dprint(ftos(self.ecs_mode1_token));
        dprint(" start=");
        dprint(ftos(ecs_mode1_start));
        dprint(" now=");
        dprint(ftos(time));
        dprint("\n");
    }
};

void() ecs_mode1_first_think =
{
    ecs_mode1_start = time;
    remove(self);
    ecs_mode1_later = spawn();
    if (ecs_mode1_later == self)
        dprint("ECS_PROBE_FAIL_MODE1_RETAINED\n");
    ecs_mode1_later.ecs_mode1_token = 47;
    ecs_mode1_later.think = ecs_mode1_later_think;
    ecs_mode1_later.nextthink = time + 0.001;
};

void() ecs_mode1_setup =
{
    physics_mode = 1;
    ecs_mode1_first = spawn();
    ecs_mode1_first.think = ecs_mode1_first_think;
    ecs_mode1_first.nextthink = time + 0.001;
};
