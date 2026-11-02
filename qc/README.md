# World-position QuakeC SDK

`worldpos.qh` declares classic-VM builtins under `QUIVER_WORLDPOS_V1`. Handles
contain a slot and generation, not coordinates. Ordinary assignment aliases a
mutable position; allocate another handle and use `wp_copy` for an independent
value. Use `wp_getentity`, `wp_add`, `wp_distance`, and `wp_direction` for gameplay
positions. `wp_delta` returns a precise local vector within its documented range.

Register custom vector fields and globals that hold persistent handles during
`worldspawn`:

```c
.worldpos home;
worldpos quest_destination;

void() RegisterWorldPositions =
{
    wp_register_field("home");
    wp_register_global("quest_destination");
};
```

`world_save` stores reachable canonical positions, owners, and named references.
`world_load` validates the current program schema, restores those positions, and
remaps every registered reference to a fresh handle. Aliases stay aliases. Zero
vectors represent empty references. Unregistered temporary handles expire on
load. A registered nonzero stale handle makes saving fail: clear aliases after
freeing a handle or removing its owner. Engine-owned vectors cannot be registered.
Names and offsets must resolve to nonoverlapping custom vector declarations.

Owners and entity references keep their edict indices within a checkpoint.
Multiplayer checkpoints omit player fields and all references to player-owned
handles; those registered locations restore as zero and reconnecting players
start fresh. Handles owned by world or surviving nonplayer entities can persist.
The checkpoint must use the same `progs.dat` and client capacity as the saved game.

The binary handle payload has versioned little-endian fields, canonical signed
64-bit chunk coordinates, a generation watermark, and bounded records: at most
256 registrations, 65,536 positions, 1,048,576 references, and 16 MiB. Load checks
schema names, types, aliases, bounds, generations, owners, and live entity masks
before replacing the handle arena. It never interprets a serialized raw vector
token as a persistent handle.

## Opt-in entity paging

`wp_streamable(subject, 1)` asks the engine to page a nonplayer entity when it is
outside active regions. It returns whether opt-in was accepted; it does not force
an immediate page-out. `wp_streamable(subject, 0)` removes opt-in.

An entity's position handles are eligible only when they are owned by that entity
and every live owned handle is reachable solely through its own registered vector
fields. Shared aliases in globals or other entities pin it. World-owned handles,
handles owned by another entity, unregistered vector aliases, and unreferenced
temporary owned handles also pin it. The engine checks every named vector global
and field for hidden aliases, so clear complete temporary vectors before leaving
QC. Ordinary entity references have additional engine paging restrictions.

Handle export is read-only. The engine must durably store the page, recheck the
snapshot and references, and only then free the entity and its handles. Restoration
allocates fresh handles while preserving unrelated entities' handles and aliases
within the page. Self-owner references remap to the newly allocated edict slot;
no old runtime slot number is treated as a durable identity. Paging runs only while
the QuakeC VM is idle. A stored handle page uses only its own handles' generation
watermark, so unrelated allocations do not continually invalidate a pending save.

Restored QC strings are interned by content for the VM lifetime. Repeated page-ins
reuse IDs, and string aliases remain valid after an entity pages out. The pool
accepts at most 16,384 distinct texts and 4 MiB of text per loaded VM; a load that
would exceed either limit fails before allocating the entity. Compiled literals
and immutable VM-owned precache strings are reused. Mutable or zoned strings are
copied into VM-owned storage. Pool metadata resets with the VM's string storage.

## Runnable checkpoint fixture

The fixture is a small original classic-v6 program. It creates a nonplayer entity
roughly a million units from the origin, checks its canonical position and shared
handle aliases every frame, and verifies that old raw tokens become invalid when
registered handles are remapped. It is a persistence fixture, not a base-game port.
`checkpoint_fixture.qc` is the readable equivalent of the emitted program.

Generate it without installing a QuakeC compiler:

```sh
python3 qc/generate_checkpoint_fixture.py /tmp/worldpos_fixture/progs.dat
```

Run the complete isolated smoke test using your own Quake game assets:

```sh
python3 qc/run_checkpoint_fixture.py --binary build/vkquake --basedir /path/to/quake
```

The runner creates a private game directory, links only `pak*.pak`, generates a
separate fixture mod, saves and reloads twice, then restarts the process and loads
again. It retains `checkpoint.log`, `restart.log`, and `summary.json` under a
printed temporary directory. Success requires the QC assertions and clean server
exits in both processes. User configs and saves are not loaded or changed.

The broader base-game QuakeC movement, AI, targeting, projectile, and trigger code
still needs migration to canonical positions. Nearby native vectors remain a
compatibility path; reading native position fields for an entity outside the
current local frame fails with guidance to use this API. `wp_tracebox_terrain`
tests terrain only and does not include entity collision.
