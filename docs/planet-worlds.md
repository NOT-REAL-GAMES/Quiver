# Planet worlds and editable construction

The `planet` generator is opt-in. Existing `terrain` and `empty` worlds keep their generator identity and extraction path. Planet worlds use a finite periodic horizontal surface, flat gameplay gravity, and an optional Earth-radius visual horizon bend. They are not spherical planets.

## Creating and editing

```cfg
world_create example planet 1
world_stats
```

The first creation prepares the global map before entering play. The default surface wraps every **1,250,048 chunks**, or **40,001,536 metres** at 32 Quake units/metre. Both axes wrap; vertical coordinates remain unbounded within engine coordinate limits. New worlds use **generator version 2**: spawn lies in a seeded lowland, with a small dry, flat starting patch and gentle surrounding relief. Existing version-1 worlds retain their original geometry and edits; create a new world name to use the revised generator.

```cfg
world_brush add box 192 160 192 0 25 0
world_brush subtract box 64 256 96 0 25 0
world_brush add wedge 192 160 128 0 -20 0
world_import building.obj
world_save
```

Brushes are centered at the current world position. Enable free flight with `noclip` before placing camera-centered brushes, so the new solid cannot trap the player. Dimensions are world units; optional rotation is pitch/yaw/roll in degrees. Imports accept a watertight OBJ solid, optionally followed by `add` or `subtract`. Mesh validation rejects malformed/open/nonmanifold geometry and unsupported sub-voxel features. Eight units is the minimum supported construction feature size. These are console construction tools; there is no graphical building editor.

Construction is intentionally bounded:

- A solid edit may touch at most **256 chunks**, with at most **256 accumulated solid operations per chunk**.
- OBJ input is limited to **8 MiB**, **8192 vertices**, **16,384 triangles**, and **256 vertices per polygon**. Polygon faces must be planar and convex; triangulate more complex faces before import.
- Geometry must be watertight, consistently oriented, non-self-intersecting, and welded. Hard edges and opposing-face thickness must meet the **eight-unit (25 cm)** minimum resolution. Vertices are bounded to 1,048,576 units from their anchor and serialized at 1/1024-unit precision.
- Existing resident, pinned-chunk, mesh, and encoded-object budgets still apply. Imported geometry is included in resident memory accounting. An edit exceeding a budget fails with an error rather than silently reducing detail.

The importer preserves face material assignments. Recognized `usemtl` names (`grass`, `soil`/`dirt`, `rock`/`stone`, `sand`) select built-in materials; other names receive deterministic material IDs. External MTL texture files are not imported.

`world_teleport <chunk-x> <chunk-y> <chunk-z> [local-x local-y local-z]` accepts periodic aliases. For example, horizontal chunk `-1` refers to chunk `1250047`. `world_load example` restores the persisted generator settings, edits, and checkpoint.

For an **existing generator-v1 world with the original default settings and seed 1**, this historical tour leaves the spawn area. Version 2 chooses a different seeded map origin, so these coordinates are not a version-2 tour. `setpos` fixes the view angles and enables free flight if needed:

```cfg
noclip
// Mountain ridge
world_teleport 908238 322278 130 512 512 512
setpos 512 512 512 20 225 0
// Rocky cliff with local overhangs
world_teleport 927770 283214 118 512 512 600
setpos 512 512 600 10 0 0
// Coast and sea-level ocean
world_teleport 673854 1103558 0 512 512 768
setpos 512 512 768 15 90 0
```

Run each destination separately and allow its terrain to stream before moving to the next. These coordinates are seed-specific.

## Generation and persistence

A versioned **4096×4096** global elevation map establishes continents, ocean basins, and mountain belts. Twelve deterministic hydraulic transport iterations accumulate runoff and deposit sediment; eight thermal iterations relax excessive slopes. Periodic neighbors make the global seam continuous.

Regional generation covers **256×256 chunks**, approximately **8.192 km square**, on a **32 m grid**. Global elevation/runoff guide finer ridges and valleys, followed by twelve hydraulic and eight thermal iterations. A halo covers the complete iteration dependency radius, so neighboring region samples agree independently of generation order. Smaller procedural rock detail is sampled below the erosion grid resolution. Moisture, shoreline elevation, and slope select natural materials; temperature and deposited sediment are exposed in the sampling API for future richer biome blending.

All simulation and height interpolation use fixed-point integer arithmetic. The descriptor identifies generator version, seed, extent, global resolution, erosion counts, sea level, and the existing material identity. Unsupported versions are rejected. Changing generation constants requires a new version, rather than silently altering existing terrain.

Version 1 forced the global origin to 4096 units above sea level, then blended down to a 128-unit spawn pad over 16,384 units. The resulting similar ramp across seeds, combined with fine noise and three-dimensional rock displacement, could be difficult to walk. Version 2 selects a gentle lowland from the eroded seeded map, rotates the periodic elevation/drainage maps to place it at spawn, and recenters that lowland to a modest seed-dependent height. Lower microdetail amplitude in lowlands avoids triggering cliff-scale rock displacement there; mountains retain stronger detail. The original version-1 branch and its golden height fixtures remain unchanged.

Global data is saved as a disposable checksummed `.planetcache` beside a persistent world database. Cache writes publish through a temporary file and rename. A cache mismatch or corruption causes deterministic regeneration. Regional caches are bounded to 32 entries by default and regenerated after eviction. Terrain edits, imported solid definitions, and snapshots use the world's durable content-addressed storage, independently of these generated caches.

Quiver world networking now uses **protocol version 5** for planet descriptors and content-addressed solid assets. Multiplayer peers must run compatible builds; older Quiver world clients are rejected. Existing legacy world databases retain their generator behavior, and older terrain override objects remain readable. The network protocol change does not automatically convert a legacy world into a planet.

Global generation is explicit; a cold regional query generates synchronously on its caller. Streaming, terrain LOD, and collision-extraction workers perform cold queries before their surfaces are published. Regional generation runs outside the short cache lookup/publication lock, so unrelated cached collision queries can proceed while a streaming worker prepares another region. Active queries retain immutable region snapshots safely across cache eviction. Generation cache memory is accounted separately from chunk, mesh, and renderer budgets.

## Geometry and rendering

The planet path uses feature-preserving surface extraction with separate hard-face vertices for construction. Solid definitions retain their geometry rather than being reduced to material-colored voxel edits. Terrain collision and rendering sample the same density field and extraction grid; tests check a known horizontal surface against the resulting mesh and traces. Collision builds bounded **128-unit local patches**, cached in a **32 MiB / 512-entry LRU**, independently of the asynchronous full-chunk render mesh worker. Movement therefore does not wait for a complete render mesh. After an edit, collision waits asynchronously for geometry matching the new revision; the player update is deferred when the required new hull is not ready. The renderer may briefly retain its previous uploaded mesh. Fully atomic GPU-and-collision publication is not implemented.

Smooth terrain uses a regularized, cell-constrained quadratic-error solver for its surface vertices. Nearly parallel terrain normals can otherwise send the unconstrained solution outside its cell; clipping each coordinate independently produces small vertical risers with stretched grass textures. The solver now minimizes the error on the cell's faces, edges and corners when necessary, with additional conditioning for smooth surfaces. Hard and mixed construction components retain their existing sharp-corner placement. Rendering and collision use the same corrected vertices, including across extraction-patch boundaries. Reloading a world rebuilds these meshes; saved seeds, edits and global generation caches need no migration or deletion.

Arbitrarily rotated wedge tips can still produce tiny folded triangles at subcell scales. An audited wedge rotated on all three axes retained these folds over approximately 0.14% of its surface area; tested rotations had no open topological edges. This is a remaining extraction-quality limit, even when the input solid meets the minimum feature-size checks.

The eroded heightfield establishes mountains and valleys. A periodic three-dimensional rock displacement adds local overhangs on steep slopes while leaving flat terrain and the spawn patch unchanged. Brush subtraction/addition supports larger caves and architectural forms. This is localized volumetric rock detail, not a three-dimensional erosion simulation.

Distant terrain LOD samples the same prepared generator and includes sea-level ocean surfaces. Coarse sampling uses global maps and procedural detail without generating regional erosion grids; transition skirts cover the small differences from nearby eroded surfaces. Render-only strips connect actual detailed chunk boundary edges to their adjacent LOD surface without changing collision. `r_planet_curvature 1` enables visual curvature; the archived default is `0`. The visual deformation leaves local physics and gravity flat. Existing clouds, depth, entity, and transparency paths receive the matching camera-relative curvature parameters.

Curvature leaves the first **4096 units (128 m)** flat and blends to the Earth-radius bend over the next 4096 units. Construction selection works within this flat zone. Gameplay gravity, collisions, and hitscan traces remain in flat coordinates; precise screen-space targeting across a visibly bent distant horizon is not implemented. Leave curvature disabled when testing long-range aim.

Distant terrain is a heightfield approximation. It does not retain individual volumetric overhangs or architectural silhouettes beyond the detailed chunk rendering range (`r_octree_distance`, default 4096 units). Ocean surfaces are static, with no fluid simulation. This is an initial streaming-world implementation, not a complete planet ecosystem or city renderer.

## Measurements and validation

Historical standalone release measurements on this workstation, generator version 1 with the original default settings and seed 1:

| Operation | Time / memory |
|---|---:|
| Prepare 4096×4096 global map | 9.173 s |
| Generate one cold regional map | 24.31 ms |
| One million warm height samples | 37.76 ms |
| Global map plus one region | 202,119,180 bytes |
| Peak process RSS during preparation | 462,392 KiB |

These CPU measurements exclude scene meshing, graphics, and cache-file I/O. Global retained data is 192 MiB; 32 regional entries add approximately 24.2 MiB. Memory remains bounded independently of explored area.

Version-2 spawn probes used the default 4096×4096 map for seeds 1, 2, 123, 777, and UINT64_MAX, sampling a 32,768-unit square around spawn at 128-unit spacing. The largest observed heightfield slope was **0.1052 (about 6°)**, and the resulting three-dimensional rock amplitude stayed below **0.88 units**. Six-seed small-map regression probes also bound the applied rock-normal slope conservatively to 0.1732, below the walking threshold; the starting point remains exactly 128 units above sea level. This fixes terrain shape rather than relaxing the player movement rules.

`planet_generation_test` covers settings validation, golden global hashes for versions 1 and 2, exact preserved version-1 spawn heights, version-2 walkable seeded surroundings including the rock displacement trigger, independent generation and reversed region order, periodic coordinates including integer limits, erosion's effect on elevations, region-boundary continuity, bounded eviction/regeneration, concurrent cold-region queries, and cache corruption/version/identity checks. GNU and Clang produced matching golden hashes; AddressSanitizer and UndefinedBehaviorSanitizer passed.

`planet_world_test` covers descriptor replication, wrapped chunk identities and edits, proof installation, insertion-order-independent content roots, snapshots, loading persisted planet metadata through an ordinary default configuration, cache deletion/regeneration, multi-height-chunk occupancy, analytic mesh/collision agreement at the spawn plateau, and downward-facing natural overhang crossings on a steep terrain fixture.

`world_solids_test` also checks smooth analytic terrain for surface error and geometric face orientation, alongside its sharp-corner and rotated-wedge checks. On an 8-unit grid, the curved fixture `10*sin(x/32)*cos(y/37)` previously produced 554 incorrectly steep or flipped faces and a maximum vertex height error of 3.755 units. The corrected solver produces zero such faces and a maximum error of 0.135 units. Three alternating standalone runs measured median CPU extraction times of 68.23 ms before and 68.14 ms after; these are mesh-build costs, not GPU or application frame times. The gentler `5*sin(x/64)*cos(y/71)` fixture's maximum error fell from 1.136 to 0.018 units.

`tests/run_planet_surface_smoke.py` captures identical cameras with terrain LOD enabled and disabled, preserving a copy of the tested binary and isolating settings and world data. Use `--source-world` to copy an existing SQLite world, `--global-cache` for its matching generation cache, and repeat `--view NAME X Y Z PITCH YAW ROLL` to specify cameras. A version-1 seed-1 grass slope at `(3200,128,450)`, pitch 15 and yaw 45, reproduced the grass ribbons with both LOD settings. The [original capture](/tmp/quiver-planet-surface-6wv7b7lu/grass_lod0.png) and [corrected capture](/tmp/quiver-planet-surface-0phhl5u0/grass_lod0.png) show their removal without changing textures or the saved generator. These are temporary workstation artifacts. All 21 automated tests pass in release and debug after this correction, including bit-exact full-mesh/collision-region agreement.

The corrected grass slope also passed [debug capture with Vulkan validation](/tmp/quiver-planet-surface-6hn6iqm6/summary.json), with the validation layer active and zero errors through graceful shutdown. A [focused release capture run](/tmp/quiver-planet-smoke-_pcte97n/summary.json) preserved construction corners, the doorway, natural ridges and cliffs, and completed the MSAA/cloud redraw checks.

Post-correction movement also passed in the [saved version-1 world](/tmp/quiver-planet-movement-elidow_r/summary.json) and all 12 [version-2 controller checks](/tmp/quiver-planet-movement-6wdg0my4/summary.json), including jumping, elevated floors, stairs, wall/ceiling contacts, support removal and return to BSP gameplay. At 144 FPS with clouds off, natural walks covered 2894 and 2900 units; their maximum frame intervals were 7.03 and 6.98 ms. The largest measured movement frame across the checks was 7.10 ms. Every physics checkpoint reported zero main-thread extraction and cooking. Natural traversal deferred 9 and 8 updates respectively while worker geometry became ready, without a cell-crossing frame spike in these runs.

The isolated graphical harness is:

```sh
python3 tests/run_planet_smoke.py --binary build/vkquake --basedir .
```

It links only game assets into a temporary directory and leaves saved player configuration untouched. It exercises curvature comparisons, construction/imports, save/reload, wrapping, mountain/cliff/coast destinations, MSAA restart, and clouds over the ocean. Its frame measurements are host-frame intervals, not isolated GPU costs. Captures require visual review. `--render-focus --capture-only` limits a rerun to construction, mountains, and MSAA/cloud redraws, and explicitly labels that reduced scope in its report.

Full release and debug graphical smoke runs completed on this workstation. The debug run enabled `VK_LAYER_KHRONOS_validation` and reported zero Vulkan validation errors, including the 4× MSAA restart and cloud composition. An earlier unoptimized debug run exceeded the original 60-second cliff-meshing deadline while still making progress; the complete rerun passed with a bounded 180-second per-stage deadline. Use `--validation --stage-timeout 180 --timeout 1200` for that debug check, optionally with `--global-cache` pointing to a cache from the exact same generator version/settings/seed.

Local verification artifacts from 2026-10-26 contain all 18 checks and their captures: [release report](/tmp/quiver-planet-smoke-u_9q887t/summary.json), [debug validation report](/tmp/quiver-planet-smoke-f9q4cdfq/summary.json), and the [earlier bounded debug timeout](/tmp/quiver-planet-smoke-y2qrqcep/summary.json). These are temporary workstation artifacts; the harness reproduces them in a new directory.

After the final transition/skirt shading and solid triangulation fixes, the focused ten-check rerun passed in [release](/tmp/quiver-planet-smoke-i57c_axo/summary.json) and [debug with Vulkan validation](/tmp/quiver-planet-smoke-tshw06sd/summary.json). The debug report confirms the validation layer was active and recorded zero errors, including shutdown. Reviewed [construction](/tmp/quiver-planet-smoke-i57c_axo/construction.png) and [mountain](/tmp/quiver-planet-smoke-i57c_axo/mountains.png) captures no longer show the earlier white boundary gaps or dark dotted LOD seams. The fully rotated subcell tip limitation described above remains.

## Walking and cell-crossing regression

Streamed worlds now use **Jolt `CharacterVirtual`** for player movement and Jolt triangle-mesh queries for terrain sweeps. Quake gamecode still owns input acceleration, jumping, gravity, water behavior, entity thinking, and touch/trigger callbacks. The controller uses the player's original box bounds, Z-up coordinates, an 18-unit stair limit, a 0.125-unit collision safety clearance, and the existing walkable-slope threshold. The clearance remains below the terrain extraction grid resolution and preserves the original box dimensions and flat-floor foot height. The controller uses cooked mesh active-edge contacts; Jolt's optional enhanced face filtering is disabled because it discarded a necessary upcoming slope contact in reproduced terrain-edge freezes. Positions and velocities are converted at 32 Quake units per metre; Jolt sees a small local frame anchored by integer chunk coordinates, preserving altitude and wrapping precision.

Nearby solid entities are mirrored as kinematic box proxies with their entity identities. Quake retains entity simulation and moving-platform ownership; this is not a conversion of every entity into an independently simulated rigid body. Brush-model entities keep their exact Quake hull sweeps, and ordinary BSP maps retain their existing collision path. Controllers and streamed Jolt scenes are destroyed before global Jolt type registration is shut down.

Collision extracts only the 128-unit patches intersecting a swept hull. Planets and construction use the same Hermite roots and feature vertices as rendering; the older `terrain` generator uses the exact marching-tetrahedra surface, clipped to patch bounds. Adjacent patches share boundary geometry, with coplanar faces assigned to one tile. Geometry is cached independently of renderer jobs, invalidated by terrain/neighbor changes, and bounded to 512 entries and 32 MiB. Each Jolt scene caches cooked immutable shapes separately, bounded to 4096 entries and 32 MiB. Character queries weld their active neighboring patches into one local mesh so shared tile borders are internal edges, rather than open edges of separate bodies. An unchanged set of surface patches and revisions reuses that mesh across ordinary queries and coordinate rebases; combined cooked-shape memory remains within the scene budget. Live collision extraction and triangle-mesh cooking run on dedicated workers. The synchronous APIs remain available only to offline reference tests.

The owner thread captures immutable chunk-neighborhood snapshots, coalesces requests, and publishes worker products only after checking their source revisions. Collision extraction allows at most **32 pending jobs / 256 MiB of conservatively counted snapshot inputs**. Completed products are limited to **32 MiB**, plus one active result, independently of the ready patch cache. Each Jolt cooking queue allows at most **64 jobs / 32 MiB**. Edited or evicted revisions cancel obsolete extraction requests; stale extraction and cooking results cannot replace current geometry. Shutdown cancels queued work and joins workers before destroying their source world or Jolt registration.

Character queries request a larger region ahead of movement and retain a ready welded hull while all its source revisions remain valid. Missing streamed chunks are distinct from loaded chunks awaiting extraction or cooking. Required unknown or pending regions fail closed; a pending character update keeps its authoritative position and pending velocity/jump impulse instead of doing synchronous work. Point-contents guards use a density lattice baked by the extraction worker, so they do not hide an on-demand mesher or generator call. Storage, render extraction, collision extraction, and Jolt cooking have separate workers.

On the first landing frame, the controller now clears the downward fall component before returning velocity to Quake gamecode. This prevents an old fall speed from canceling a jump impulse pressed immediately after landing, including on descending slopes. The correction preserves horizontal input intent and accounts for moving-support velocity; ordinary grounded frames do not acquire lateral velocity from this landing correction.

`world_stats` identifies the active Jolt backend and reports collision worker builds, main-thread builds, queued snapshot memory, stale results, worker cooks, main-thread cooks, and deferred updates. **Both main-thread build/cook counters must remain zero during live play.** Deferred updates count readiness waits and are reported separately from frame time; low frame times alone do not prove movement stayed responsive.

The isolated movement harness uses ordinary walking and jumping input:

```sh
python3 tests/run_planet_movement.py --binary build/vkquake --basedir . --require-jolt --controller-checks --max-frame-ms 100
```

It walks across several cells on natural terrain, on edited floors at z=1024 and z=133120, and up a gentle wedge; jumps must land back on the expected surface. `--require-jolt` rejects an unverified or legacy collision backend and requires zero main-thread extraction and cooking at startup, after every walking/jumping measurement, and after controller edits. Reports retain these worker counters and deferred-update counts alongside movement distances and frame times. `--controller-checks` adds a 16-unit stair, wall, low ceiling, removal of support under a stationary player, and return to a BSP map. `--world-kind terrain` exercises the older marching-tetrahedra generator. `--max-frame-ms` sets an explicit movement-frame limit; reported host-frame intervals are not isolated physics or GPU timings. `--source-world <database> --natural-only` copies an existing database through SQLite's backup API for a repeatable older-world comparison. Saved player configuration and source worlds are left untouched.

`jolt_stream_test` exercises actual cooked Jolt shapes and `CharacterVirtual`: ray and box sweeps, resting/tangent contacts, patch/chunk seams, pristine legacy-terrain walking across eight subcell approach phases at 72, 120, 144, and 200 Hz plus alternating frame durations (80 combinations), with both immediate and accelerated input, independent continuous-terrain trace comparisons, edited shape invalidation, welded character-mesh reuse across warm queries and rebases, unknown barriers, deep-interior protection, jumps, 16-unit versus 20-unit steps, wall sliding, low ceilings, upward swimming while the ground flag remains set, uphill-to-flat support, kinematic support identity/velocity, huge coordinates beyond 2^53 chunks, high altitude, and walking across the wrapping seam. Disabling smooth terrain rendering must retain the same smooth collision surface; a separate regression compares non-voxel-aligned terrain heights with that rendering control disabled. `terrain_mesher_test` separately compares bounded marching-tetra regions against the scalar collision oracle and verifies tile-edge agreement and unique ownership of coplanar floors; its ASan/UBSan run passed. `world_collision_async_test` compares worker-produced geometry against the synchronous reference for all generators and construction, checks ready/pending/unknown distinctions and cached occupancy, and exercises immutable edit snapshots, stale-result rejection, bounded queues, reuse, and shutdown with work pending.

`tests/run_jolt_entities_smoke.py` creates an isolated original QC fixture and connects an actual UDP client. The player lands on and rides a diagonal moving platform while the fixture checks relative position, ground entity, solid touch, and trigger callbacks. The asynchronous release build passed 118 checked frames: [entity fixture report](/tmp/quiver-jolt-entities-8fyclxpq/summary.json). Its `--brushes` variant generates original BSP29 collision hulls and checks exact brush-model floor sliding, a 16-unit stair, ground identity, and callbacks inside a streamed world; the asynchronous release build passed 236 frames: [brush fixture report](/tmp/quiver-jolt-entities-nmqy22jx/summary.json).

```sh
python3 tests/run_jolt_entities_smoke.py --binary build/vkquake --basedir .
python3 tests/run_jolt_entities_smoke.py --binary build/vkquake --basedir . --brushes
python3 tests/run_jolt_jump_smoke.py --binary build/vkquake --basedir .
```

The asynchronous release and debug builds pass all **21 automated tests**. Both dedicated entity fixtures also pass on the debug build: [moving platform and callbacks](/tmp/quiver-jolt-entities-f6yet0yw/summary.json), [BSP floor/stair and callbacks](/tmp/quiver-jolt-entities-4xhn72nf/summary.json). Native Windows builds have not been validated; the checked Linux library and client code use matching Distribution ABI flags.

`tests/run_jolt_jump_smoke.py` sends real UDP client input to an original QuakeC fixture. It checks a jump held across landing, held-button debouncing, and repeated release/repress cycles. All eight jumps passed on both [release (332 checked frames)](/tmp/quiver-jolt-jump-0j3fgbx5/summary.json) and [debug (322 checked frames)](/tmp/quiver-jolt-jump-6w4bqjo7/summary.json). These dedicated-server checks validate behavior, not graphical frame time.

Final asynchronous graphical validation on 2026-10-27 passed all **12 new-world movement checks**, including both elevated platforms, stairs, wall sliding, ceiling collision, removal of support, and return to BSP gameplay: [release report](/tmp/quiver-planet-movement-io1uhwfk/summary.json). The unchanged [version-1 world](/tmp/quiver-planet-movement-gpabvrg8/summary.json) also passed walking and jumping. Both main-thread extraction and main-thread cooking counters remained **zero** at every checkpoint.

At a 144 FPS cap with clouds disabled, the application frame intervals compare as follows against the [pre-async planet baseline](/tmp/quiver-planet-movement-9q22hq0s/summary.json) and [pre-async version-1 baseline](/tmp/quiver-planet-movement-frsu6jus/summary.json):

| Release fixture | Previous maximum | Async maximum | Deferred updates around walk |
|---|---:|---:|---:|
| Natural version-2 terrain | 16.64 ms | 6.96 ms | 9 |
| Extracted floor at z=1024 | 17.68 ms | 7.09 ms | 7 |
| Extracted floor at z=133120 | 20.16 ms | 6.95 ms | 15 |
| Wedge ramp | 24.25 ms | 6.96 ms | 0 |
| 16-unit stair | 15.48 ms | 6.95 ms | 0 |
| Construction wall | 22.13 ms | 6.95 ms | 0 |
| Unchanged version-1 terrain | 21.11 ms | 6.96 ms | 9 |

All listed asynchronous release means were approximately **6.945 ms**, with 99th percentiles approximately **6.945 ms** and no frames above 100 ms. Natural, low-floor, and high-floor walks traveled **2895.19 / 2917.87 / 2963.42 units**. The unchanged version-1 walk traveled **2749.66 units** and retained support in all 30 sampled walking states. These are whole application input-batch intervals, including scheduling and presentation; they are not isolated physics or GPU timings.

Deferred counts bracket each walk and its final settling check; they measure skipped physical updates, not elapsed time. The worker pipeline can still briefly pause movement when required geometry is unavailable, especially after teleports or edits. The distance and support assertions prevent low frame times from concealing a permanently stalled player. A preliminary landing fix exposed a real old-version cliff by adding lateral velocity on every grounded frame; the final fix preserves horizontal intent and applies its vertical correction only on the first landing frame. Generated geometry was not changed.

The final older `terrain` generator run passed all seven walking/jumping/platform/ramp checks: [legacy-terrain report](/tmp/quiver-planet-movement-nudarbxd/summary.json). Its maximum frame intervals were **6.96 / 7.04 / 6.96 / 6.95 ms** on natural terrain, z=1024, z=133120, and the ramp, compared with **9.72 / 15.37 / 15.68 / 20.79 ms** in the [pre-async baseline](/tmp/quiver-planet-movement-dwd8uspu/summary.json). Corresponding deferred-update counts were **8 / 7 / 8 / 0**; both main-thread counters remained zero.

The final [debug movement report](/tmp/quiver-planet-movement-2tb53dqv/summary.json) confirms walking/jumping passed with native Vulkan validation active and **zero validation errors**, including shutdown. This unoptimized debug/validation run measured a **7.43 ms mean, 22.61 ms 99th percentile, and 69.99 ms maximum**, with no frames above 100 ms. It traveled 2751.40 units and recorded 37 deferred updates around the walk, with zero main-thread extraction/cooking. Debug measurements are separate from release acceptance. Running `--validation` requires an installed Khronos validation layer or correctly configured `VK_LAYER_PATH` and runtime library paths.

The following **historical measurements precede the Jolt controller integration**. They compare the original stall against the bounded-patch Quake controller on the same generator-v1 world, at a 144 FPS cap with clouds disabled:

| Application frame interval | Original build | Fixed build |
|---|---:|---:|
| Mean | 15.62 ms | 6.96 ms |
| 99th percentile | 48.14 ms | 7.15 ms |
| Maximum | 1614.50 ms | 11.38 ms |
| Frames above 100 ms | 3 | 0 |

These measure application frame intervals, including CPU work, scheduling, and presentation; they are not isolated GPU timings. The original build stalled and failed the walking-distance check. The fixed build passed walking and jumping on unchanged terrain: [original report](/tmp/quiver-planet-movement-j0k7jemd/summary.json), [same-world fixed report](/tmp/quiver-planet-movement-bu257adi/summary.json). The seven-check new-world movement run also passed, including both altitude fixtures and the ramp, with no frames over 100 ms: [movement report](/tmp/quiver-planet-movement-w54qz4v9/summary.json).

Those pre-Jolt release and debug builds passed all 19 automated tests. Their regressions verify collision before any renderer mesh exists, repeat-query cache reuse, edit invalidation, normalized coordinates, and exact patch/full-mesh vertex attributes at boundaries. Generator checks retain version-1 hashes/heights and cover version-2 spawn slopes and seed variation.

After the additional extraction optimizations, the final release movement run passed all seven movement checks: maximum frame intervals were **8.79 ms** on natural terrain, **8.12 ms** at z=1024, **8.30 ms** at z=133120, and **10.28 ms** on the ramp, with zero frames above 100 ms. See the [final release report](/tmp/quiver-planet-movement-iglu5s18/summary.json). Its top-level validation request failed because native Vulkan validation is compiled only into debug builds; the report explicitly records that validation was inactive rather than claiming a clean validated release run.

The final [debug v1-world movement report](/tmp/quiver-planet-movement-golmqlgg/summary.json) confirms native Vulkan validation was active with **zero validation errors**, and walking/jumping passed on unchanged older terrain. This unoptimized debug/validation run had a 13.37 ms mean and 152.15 ms maximum frame interval, including three frames above 100 ms; release performance figures above must not be attributed to the debug build.
