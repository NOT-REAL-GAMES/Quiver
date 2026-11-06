# ECS entity stress measurements

## Playable level

In a normal build, open the console and enter `map ecs1024` or `map ecs32768`.
The packaged levels use the same lit arena and player spawn. Their 1,024 or
32,768 test entities are
invisible and inert; each has an ECS boolean component that flips once per
server physics tick while the game is running. Enter `ecs_stress_stats` to
see the entity count, tick and update totals, verification failures, and mean
component-update time. The workload pauses with the game.

With the separate `quiver_ripple` game package, the same entities also receive
visual components and a QuiverScript-authored QuakeC callback moves them in a
Z-axis ripple. Build and launch the package as described in
[QuiverScript](quiverscript.md). This visual workload is local-only: a
host-thread snapshot feeds the local renderer without transmitting the
positions over the game protocol. `ecs_visual_stats` reports the client visual
count, and `ecs_visual_stats <index>` shows one position; `ecs_stress_stats`
also reports the mean script-system time. The original boolean-flip workload
and its counters remain active.

The 1,024-entity variant is a 32×32 grid covering the same arena footprint;
the 32,768-entity variant uses its original denser grid. Both use the same
script and ECS components. To regenerate the packaged smaller BSP with
identical geometry, visibility and lighting, run
`python3 Misc/make_ecs1024_map.py`. To exercise its animation and lifecycle,
run `tests/smoke_ecs_ripple.py` with `--count 1024`; omit that option to test
the 32,768-entity level.

One local release sample at 1280×720 on a Ryzen 9 7900X / RTX 4080, with
ray-traced shadows off and the player looking across the wave, measured
82.3 FPS over three seconds. CPU-side `host_speeds` samples had median total,
server, and graphics phases of 13.18, 4.29, and 9.12 ms respectively. The
ECS flip and scripted ripple reported means of 0.063 and 2.99 ms per tick;
process RSS was approximately 615 MiB. These are separate, short measurements,
not additive GPU timings; instrumentation itself has overhead.

After batching the local marker draws and reusing unchanged ECS visual
snapshots, three alternating release runs on the same Ryzen 9 7900X / RTX 4080
at 1280×720 measured a median **888 FPS** (1.126 ms/frame), versus **84.6 FPS**
(11.821 ms/frame) for the preserved original build. Each run measured five
seconds after two seconds of warmup; all one-second optimized samples exceeded
500 FPS, with a minimum of 800 FPS. These runs used 1,048,576 maximum edicts,
`r_rtshadows 2`, MSAA off, vsync off, and an uncapped frame rate. A separate
user game process and desktop applications were running during the comparison.
All 32,768 entities and their scripted motion were checked, and paused images
with `r_ecs_batch 0` and `1` were pixel-identical. The original gamecode was
used unchanged for both binaries. Results include host frame time rather than
claiming this is solely GPU time.

With the default 4× MSAA enabled, the final optimized build measured 885, 890,
and 865 FPS in three alternating five-second runs at the same resolution and
edict capacity, versus a baseline median of 84 FPS. The lowest one-second
optimized sample was 824 FPS. The three paused batched-versus-legacy images
were pixel-identical. In a separate debug run, Vulkan synchronization validation
reported no errors for batching, single-threaded rendering, wireframe fallback,
or an MSAA/video restart.

A short follow-up on the ordinary `e1m1` server found an extra 3–4 µs/tick in
the first optimized build. Restricting the stationary-edict shortcut to large
populations and avoiding an unnecessary empty hash-map check returned its
instrumented server tick from about 0.041 to 0.037 ms in an isolated two-pair
comparison; normal demo frame times were essentially unchanged. Those short
samples are sensitive to scheduling noise.

The benchmark records the original and optimized executable hashes, individual
one-second samples, scene checks, and screenshots. To reproduce it, preserve a
pre-optimization executable and run `tests/bench_ecs_ripple.py` with
`--baseline`, `--candidate`, `--basedir`, and `--progs`; use
`--max-edicts 1048576 --rtshadows 2 --compare-renderer --require-batched 1`.
The benchmark disables the frame cap only inside its disposable configuration.
`r_ecs_batch 0` selects the legacy draw path; wireframe rendering automatically
does so as well.

An experimental GPU path is available for the homogeneous local ripple
instances: set `r_ecs_gpu_cull 1` and use `ecs_gpu_stats` to verify that it is
active and dispatching. It uploads instance data, performs per-instance frustum
tests in a compute shader, compacts the visible instances and writes an
indexed indirect draw. Lighting is still sampled on the CPU; a conservative
block test skips lighting work for blocks guaranteed to be outside the GPU's
frustum test. It does not move ordinary Quake entities or world geometry to
this path. Unsupported resources, wireframe, single-threaded rendering,
`scr_speeds`, and disabled batching use the existing CPU path.
The experimental path's dedicated Vulkan buffers are not included in the
engine's existing Vulkan memory-allocation statistics.

The GPU path is **off by default** because it is currently slower on the test
machine. With 32,768 animated instances at 1280×720, 4× MSAA, ray-traced
shadows set to 2, 1,048,576 maximum edicts, vsync off, and an uncapped frame
rate, two alternating three-second runs measured a median 959 FPS for CPU
culling and 84 FPS for GPU culling. Both paths used the same release binary
and gamecode with the desktop in use. This is a host frame-rate comparison,
not a GPU-only timing. CPU preparation of
lighting for candidate instances remains expensive. Four paused camera views
and an active dynamic-light test matched pixel for pixel; Vulkan
synchronization validation passed on the smaller scene. Results are specific
to this hardware and workload.

Under the same settings, two alternating three-second runs of the final
1,024-instance build measured median host frame rates of 1,677 FPS for CPU
culling and 1,545 FPS for GPU culling, before the mailbox change described below.

On the same Wayland system with vsync off, an instrumented prototype requesting
three mailbox swapchain images instead of two measured 2,679 FPS median in
three alternating five-second 1,024-instance runs, versus 1,684 FPS with two
images. Every one-second prototype sample exceeded 2,500 FPS; successful image
acquisitions matched presentation calls in each run. These are host frames and
present calls, not a claim about the number of frames displayed: mailbox mode
may replace queued frames. The production renderer now requests three images
in this mode when the surface supports them. FIFO, immediate and vsync-on
selection is unchanged. A subsequent comparison of the integrated release
build with the preserved original 1,024-instance fixture measured a median of
2,638 FPS versus 1,606 FPS across three alternating five-second runs. The
lowest one-second integrated-build sample was 2,502 FPS. Both used the same
gamecode and scene at 1280×720, 4× MSAA, `r_rtshadows 2`, vsync off, and
`max_edicts 1048576`. Entity counts, update counts, animation, visible output,
and paused batched-versus-legacy image parity were checked in each run. These
are host frame rates, and the results depend on the hardware and compositor.
A separate verification of the final executable measured 2,621 FPS median
over three five-second runs, with a minimum one-second sample of 2,426 FPS.

The level requires enough `max_edicts` slots for the test entities, the world,
the player, and map entities. If the configured capacity is too low, the map
still loads and reports that the stress workload is inactive. Raise the
setting and reload the map. The playable workload is separate from the
automated benchmark below; its reported mean includes the per-entity checks
performed while flipping the values.

## Automated measurements

These are dedicated-server measurements on an AMD Ryzen 9 7900X, in three
separately loaded generated levels. Each level creates the specified number of
inert QuakeC edicts. The opt-in test system registers an ECS-owned C `bool`
component on each of those exact edict bindings and flips it once per server
physics tick. It verifies every component value, binding, and visit count on
every tick. These levels use no network clients.

| Test entities | ECS flip median per tick | ECS flip p95 per tick | Server physics per tick |
| ---: | ---: | ---: | ---: |
| 1,024 | 0.002028 ms | 0.003296 ms | 0.079 ms |
| 8,192 | 0.014067 ms | 0.022132 ms | 0.610 ms |
| 32,768 | 0.053986 ms | 0.070523 ms | 2.444 ms |

The table reports medians across three independent runs per level. Each run
includes eight warmup ticks and 128 measured ticks; all 136 ticks are checked.
The isolated flip timing excludes the verification pass and all QuakeC and
physics work. `sv_speeds` reports the separate server physics figure with
QuakeC think callbacks individually instrumented; each run produced only one
or two one-second samples, so those figures are approximate and do not include
the ECS flip pass. This is not a gameplay or rendering frame-time benchmark.

All three levels configured `max_edicts 32770`. ECS component storage is
currently allocated for that capacity and rounded to `max_align_t` per slot,
so the added bool component occupied 524,320 bytes per level, rather than one
byte per active entity. The timings include the ordered-query visit checks.

A separate real-server capacity smoke filled all 1,048,576 edict slots,
including the world and reserved client slot, and successfully freed and
reused edict 1,048,575. A separate high-index trigger test freed and reused
edict 65,536 and confirmed it received a touch. A local PEXT2 client/server
smoke checked high-index baseline, attachment, movement and sound messages.
These tests do not establish that a level with a million active entities is
playable at a particular frame rate.

To reproduce the level test, use a release build with the test hook enabled;
the hook is omitted from normal builds and activated only by the test runner.
A compiler for QuakeC, a compatible id1 QuakeC source tree and local Quake
PAKs are required:

```sh
meson setup /tmp/quiver-ecs-level --buildtype=release \
    -Decs_level_stress=true -Dembedded_maptools=disabled
ninja -C /tmp/quiver-ecs-level vkquake
python3 tests/stress_ecs_quakec.py \
    --binary /tmp/quiver-ecs-level/vkquake --maptool-binary build/vkquake \
    --basedir /path/to/quake --compiler /path/to/fteqcc \
    --quakec-source /path/to/id1-quakec --native-components \
    --counts 1024 8192 32768 --max-edicts 32770
```

The runner creates an isolated temporary game directory and writes a JSON report
and per-level logs there. `ninja -C build ecs_stress_bench` also builds a
standalone synthetic ECS-world benchmark; its measurements omit server and
QuakeC processing and should not be mixed with the level results above.
