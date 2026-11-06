/* Fill all 1,048,576 edict slots, including world and the reserved client.
 * No filler entity runs a think or has a model, avoiding network output. */
float(entity ent) num_for_edict = #512;
float ecs_capacity_count;
entity ecs_capacity_last, ecs_capacity_reused;

void() ecs_capacity_frame =
{
    local float i;
    /* Bound each QuakeC invocation well below the runaway instruction limit. */
    i = 0;
    while (i < 65536 && ecs_capacity_count < 1048574)
    {
        ecs_capacity_last = spawn();
        ecs_capacity_count = ecs_capacity_count + 1;
        i = i + 1;
    }
    if (ecs_capacity_count != 1048574)
        return;
    if (num_for_edict(ecs_capacity_last) != 1048575)
    {
        dprint("ECS_CAPACITY_FAIL_LAST_SLOT\n"); localcmd("quit\n"); return;
    }
    remove(ecs_capacity_last);
    ecs_capacity_reused = spawn();
    if (ecs_capacity_reused != ecs_capacity_last || num_for_edict(ecs_capacity_reused) != 1048575)
    {
        dprint("ECS_CAPACITY_FAIL_REUSE\n"); localcmd("quit\n"); return;
    }
    dprint("ECS_CAPACITY_PASS\n");
    localcmd("quit\n");
};
