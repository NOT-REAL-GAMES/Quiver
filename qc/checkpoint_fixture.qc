// SPDX-License-Identifier: GPL-2.0-or-later
// Readable equivalent of generate_checkpoint_fixture.py's classic-v6 fixture.
// Declarations from the normal Quake ABI precede this file. The Python emitter
// supplies those declarations and no-op engine entry points without requiring qcc.
#include "worldpos.qh"

.worldpos checkpoint_home;
worldpos checkpoint_anchor;
worldpos checkpoint_alias;
worldpos checkpoint_raw;     // Deliberately saved but NOT registered.
worldpos checkpoint_probe;   // Temporary; recreated when invalid after load.
entity checkpoint_actor;
float checkpoint_checked;

void() worldspawn =
{
    wp_register_field("checkpoint_home");
    wp_register_global("checkpoint_anchor");
    wp_register_global("checkpoint_alias");
    checkpoint_anchor = wp_alloc(world);
    wp_add(checkpoint_anchor, checkpoint_anchor, '1048576.125 -1048575.75 512.5');
    checkpoint_alias = checkpoint_anchor;
    checkpoint_raw = checkpoint_anchor;
    checkpoint_actor = spawn();
    checkpoint_actor.checkpoint_home = checkpoint_anchor;
    wp_setentity(checkpoint_actor, checkpoint_anchor);
};

void() StartFrame =
{
    if (!wp_valid(checkpoint_anchor))
        error("world position checkpoint fixture assertion failed");
    // These comparisons test opaque handle identity, not position equality.
    if (checkpoint_anchor != checkpoint_alias)
        error("world position checkpoint fixture assertion failed");
    if (checkpoint_actor.checkpoint_home != checkpoint_anchor)
        error("world position checkpoint fixture assertion failed");
    if (!wp_valid(checkpoint_probe))
        checkpoint_probe = wp_alloc(world);
    wp_getentity(checkpoint_probe, checkpoint_actor);
    if (wp_distance(checkpoint_anchor, checkpoint_probe) != 0)
        error("world position checkpoint fixture assertion failed");
    if (checkpoint_raw != checkpoint_anchor)
    {
        if (wp_valid(checkpoint_raw))
            error("world position checkpoint fixture assertion failed");
        dprint("World position fixture remap PASS\n");
        checkpoint_raw = checkpoint_anchor;
    }
    if (!checkpoint_checked)
    {
        dprint("World position fixture PASS\n");
        checkpoint_checked = 1;
    }
};
