/* Verify free-list and trigger paths with an edict number above UINT16_MAX.
 * The test server has one reserved client slot and no connected clients. */
float(entity ent) num_for_edict = #512;
.float ecs_highid_tag;
float ecs_highid_started, ecs_highid_touched, ecs_highid_wait;
entity ecs_highid_trigger, ecs_highid_old, ecs_highid_mover;

void() ecs_highid_touch =
{
    if (num_for_edict(self) == 65536 && self.ecs_highid_tag == 17 && other == ecs_highid_mover)
        ecs_highid_touched = 1;
    else
        ecs_highid_touched = -1;
};

void() ecs_highid_frame =
{
    local float i;
    local entity filler;
    if (!ecs_highid_started)
    {
        ecs_highid_started = 1;
        /* World is 0 and client is 1, making the next index exactly 65536. */
        i = 0;
        while (i < 65534)
        {
            filler = spawn();
            i = i + 1;
        }
        ecs_highid_old = spawn();
        if (num_for_edict(ecs_highid_old) != 65536)
        {
            dprint("ECS_HIGHID_FAIL_INDEX\n"); localcmd("quit\n"); return;
        }
        remove(ecs_highid_old);
        ecs_highid_trigger = spawn();
        if (ecs_highid_trigger != ecs_highid_old || num_for_edict(ecs_highid_trigger) != 65536)
        {
            dprint("ECS_HIGHID_FAIL_REUSE\n"); localcmd("quit\n"); return;
        }
        dprint("ECS_HIGHID_REUSE_PASS\n");
        ecs_highid_trigger.ecs_highid_tag = 17;
        ecs_highid_trigger.solid = SOLID_TRIGGER;
        ecs_highid_trigger.touch = ecs_highid_touch;
        setsize(ecs_highid_trigger, '-32 -32 -32', '32 32 32');
        setorigin(ecs_highid_trigger, '0 0 64');
        ecs_highid_mover = spawn();
        ecs_highid_mover.solid = SOLID_BBOX;
        ecs_highid_mover.movetype = MOVETYPE_TOSS;
        setsize(ecs_highid_mover, '-4 -4 -4', '4 4 4');
        setorigin(ecs_highid_mover, '0 0 64');
        return;
    }
    if (ecs_highid_touched == 1)
    {
        dprint("ECS_HIGHID_TOUCH_PASS\n"); localcmd("quit\n"); return;
    }
    if (ecs_highid_touched == -1 || ecs_highid_wait > 5)
    {
        dprint("ECS_HIGHID_FAIL_TOUCH\n"); localcmd("quit\n"); return;
    }
    ecs_highid_wait = ecs_highid_wait + 1;
};
