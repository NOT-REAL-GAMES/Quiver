# Entity-component system

The engine has a small native C ECS for incrementally migrating entity work.
It is not a replacement for the QuakeC virtual machine, edict ABI, wire entity
numbers, or renderer-owned entity storage. Server QuakeC, client QuakeC, and
network-render entities each have their own world. Worlds and their contents
are owned by the main thread; there is no concurrent mutation or automatic
system scheduling.

The server and client `max_edicts` setting defaults to 32,000 and can be set
as high as 1,048,576 before loading a map. The count includes the world and
reserved client slots. QuakeC represents entity addresses as signed 32-bit
byte offsets, so a mod with unusually large entity fields may have a lower
effective limit; the engine rejects configurations whose edict allocation
would exceed that address space. Memory usage grows with the configured
capacity, even when relatively few entities are active.

Older network protocols retain their entity limits. PEXT2 remote clients are
limited to 32,000 by default because their actual capacity is not negotiated;
server operators may raise `sv_pext2_maxentities` for known-capable clients
whose own `max_edicts` is configured high enough. A local PEXT2 client uses the
server's configured limit. Beam, lightning, and trail messages with entity
owners above 32,767 are omitted where their legacy wire formats cannot safely
encode them; custom mod messages must also respect their peers' protocols.

## Using the core

`Quake/ecs.h` declares the interface. Create a world with a fixed slot capacity,
register components (up to 64 per world), and spawn entities at the slots owned
by the calling subsystem. Each handle contains a world identity, slot, and
generation. `ECS_Alive`, `ECS_Get`, `ECS_Add`, `ECS_Remove`, and `ECS_Despawn`
reject handles from another world or an earlier occupant. Destroying and
recreating a world invalidates all of its handles. Spawning an already-live
slot returns its current handle. Components are zero-initialized on addition;
removing and adding one again clears it. Component pointers remain at stable
addresses until world destruction, but their contents stop belonging to an
entity when that component or entity is removed.

Use `ECS_NextUntil` with `ECS_MASK(component)` to query matching live slots in
ascending order, or `ECS_Next` to search the whole world. Re-query starting at
one past the last slot after each entity: mutations are immediate and a later
slot spawned during a callback must be visible in that same ordered traversal.
These APIs do not implicitly defer structural changes. Registered component
storage is allocated for the world's full capacity; `ECS_BytesAllocated`
reports core allocations, excluding allocator overhead and referenced external
objects.

## Compatibility bindings

Each QCVM binds its live edict slots to components pointing to stable edicts and
their engine-owned timing data. The timing component's backing storage stays
inside the edict: this preserves full-edict snapshots, fast-load behavior, and
retained pointers during QuakeC callbacks without maintaining a second copy.
QuakeC fields, including mod-defined fields, are still authoritative. The
server physics system visits live bindings in edict order and continues to run
QuakeC callbacks synchronously. A load or fast-load rebuilds the world and
invalidates its previous ECS handles; ordinary edict slot reuse advances the
slot generation. World, client slots, edict free-list and protocol numbers
retain their existing rules.

The client render world binds initialized network protocol slots to existing
`entity_t` objects. These ECS handles identify client protocol slots, not
individual server-edict incarnations. Relinking iterates bindings in protocol
order while retaining the existing interpolation, effects and render-entity
pointers. Static and temporary render entities and the viewmodel still use
their original paths. Map changes and connection resets destroy the bindings;
demo seeking keeps the existing protocol slot storage.

Run `ecs_stats` in the console to report the currently allocated server,
client-QuakeC, and client-render ECS storage and the number of initialized
client protocol slots. It does not include legacy edict and render entity
storage or allocator bookkeeping.

## Validation and benchmarks

`meson test -C build --print-errorlogs` includes core lifetime and mutation
coverage. On Linux with GCC or Clang, it also tests isolated server and client
QuakeC VM bindings and client relinking with synthetic network state and
renderer stubs; this does not simulate actual packet loss. With local Quake
PAK files and a Vulkan display, use
`tests/smoke_ecs_gameplay.py --binary build/vkquake --basedir /path/to/quake`
to exercise live save/load, map resets and demo seeking. The existing
`tests/smoke_sector_props_runtime.py` exercises player pushing and physics.

To compare a preserved pre-ECS executable with a new release build, run:

```
python3 tests/bench_ecs.py --baseline /path/to/old/vkquake \
    --candidate build/vkquake --basedir /path/to/quake
```

Each engine runs separately with disposable configuration. Alternating pairs
compare warmed, uninstrumented timedemos and independently sampled live server
physics times. Results include ECS-owned allocation and whole-process RSS;
RSS includes renderer, game assets and allocator effects, so its difference is
not a precise ECS allocation measurement. The timing samples are workload- and
machine-dependent; ECS does not by itself guarantee a speedup.

Further stages can migrate additional engine-owned components and systems after
measuring them, while retaining the QuakeC ABI and synchronously ordered
callbacks for compatibility. Parallelism requires an explicit proof that a
system cannot observe or mutate shared gameplay or renderer state concurrently.

A local release comparison on a Ryzen 9 7900X / RTX 4080 at 1280×720,
using four alternating pairs and three measured `demo1` replays per process,
measured median frame times of 0.553 ms before and 0.557 ms after (median
paired change +0.3%). In separate six-second `e1m1` samples with `sv_speeds`,
median physics cost was 0.0365 ms before and 0.0405 ms after (median paired
change +8.8%). The first ECS implementation measured +17.4% for that physics
sample; replacing three lookups per entity with one reduced the observed
increase. These microsecond-scale samples vary across runs, so the residual
increase is a limitation to investigate on other maps and machines rather than
a general performance claim. The ECS worlds reported 1,025,056 bytes each for
the server and client render worlds, with client QuakeC inactive. Whole-process
RSS varied by several MiB between runs and is not a reliable isolated measure
of that allocation.

`tests/smoke_ecs_quakec.py` additionally builds a custom-field and callback
mutation probe with an external `fteqcc`, GPL id1 QuakeC sources and local PAKs.
It checks same-frame changes and slot reuse against actual QuakeC execution;
run it against both the baseline and candidate when those dependencies are
available. The source and compiler are inputs, not installed or modified by the
test. For example:

```
python3 tests/smoke_ecs_quakec.py --binary build/vkquake \
    --basedir /path/to/quake --compiler /path/to/fteqcc \
    --quakec-source /path/to/id1-quakec
```
