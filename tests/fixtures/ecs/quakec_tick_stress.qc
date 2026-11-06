/* Dedicated-server stress fixture. The flag is a QuakeC edict field, not a
 * native ECS component; the server nevertheless dispatches its think via ECS. */
.float ecs_stress_flag;
float ecs_stress_target, ecs_stress_created, ecs_stress_visits;
float ecs_stress_errors, ecs_stress_active, ecs_stress_phase, ecs_stress_frames;
entity ecs_stress_new;

void() ecs_stress_think =
{
    if (ecs_stress_active)
    {
        self.ecs_stress_flag = 1 - self.ecs_stress_flag;
        if (self.ecs_stress_flag != ecs_stress_phase)
            ecs_stress_errors = ecs_stress_errors + 1;
        ecs_stress_visits = ecs_stress_visits + 1;
    }
    self.nextthink = time + 0.0001;
};

void() ecs_stress_frame =
{
    local float i;
    if (!ecs_stress_target)
    {
        if (mapname == "ecs1024") ecs_stress_target = 1024;
        else if (mapname == "ecs8192") ecs_stress_target = 8192;
        else if (mapname == "ecs31998") ecs_stress_target = 31998;
        else if (mapname == "ecs32768") ecs_stress_target = 32768;
        else { dprint("ECS_STRESS_FAIL_BAD_MAP\n"); localcmd("quit\n"); return; }
    }
    if (!ecs_stress_active)
    {
        /* Limit work per VM invocation and allow startup to settle. */
        i = 0;
        while (i < 512 && ecs_stress_created < ecs_stress_target)
        {
            ecs_stress_new = spawn();
            ecs_stress_new.movetype = MOVETYPE_NONE;
            ecs_stress_new.think = ecs_stress_think;
            ecs_stress_new.nextthink = time + 0.0001;
            ecs_stress_created = ecs_stress_created + 1;
            i = i + 1;
        }
        if (ecs_stress_created == ecs_stress_target)
        {
            ecs_stress_active = 1;
            ecs_stress_phase = 1;
            dprint("ECS_STRESS_READY\n");
        }
        return;
    }
    if (ecs_stress_visits != ecs_stress_target || ecs_stress_errors != 0)
    {
        dprint("ECS_STRESS_FAIL_VISITS_OR_VALUE\n");
        localcmd("quit\n");
        return;
    }
    ecs_stress_frames = ecs_stress_frames + 1;
    if (ecs_stress_frames == 8)
        dprint("ECS_STRESS_MEASURE\n");
    if (ecs_stress_frames == 136)
    {
        dprint("ECS_STRESS_PASS\n");
        localcmd("quit\n");
        return;
    }
    ecs_stress_visits = 0;
    ecs_stress_phase = 1 - ecs_stress_phase;
};
