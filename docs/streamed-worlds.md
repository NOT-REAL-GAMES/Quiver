# Streamed octree worlds

Quiver has an experimental voxel-world path alongside ordinary BSP maps. The
current implementation provides an empty map, deterministic streamed terrain,
smooth terrain collision, cached Vulkan meshes, persistent voxel edits, and a
server-authoritative network transport. The complete large-world QuakeC/gameplay migration and a
full 64-client gameplay qualification are still outstanding. A 64-client UDP
connection and world-stream smoke test is included and has passed.

## Try it

Build the engine with the usual Meson/Ninja configuration. SQLite3 development
headers and the SQLite3 library are required in addition to the normal engine
dependencies. Run the game with your existing Quake assets; these modes still use
`progs.dat`, models, sounds, and the normal UI.

In the game console:

```text
map __empty
```

`__empty` is an engine-generated scene with a native free-fly camera and no BSP
file or solid geometry. It skips the mod's worldspawn, player spawning, and
gameplay callbacks, so missing spawn points or spawn functions cannot block entry.
Use the normal movement/look controls and move up/down (E/C in the default binds)
to fly; forward movement follows your view pitch. There is no gravity, player
body, weapon, or camera bob. You can run `world_create` directly from its console.
The engine still loads a valid `progs.dat` for the entity ABI and the usual UI
assets. The existing `empty` BSP map remains a separate gameplay map.

The isolated camera regression uses both a mod with callbacks that deliberately
fail and a mod with no gameplay/spawn functions. It checks flight, idle stability,
reconnection, gameplay transitions, and world creation:

```sh
python3 tests/run_empty_camera_smoke.py --binary build/vkquake --basedir /path/to/quake
```

To start transient generated terrain:

```text
map __octree
```

To create a persistent named world:

```text
world_create firstworld terrain 12345
```

The generator includes smooth stepped terrain and a flat starting plateau.
Adjacent terrain columns differ by at most one 8-unit voxel, within the player’s
normal step height. A named empty voxel world can be created with
`world_create firstworld empty`; unlike `__empty`, it uses voxel-world collision
and streaming. Use `noclip` while constructing its first floor.

World commands:

| Command | Behavior |
| --- | --- |
| `world_create <name> <empty\|terrain\|planet> [seed]` | Create a named world and enter it. Existing names are rejected. See [planet worlds and construction](planet-worlds.md) for the wrapping continent generator. |
| `world_load <name>` | Open an existing named world and restore its entity checkpoint, when present. |
| `world_save [name]` | Replace the `latest` whole-world snapshot, optionally persisting a transient world under a new name. |
| `world_snapshot list` | List pinned snapshots, including `latest` when present. |
| `world_snapshot create <name>` | Pin terrain, live entities, dormant pages, globals, and saved player state together. Existing names are rejected. |
| `world_snapshot restore <name>` | Restore a pinned whole-world snapshot and reload the session; remote clients must reconnect. |
| `world_snapshot delete <name>` | Delete a named pin and collect unreferenced objects. The reserved `latest` pin cannot be deleted. |
| `world_snapshot gc` | Collect objects unreachable from current terrain and pinned snapshots. |
| `world_stats` | Print cache, mesh, canonical position, and peer transfer diagnostics. |
| `world_teleport <chunk-x> <chunk-y> <chunk-z> [local-x local-y local-z]` | Teleport using exact signed 64-bit chunk coordinates. Default local offset is `128 128 256`. |
| `world_remove` | Remove the voxel under the crosshair within 256 units. |
| `world_place [material]` | Place an adjacent voxel; material is `1` through `4`. Placement overlapping a solid entity is rejected. |

Editing commands work in hosting and remote-client consoles. The server checks
that editing is enabled, the target is reachable and visible, its revision is
current, and a placement does not overlap a solid entity. Remote requests are
limited to ten per second, with one edit in flight per client and per chunk.
The server queues remote storage writes on its worker and sends the success
acknowledgment only after the durable revision is published. Accepted placements
reserve their cell against movement while storage is pending; failed writes
release the reservation without changing the terrain. A new request is rejected
while that client's edit or acknowledgment is pending. Repeating its most recent
accepted request before sending another request returns the saved acknowledgment
without applying the edit again. Disconnecting does not cancel a storage
transaction already accepted by the server.
For convenient editing, assign unused keys, for example:

```text
bind mouse4 world_remove
bind mouse5 "world_place 1"
```

These bindings replace any existing actions on those buttons.

For a distant-coordinate check:

```text
world_teleport 1000000000 -1000000000 0
world_stats
```

Each chunk is 1024 world units wide. This command travels roughly one trillion
units along each horizontal axis without converting the global position to a
float. `world_stats` reports chunk coordinates separately from local offsets.

## Persistence and resource controls

Named worlds live in `<active game directory>/worlds/<name>.sqlite3`. Their
procedural base is identified by a canonical descriptor containing the seed,
generator implementation version, generator type, voxel/chunk dimensions, and
material definitions version. An untouched world stores this metadata without
storing generated terrain chunks. Generator compatibility is required: a seed
alone cannot reproduce a world after its generation rules change.

Edits are sparse override octrees over that base. Inherited cells and explicitly
empty cells are distinct, so excavations survive regeneration. Reverting a chunk
to its generated contents removes its override. SHA-256 identifies immutable
objects; a path-compressed spatial Merkle octree indexes modified chunks across
the signed 64-bit coordinate space. Content hashes do not encode revision
numbers, so identical contents deduplicate. Separate monotonic revisions and
authority epochs reject stale network work, including after a snapshot restore.

SQLite atomically commits objects and the current terrain root before reporting
an edit's success. Remote edits complete asynchronously; local host-console edits
and explicit save operations may wait for storage. `world_save` replaces the
reserved `latest` pin. Named snapshots retain their own terrain root, typed entity
fields, persistent QuakeC globals, exact entity positions, registered position
handles, and dormant NPC page manifest. Private checkpoint/page payloads use
hashed 64 KiB pieces with manifests and deduplicate across snapshots; they are
never exposed through the peer terrain service. A whole-snapshot hash binds the
generator descriptor, terrain root, checkpoint, and ordered page metadata.
Ordinary Quake `.sav` files are not the world database format.

Older chunk-table databases are migrated into a separate verified
`<name>.sqlite3.merkle` database. The original database is preserved; opening the
original name subsequently selects the migrated copy. Saves and snapshot
creation/deletion collect unreachable immutable objects. `world_snapshot gc`
also runs collection explicitly. Current terrain and pinned snapshots survive
collection; active network transfers retain their own object bytes. SQLite can
reuse the freed pages without shrinking the database file immediately.

Mods can opt self-contained NPCs into automatic paging with
`wp_streamable(entity, 1)`. Eligible distant NPCs are written asynchronously to
SQLite before their live entity slot is released. Returning players cause nearby
pages to load again. A stable 64-bit identity survives slot reuse; self references
and owned registered position handles are remapped on load. Simulation is paused
while an NPC is paged out. Ordinary NPCs remain resident unless their mod opts in.

Paging is deliberately conservative. Incoming entity/global references, outgoing
references to another entity, and position handles shared outside the NPC keep it
resident. A self reference and aliases among its own registered position fields
are supported. Mods must release temporary references when they want an NPC to
be eligible. VM pointers into the NPC also pin it, and moving pushers remain
resident. Its models must be precached from `worldspawn` so a page can also load
after a process restart. Arbitrary custom absolute-time fields require mod support;
the engine adjusts ordinary `nextthink` on restoration. References hidden through
integer/float casts cannot be tracked automatically and violate this opt-in
contract.

Restored QuakeC strings are reused across page cycles. The per-VM pool is capped
at 16,384 unique strings and 4 MiB of text, with metadata and owned-storage
overhead additional. Exceeding that budget leaves a page dormant or refuses
checkpoint restoration. Reuse prevents repeated page cycles from continually
allocating identical strings; mods must still control uniquely generated text.

A checkpoint requires the same `progs.dat` hash and size and the same reserved
client count. In single player, an active saved player resumes its state and
position. In multiplayer, world entities persist, while client progression and
player-owned handles reset when players reconnect. Static entities and ambient
sounds are not checkpointed yet; saving refuses a world containing them.

Terrain commits continuously, while entity checkpoints require an explicit save.
Ordinary `world_load` and `restart` retain the latest durable terrain and restore
the `latest` saved entity state after initial world spawn. Acknowledged edits
therefore survive a restart even if no further `world_save` was issued. Later
terrain edits may change the surroundings of a saved entity or player.
Use `world_snapshot restore latest` (or a named pin) to roll terrain and entity
state back together. Restoration validates checkpoint compatibility before
publishing the root and reloads the session with a new authority epoch. Remote
clients must reconnect. Snapshots retain named states, not an unlimited edit log.
The checkpoint includes the current NPC page manifest and live stable identities.
Loading restores that manifest together with live entity state, avoiding duplicate
NPCs and discarding page changes made after the explicit save. Entity identity and
revision counters remain monotonic across this rollback.
NPC pages store the runtime working set; they are not independent savepoints.
If no explicit checkpoint exists, loading starts fresh entity state from
`worldspawn` and discards old page heads. Use `world_save` to preserve NPC progress.

| Cvar | Default | Purpose |
| --- | --- | --- |
| `world_cache_chunks` | `2048` | Resident CPU chunk cap, read when a world opens; maximum 8192, with a server minimum of 128 or 32 per reserved client slot. |
| `world_cache_mb` | `1024` | Server resident CPU terrain-tree budget in MiB; clamped to 64–4096. |
| `world_view_chunks` | `2` | Requested render neighborhood radius in chunks; clamped to 1–3. |
| `world_editing` | `1` | Allow voxel edits on the server, including remote-client requests. |
| `world_entity_paging` | `1` | Enable automatic paging of NPCs explicitly registered by their mod. |
| `world_entity_radius` | `2048` | Keep registered NPCs near a spawned player resident; clamped to 1024–4096 units per axis. |
| `world_entity_idle` | `2` | Seconds an eligible NPC must remain distant before paging; clamped to 0.1–60. |
| `world_entity_budget` | `4` | Maximum eviction attempts per frame, shared between validating committed snapshots and submitting new writes; clamped to 1–16. Pinned or failed candidates also consume attempts. |
| `world_entity_limit` | `1024` | Limit resident registered NPCs restored from pages; clamped to 16–8192. Registration of existing live NPCs is not capped by this setting. |
| `r_octree_distance` | `4096` | Maximum detailed chunk-center rendering distance, plus one chunk for edge coverage; clamped to 1024–65536. With LOD enabled, detail is also restricted to level-zero tile columns within at most eight chunks of the camera. |
| `r_octree_smooth` | `1` | Use marching tetrahedra for terrain-world surfaces. `0` rebuilds the original block meshes. Empty-generator worlds retain block meshing. |
| `r_octree_lod` | `1` | Draw coarse procedural terrain beyond the nearby editable voxel meshes. |
| `r_octree_lod_distance` | `32768` | Horizontal terrain tile coverage in units, clamped to 4096–16777216; independent of the detailed chunk streaming neighborhood and the camera's far clip plane. |
| `r_octree_mesh_mb` | `128` | GPU chunk mesh budget in MiB; clamped to 16–2048. |
| `r_octree_upload_mb` | `8` | Mesh upload target per frame in MiB; clamped to 1–128. A single eligible chunk may exceed this target. |
| `sb_scorepage` | `-1` | Follow the local player's scoreboard page; use `0`, `1`, etc. to inspect other pages in larger sessions. |

The core also limits cached CPU meshes to 128 MiB. Immutable snapshots retained
by rendering or chunk transfer can temporarily remain alive after cache eviction.
Mesh requests reserve that CPU capacity for the nearest chunks, using measured
mesh sizes once available. This prevents repeated eviction and rebuilding after
travel; distant LOD covers chunks outside the admitted working set.
The GPU budget includes buffers awaiting completion of earlier frames. When the
budget is full, nearby chunks take priority; a replacement may wait several
frames for retired buffers. Increasing `r_octree_distance` alone does not increase
the requested detailed streaming neighborhood. The separate terrain LOD horizon
extends beyond that neighborhood without loading collision octrees. Unloaded
collision terrain blocks travel until it is available, rather than letting the
player fall through absent data.

### Terrain horizon LOD

Terrain worlds enable the coarse horizon automatically. For a longer view, use:

```text
r_octree_lod 1
r_octree_lod_distance 65536
gl_farclip 65536
octree_render_stats
```

The default tile coverage is 32768 units. Quake's `gl_farclip` (default 16384)
sets the far plane for BSP, detailed chunks, distant terrain and volumetric
clouds. Its distance is measured along the camera's forward axis; a ray near
the edge of a wide view can travel farther before reaching the same plane.
Increase both settings for a longer terrain view. Selecting more tiles alone
does not change the projection or the depth of other objects.
`r_octree_lod 0` returns to detailed chunks alone.

The terrain participates in Quake's normal world rendering controls:

| Console setting | Result on a streamed world |
|---|---|
| `r_showtris 1` / `2` | Overlay all submitted triangle edges / only depth-tested edges. This includes detailed chunks, selected LOD tiles, boundary strips, ocean surfaces and the generated sky dome. Cloud shading remains visible underneath. `0` turns the overlay off. Requires non-solid fill support. |
| `r_drawworld 0` | Hide terrain, ocean, generated sky and clouds. `r_drawentities` controls entities independently. |
| `r_fullbright 1` | Show terrain material colors without directional shading. |
| `r_lightmap 1` | Show terrain lighting without material textures, suppress the sky, and take precedence over fullbright. Procedural terrain uses its own lighting despite the virtual BSP having no baked lightmaps. |
| `vid_filter 1` / `0` | Use point / linear sampling for terrain materials. Clouds retain the explicit `r_clouds_filter` control. |

As on BSP maps, `r_showtris`, `r_drawworld`, `r_fullbright` and `r_lightmap` debug overrides are restricted to a single-client server. Normal material and lighting modes retain scene fog. To check these controls, run `python3 tests/run_world_render_rules.py --binary build/vkquake --basedir .`; add `--multiplayer` to check that a two-slot server ignores the overrides, or `--validation` with a debug binary and the Vulkan validation layer installed.

The horizon uses a nonoverlapping quadtree of tiles, each with a fixed 16 by 16
surface grid. Nearby tiles span one chunk; distant tiles cover progressively
larger areas with the same triangle count. Shared integer height sampling keeps
the landscape deterministic at negative and very large chunk coordinates.
Downward skirts cover gaps between different resolutions. A nearby fallback tile
is hidden only after its detailed GPU mesh is ready, including an intentionally
empty mesh, so an excavated area is not filled back in by its fallback.

A separate worker generates the horizon without building voxel octrees or
querying storage. Selection and CPU caching are capped at 512 tiles, with eight
outstanding jobs and a 32 MiB CPU mesh budget. Near and far GPU meshes share
`r_octree_mesh_mb` and the per-frame upload target; detailed terrain takes priority.
With LOD active, up to 8 MiB (at most one quarter of the total GPU budget) is
reserved for the horizon so detailed meshes cannot starve it. Nearest detailed
chunks are admitted first within the remaining share; spare capacity can also
hold horizon tiles. Lowering a live budget can take three renderer fence advances
to release already submitted buffers.
`octree_render_stats` and `world_stats` report draw counts, triangles, GPU memory,
tile cache usage, and pending work for tuning.

The far horizon approximates the **base procedural landscape**. It does not
represent edits, caves, overhangs, buildings, or remote entity state outside the
loaded detailed neighborhood. Those become visible through authoritative voxel
meshes nearby. Empty-generator worlds have no procedural horizon. Changing the
view distance does not expand simulation, collision, or multiplayer chunk traffic.

The core and cache tests run with `meson test -C build`. On Linux with a graphical
Vulkan session and Quake assets, run the isolated visual and frame-timing check:

```sh
python3 tests/run_terrain_lod_smoke.py --binary build/vkquake --basedir /path/to/quake
```

It compares the same view with LOD disabled and at 32768/65536 units, then checks
negative and large coordinates, budget changes, empty worlds, and map resets.
Its temporary output directory contains screenshots, renderer counters, and
normal host-frame interval measurements; the timings depend on the machine.

### Terrain lighting

Nearby terrain and the horizon use smooth slope normals. Generated slopes use
the continuous terrain generator, while edited surfaces use the local density
gradient. In block mode, walls, excavations, and raised blocks keep hard normals.

Terrain receives per-pixel sunlight and ambient sky lighting using the sky's
existing `r_sun_angle`, `r_sun_azimuth`, and `r_sun_intensity` controls. Changing
the sun updates shader constants without rebuilding or uploading terrain meshes.
Material detail uses mipmapped textures and filtered procedural variation to
avoid subpixel shimmer. This lighting does not yet cast terrain shadows or add
local entity lights.

The lighting smoke check captures fixed camera views under several sun settings,
checks that geometry and allocation remain stable, and records frame intervals:

```sh
python3 tests/run_terrain_lighting_smoke.py --binary build/vkquake --basedir /path/to/quake
```

### Cloud horizon and depth

Volumetric clouds form a curved shell above the local world, so the cloud deck
continues naturally toward and slightly below the horizon. Cloud height,
thickness, coverage, density, scale, and wind use the existing `r_clouds_*`
controls. Canonical noise coordinates remain stable during large-world travel.
Atmospheric horizon haze extends below the local horizon to fill distant gaps
when flying above the curved deck.

Clouds composite after opaque terrain and before transparent surfaces. Each ray
stops at the opaque scene depth: nearby ground and roofs hide clouds behind them,
while flying above the layer lets clouds obscure the ground below. The renderer
reads the existing depth attachment, including MSAA samples. Clouds normally
render at half width and height, with nearest filtering for both the noise and
the enlarged image. A depth check skips blocks hidden by opaque geometry and
uses full-resolution depth-clipped rays at mixed boundaries, so reducing cloud
resolution does not paint clouds over foreground terrain. The full-resolution
path remains available on devices without sampled-depth support. Adaptive rays
start with 24–80 front-to-back intervals, with smaller intervals near the camera
on long grazing rays and up to four light samples. Reduced-resolution rays
progressively pair distant intervals to reduce work while preserving their
nearest 32 intervals; full-resolution rays retain the original integration.
Noise detail fades with sample footprint, and opacity integrates each interval's
length instead of turning isolated samples opaque. Stopped wind also freezes
the sampling jitter. Turning clouds off skips the cloud composition pass.
Deterministic stochastic blending reduces repetition in the dominant noise
octave, and the periodic 3D texture removes discontinuities at tile boundaries.
The pattern remains anchored to canonical coordinates during origin rebasing.

`r_clouds_resolution` selects the resolution divisor (`1`, `2`, or `4`; default
`2`). `r_clouds_filter 0` preserves the crisp nearest-filtered appearance;
`1` enables linear filtering. `r_atmosphere_lut 1` caches the atmospheric
integration in a 256×128 HDR lookup texture until the sun direction changes;
`0` evaluates it directly. These settings are saved in the player configuration.
There is no temporal accumulation or frame-to-frame smoothing.

Cloud and atmospheric radiance use the neutral AgX display transform before
entering the scene color buffer. Cloud opacity remains independent of tone
mapping. These lighting passes apply `contrast` before AgX and reserve matching
headroom for the final display pass, retaining highlight detail even at elevated
sun intensity; the existing `gamma` adjustment still applies afterward. The
shared shader uses the MIT-licensed [minimal AgX implementation by Benjamin
Wrensch](https://iolite-engine.com/blog_posts/minimal_agx_implementation).

The horizon regression captures views below, inside, and above the layer and
checks quality settings, foreground occlusion, large coordinates, odd dimensions,
render scale, partial viewports, and MSAA/video restarts. Add
`--lighting-checks` to capture bright sunlight, sunset, and maximum contrast:

```sh
python3 tests/run_cloud_horizon_smoke.py --binary build/vkquake --basedir /path/to/quake
```

See [sky performance](sky-performance.md) for isolated uncapped benchmarks,
GPU diagnostics, and the distinction between host-frame and GPU measurements.

### Terrain materials

Terrain uses seamless grass, granular soil, fractured rock, and sand textures.
Natural cover blends grass and soil in broad patches; steeper surfaces expose
rock. The generator's surface and interior material IDs share this continuous
cover, removing the former horizontal green bands. Placed rock (`world_place 3`)
and sand (`world_place 4`) blend with neighboring materials on smooth meshes.
In empty-generator worlds, material 1 uses natural cover and material 2 uses soil.

Textures project from three world axes so excavated walls and overhangs do not
stretch. Near meshes and the distant horizon use the same material coordinates
and fragment-level blend rules. Integer chunk phases keep textures anchored
through negative coordinates, very large coordinates, and floating-origin shifts.
The horizon still approximates the base terrain and cannot show distant edits.

Four deterministic 256×256 textures are generated once and retained for renderer
restarts; no external texture pack is required. Full mip chains, linear filtering,
and the existing anisotropic filtering control keep distant detail stable.
At full resolution they use about 1.33 MiB of GPU texture storage, plus 1 MiB of
retained source pixels. GPU vertices remain 32 bytes. Material weights are built
with the mesh, and stationary terrain requires no further uploads or mesh work.

The material smoke check records matched before/after views and timings, checks
idle work, edits, distant coordinates, and repeated renderer restarts:

```sh
python3 tests/run_terrain_material_smoke.py --binary build/vkquake --basedir /path/to/quake
```

### Smooth terrain geometry

`r_octree_smooth 1` enables marching tetrahedra by default in terrain worlds.
The background mesh worker splits each surface cube into six consistently
oriented tetrahedra and interpolates the zero-density surface. Unedited terrain
uses the continuous procedural height, removing the visible eight-unit terraces.
Stored voxel edits change the sampled density, so excavations and placed cells
also produce smooth surfaces. The distant heightfield LOD samples the same
continuous height function.

Meshing uses immutable snapshots of the chunk and all 26 neighbors. Updates to
face, edge, and corner neighbors invalidate the affected geometry. Shared density
samples and clipping at chunk boundaries keep adjacent meshes joined; missing
neighbors temporarily use the base generator until their authoritative cells
arrive. Vertex/index storage is included in the existing CPU and GPU budgets.
The coarse LOD provides coverage while detailed meshes build or wait for space.

Terrain collision uses the same density samples and tetrahedra as the smooth
mesh. Swept player boxes contact the complete surface under their footprint,
including slopes, excavations, ceilings, and placed cells. Collision samples
the authoritative cells immediately; it does not wait for mesh jobs or GPU
uploads. Point-contents queries follow the smooth solid, while liquids retain
their nonblocking contents. Unloaded chunks remain solid streaming barriers.
Grounded walking follows walkable terrain without adding gravity-induced
downhill drift; jumping and loss of support still use normal airborne physics.

Save files and network cells retain the eight-unit voxel format. Edit selection
still targets stored voxel faces, and placement checks include the half-cell
region affected by interpolation. The world wire protocol is version 5, including planet descriptors and solid
assets. Older clients are rejected; existing world databases remain compatible. Terrain collision is always smooth on servers and clients.
`r_octree_smooth 0` is a rendering comparison switch only and does not change
physics or rewrite the world. Empty-generator worlds retain block collision.

To compare both meshers and check edits, streaming, budgets, and restarts:

```sh
python3 tests/run_terrain_smoothing_smoke.py --binary build/vkquake --basedir /path/to/quake
```

The collision tests compare traces against rendered triangles and test player
footprint support, seams, edits, liquids, and large coordinates. The graphical
movement check exercises walking, jumping, excavation, and save/load:

```sh
python3 tests/run_terrain_collision_smoke.py --binary build/vkquake --basedir /path/to/quake
```

### Engine memory allocation

Engine heap allocations use on-demand mimalloc (or the configured CRT/SDL
allocator); there is no fixed Quake hunk reservation or `-heapsize` requirement.
Jolt's collision queries also allocate on demand. The unused startup scratch
heap and job pool have been removed. Chunk, mesh, and GPU cache budgets remain
explicit limits on retained resources, rather than startup heap reservations.

## Architecture

A canonical position contains three signed 64-bit chunk coordinates and three
float offsets normalized into `[0, 1024)`. Checked coordinate operations handle
negative chunks and reject overflow. Simulation and rendering work with bounded
local differences. A sparse octree represents each chunk with 8-unit finest cells;
homogeneous children collapse back into a single leaf.

An owned worker handles generation, storage, and meshing. The main thread publishes
immutable chunk and mesh snapshots to rendering workers. The renderer uploads a
chunk only when its mesh serial changes, including changes to neighboring
boundaries, and retains the resulting indexed GPU geometry between frames. Mesh
vertices remain chunk-local; a camera-relative matrix places them in the view.
Coplanar cells merge into larger square faces. Their perimeter vertices remain
on the finest 8-unit grid, so different merge sizes meet without T-junctions.
Distance and frustum checks reject distant or invisible chunks. Retired buffers
are freed after the double-buffered renderer has advanced enough frame fences.
The cloud sampling phase is reduced from integer chunk coordinates before float
conversion so an origin change does not restart the cloud pattern.

The `QUIVER_WORLDPOS_V1` QuakeC extension has an SDK in
[`qc/worldpos.qh`](../qc/worldpos.qh). It represents positions with opaque vector
handles whose actual coordinates live in the engine. The API provides allocation,
copying, entity-position access, local offsets, distance/direction, contents,
terrain box traces, and radius iteration. Assigning a handle aliases it; freeing
it, deleting its owner, or resetting the world invalidates aliases. `wp_delta`
rejects differences exceeding 1,048,576 units on any axis. Terrain traces exclude
entities and return their end position through the supplied handle.

Mods register custom vector fields and globals with `wp_register_field` and
`wp_register_global`, normally from `worldspawn`. Checkpoints preserve registered
positions and aliases, then remap them to fresh handles on load. Temporary and
unregistered handles expire. A stale nonzero registered token makes saving fail;
clear references after freeing a handle or deleting its owner. Multiplayer saves
reset player fields and references to player-owned handles. Native engine vector
fields and globals cannot be registered as position handles.

The version 5 network extension sends the generator descriptor, authoritative
terrain roots and epochs, and bounded Merkle proofs through the existing reliable
game connection. Clients generate an unmodified chunk only after verifying a
proof that its override is absent. Modified chunks require an inclusion proof and
the matching SHA-256 object before installation. A descriptor mismatch is
rejected. Chunk revisions prevent stale replacements, and successful remote
edits are acknowledged after commitment. Every verified chunk is acknowledged by
its receiver; unacknowledged transfers retry without blocking other nearby chunks.
The host's local client shares its server world and needs no terrain transfer.

Connected clients can serve verified, immutable terrain objects over a separate
nonblocking TCP listener. The server supplies peer addresses derived from their
game connections and rotating session tokens. Receivers verify object hashes and
the authoritative proof before caching or installing peer data. The first client
receives objects from the server; later clients can fetch them from reachable
peers. If a peer is unavailable, rejects a request, or returns invalid bytes, the
reliable server transfer supplies the object. Peer data never approves an edit.

With GameNetworkingSockets enabled, clients also negotiate encrypted reliable UDP
connections using native ICE and configurable public STUN. The game connection
relays bounded rendezvous messages between peers introduced by the same server.
Each identity binds a fresh world-session nonce and a server-issued connection
ID; clients cannot choose another client's signaling source identity. A direct
peer path is preferred, with TCP and authoritative server delivery available when
ICE fails. TURN/Steam relay services are not required or configured.
See [NAT traversal setup and controls](world-networking.md).

The TCP backend supports reachable IPv4 peers. The verified client cache is
capped at 32 MiB and object responses at 1 MiB. Each backend accepts at most
eight incoming connections; queued fetches share an eight-request limit across
both backends. The ICE backend caps all live connections at 16. Shared I/O is limited to
256 KiB per pump and 2 MiB/s per client, with bounded timeouts. Peer authorization
expires and disconnecting closes the listener and clears cached session data.
`world_stats` reports served/fetched objects, bytes, cache size, and rejected
objects, plus NAT connection/transfer counts. Checkpoints, entities, globals, and
player data remain server-private. Protocol 4 requires matching clients/servers.

The engine reserves up to 128 client slots. `-listen 64` and `-dedicated 64`
request 64 slots at startup for an octree session; the default player count is
unchanged. Ordinary BSP multiplayer remains limited to 16 players. The complete
scoreboard paginates larger sessions, while the small HUD scoreboard still shows
nearby ranks. Sixty-four actual UDP clients have completed signon, sent movement
and attack inputs, and received world frames/chunk fragments, including one
client at a distant global position. The smoke also verifies remote edit
acknowledgments, duplicate-request replay, stale-revision rejection, and
player-overlap rejection. It disables deathmatch/coop and waits for confirmed
godmode on every client before sending movement and attack inputs, then checks
that the distant player remains alive. This isolates transport from base-game
death/respawn logic. It validates transport, edit handling, and capacity; combat
outcomes and full gameplay performance remain unqualified.

## Current limitations and acceptance work

This is a foundation for massive worlds, not completion of the full engine
migration described in the design:

- Native entities have canonical sidecar positions, but existing QuakeC still
  executes in temporary local frames. Mods using absolute vector arithmetic,
  cross-region entity references, or cached coordinates need the proposed
  `QUIVER_WORLDPOS_V1` API and a source migration. Direct position-field access to
  entities outside the current local frame fails explicitly; nearby legacy
  position access remains experimental. A port of the full base-game QuakeC is
  not delivered here. For example, base-game `SelectSpawnPoint` can access a
  distant spawnpoint through native `origin` and hit this guard after a player
  dies far from the initial region. The transport smoke's godmode isolation does
  not qualify that respawn path.
- Nearby voxel meshes retain the finest cell size; the terrain horizon uses the
  bounded heightfield LOD described above. General three-dimensional voxel LOD,
  authoritative distant edits, geomorphing, production lighting, and liquid
  rendering require further work.
- Explicit entity checkpoints and registered position-handle remapping are
  supported, with the compatibility and persistence limits above. Automatic
  paging is limited to opt-in self-contained NPCs. General entity graphs, full
  moving-entity/trigger semantics, and a complete base-game QuakeC migration still
  need implementation and qualification.
- Peer transfer has reliable server fallback, but thorough loss,
  congestion, disconnect, and gameplay-latency tests remain necessary. Remote
  editing currently has a server-wide enable switch rather than per-player
  building permissions.
- Raising player capacity and testing codecs does not demonstrate a stable
  64-player game. The included UDP smoke test covers signon, movement/attack
  command transport, chunk delivery, edit request/revision handling, and one client
  a trillion units away from the others. Sustained combat/edit load tests,
  tick-deadline measurements, sustained-travel memory checks, and full gameplay
  regressions are outstanding.

The standalone core, protocol, and peer tests exercise coordinates, octree
storage, collision/meshing, sparse overrides, deterministic generation fixtures,
canonical roots, snapshot deduplication, crash recovery, migration, corruption
rejection, garbage collection, and bounded TCP transfers. Runtime Vulkan, gameplay,
and multiplayer checks should be recorded separately rather than inferred from
those tests.

To rerun the isolated UDP smoke test with existing game assets:

```sh
python3 tests/run_world_network_smoke.py --binary build/vkquake --basedir /path/to/quake --clients 64 --edit-checks
```

It links game assets into a temporary directory and emits a JSON summary plus a
server log. It does not modify the supplied game directory's configuration or
saves. The clients exercise the protocol without opening 64 rendering windows.

To verify whole-world snapshot restore and actual client-to-client transfers:

```sh
python3 tests/run_world_snapshot_smoke.py --binary build/vkquake --basedir /path/to/quake
python3 tests/run_world_peer_smoke.py --binary build/vkquake --basedir /path/to/quake
```

The peer smoke opens two real graphical engine clients and tests successful
exchange, provider departure, and corrupt peer bytes followed by server fallback.
It requires an available SDL/Vulkan display. The snapshot smoke uses isolated
dedicated sessions and verifies terrain/player rollback, epoch changes, named
pin management, and collection.

To run the isolated entity-checkpoint persistence smoke:

```sh
python3 tests/run_world_checkpoint_smoke.py --binary build/vkquake --basedir /path/to/quake
```

This harness creates its own temporary world database and checks saving and
restoring entity state across a server restart. It does not establish complete
mod compatibility or multiplayer progression persistence.

To exercise opt-in NPC paging with an original generated QuakeC fixture:

```sh
python3 tests/run_world_paging_smoke.py --binary build/vkquake --basedir /path/to/quake
```

The fixture moves a real UDP player's interest between distant regions. It checks
unloading, restoration into a different entity slot, self references, owned
position aliases, stable identity, reference pinning, and both dormant and active
checkpoint restarts. A player near the paging radius edge checks the actor's exact
position rather than its chunk center. SQLite checks cover the page manifest and
revision behavior.
It also checks cancellation of snapshots changed during asynchronous writes and
a cold restart before the first explicit save. The four-phase smoke has passed.
This is a functional paging smoke, not an NPC combat or sustained-load benchmark.

To check real walking across streamed chunk boundaries:

```sh
python3 tests/run_world_travel_smoke.py --binary build/vkquake --basedir /path/to/quake
```

This isolated UDP client holds a fixed westward direction and normal forward
movement, without teleporting, noclip, jumping, or attacking. It requires three
canonical chunk-boundary crossings within 45 seconds and records player height,
grounded frames, chunk delivery, sampled server RSS, and world diagnostics. The
run passed after smoothing the generated terrain's previously impassable ledges.
It checks ordinary streaming travel; it does not inject delayed or missing chunks
or establish a long-duration memory bound.
