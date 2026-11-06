/* High entity baselines, parent attachment, sound and subsequent deltas. */
float(entity ent) num_for_edict = #512;
.entity tag_entity;
.float tag_index;
entity ecs_net_parent, ecs_net_child;
float ecs_net_done;

void() ecs_net_setup =
{
    local float i;
    local entity filler;
    precache_model("progs/armor.mdl");
    precache_sound("misc/menu1.wav");
    i = 0;
    while (i < 65534)
    {
        filler = spawn();
        i = i + 1;
    }
    ecs_net_parent = spawn();
    ecs_net_child = spawn();
    if (num_for_edict(ecs_net_parent) != 65536 || num_for_edict(ecs_net_child) != 65537)
    {
        dprint("ECS_NET_FAIL_BASELINE_INDEX\n");
        return;
    }
    setmodel(ecs_net_parent, "progs/armor.mdl");
    setorigin(ecs_net_parent, '0 0 64');
    setmodel(ecs_net_child, "progs/armor.mdl");
    ecs_net_child.tag_entity = ecs_net_parent;
    ecs_net_child.tag_index = 0;
    setorigin(ecs_net_child, '0 0 0');
    dprint("ECS_NET_BASELINE_READY\n");
};

void() ecs_net_frame =
{
    local entity player, created;
    if (ecs_net_done)
        return;
    player = find(world, classname, "player");
    if (player == world || player.impulse != 211)
        return;
    player.impulse = 0;
    ecs_net_done = 1;
    setorigin(ecs_net_parent, '48 0 64');
    created = spawn();
    if (num_for_edict(created) <= 65537)
    {
        dprint("ECS_NET_FAIL_DYNAMIC_INDEX\n");
        return;
    }
    setmodel(created, "progs/armor.mdl");
    setorigin(created, '-48 0 64');
    sound(ecs_net_parent, CHAN_AUTO, "misc/menu1.wav", 1, ATTN_NORM);
    dprint("ECS_NET_DYNAMIC_ID "); dprint(ftos(num_for_edict(created))); dprint("\n");
    dprint("ECS_NET_DELTA_READY\n");
};
