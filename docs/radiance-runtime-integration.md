# Radiance renderer integration

`Quake/radiance_engine.c` owns frame-slot preparation, source snapshots,
sparse probe scheduling and receiver publication. A separate per-slot command
buffer records the lighting inputs, acceleration structures and cascades; it
is submitted before scene draws only after successful preparation. Scene and
input uploads, sparse reservations and cascade work are committed on
submission and discarded on failure. The renderer waits for each slot's fence
before reusing resources, and shuts the resources down after device idle on
video restart or shutdown. Failure to prepare a frame selects classic
lighting for that frame.

The collector owns the material IDs and atlas shared by geometry and runtime
lighting. Camera, visible receivers, dynamic occluders and instance-batch
bounds drive sparse requests; source and geometry changes schedule refreshes.
Software traversal and ray queries use the same scene and light data. Static
map and dynamic geometry have separate ray-query acceleration structures;
material-only changes update scene data without rebuilding geometry. Sparse
updates at a stable fine level merge only their changed intervals; coarser
updates and changed topology propagate as required to finer levels. Topology
and update bookkeeping must be committed only when submitted work succeeds.

The sparse grids use centered, brick-aligned origins with a small movement
deadband, so crossing a world axis alone does not invalidate the hierarchy.
The two coarsest grids are split into smaller bricks, retaining overlapping
probe records when they scroll; newly exposed bricks still need to converge.
Teleports beyond a grid's coverage still invalidate that level's history.

`radiance_stats` distinguishes completed prepass GPU time from whole-frame
time. The adaptive interval budget uses that prepass measurement; it does not
include the per-pixel direct-light and shadow work done by receiver shaders.
Tracked memory is a lower bound, not a complete driver allocation total.

`r_radiance_soft_shadows 1` requests an experimental native ray-sampled
contact-hardening pass for opaque reduced receivers in texture-filter mode.
The default is `0`, which creates no optional pipeline and records no extra
dispatch. One eligible positive additive source per sampled receiver uses four
visibility rays in total, including the existing center ray, with at most three
additional rays regardless of the number of candidate lights. Full-resolution,
reference-filter, model and unsupported receiver paths keep their existing
shadows. Probe transport keeps its existing lighting and sampling. Optional
pipeline failure leaves the hard receiver available; stats report the pass
active only after it is actually recorded for the selected world path.

`r_radiance_shadow_radius` sets the finite point/spot source radius in world
units (default `2`, clamped to `0..64`).
`r_radiance_shadow_sun_angle` sets the directional source's angular **radius**
in degrees (default `0.5`, clamped to `0..15`). These controls are archived.
Rectangle emitters use their existing source geometry. Nonfinite values
disable the corresponding setting. `radiance_stats` reports
the requested and active soft-shadow mode, sanitized source sizes, and the
maximum extra-ray count. The controls update live without resetting probe
transport. Receiver upsampling adds no shadow rays.

## Experimental reduced-resolution receivers

The [focused receiver report](radiance-receiver-filter.md) contains controls,
current RTX 4080 measurements and validation artifacts for the texture path.

`r_radiance_receiver_filter 1` (the default) selects the texture-filtered receiver path.
It keeps the forward renderer and existing depth/normal/identity prepass.
Indirect lighting uses the original eight visibility-tested probe corners per
reduced receiver, followed by horizontal and vertical geometry-guided filtering
and a copy into the resolved indirect target. Upsampling
uses four compatible texture samples without repeating probe visibility
checks. Direct light and local minimums remain reduced-resolution and are not
Gaussian-blurred. `r_radiance_receiver_filter 0` retains the original
probe-filtered path for comparison. Full-resolution receivers, models,
transparency, and unsupported frames retain their existing lighting path.

In texture mode, `r_radiance_probe_gaussian` controls a separable approximation
whose radius follows world-space probe spacing: strengths through `1` have
support of 1.5 probe spacings, and strengths above `1` have 2.5. Each axis uses
nine logical taps, interpolating compatible texture endpoints, with a maximum
radius of 448 reduced texels. Supports stop at the nearest visible boundary;
world-space integration weights keep clipped supports continuous. Strength `0` skips Gaussian smoothing; checkerboard
holes still receive spatial reconstruction. The original probe Gaussian remains
the full-resolution reference. These filters intentionally produce different
approximations. Irradiance is filtered before material
color, bounce gain, distance fade, direct/minimum composition, and highlights.
Valid black samples remain data; unavailable probe evidence stays invalid.
The horizontal pass uses only original scheduled checker samples. The vertical
pass filters that result. Classification and filtering can cross continuous
coplanar triangle boundaries; the forward fragment must still match its exact
prepass identity. Depth-derived positions, normals and symmetric plane tests
reject incompatible surfaces. Cached edge masks reject intervening geometry
boundaries, gaps and originally missing scheduled evidence. Local upsampling
retains its two-probe-spacing support limit.
These guides cannot prove visibility of an occluder absent from the receiver
prepass, so the approximation can still leak light.
If no upsample tap survives, the fragment evaluates the requested probe Gaussian.
This preserves smoothing on surfaces that cannot use the reduced result.

`radiance_stats` reports the requested and actually submitted receiver filter
alongside scale and checkerboarding. Monotonic full/reduced recorded-frame
counts also expose a one-frame fallback between state snapshots; these are
CPU recording counts, not fenced GPU sample counts. Receiver capture waits for
the prepared Radiance publication before testing eligibility.
`r_radiance_receiver_diagnostics 1` enables
a separate optional coverage-estimation pass for active texture receivers and
prints completed-frame block counts and estimated GI/direct reuse and fallback.
Probe, full-resolution and unsupported frames print no coverage rows.
Estimates cover opaque
world prepass pixels, not model/transparency execution. Original producer
validity is retained separately when the raw indirect target becomes filter
scratch; zero bounce produces no valid GI samples. Leave diagnostics disabled for timing;
the disabled path records no diagnostic GPU work.

`gpu_stage_time` optionally reports receiver classification (including the
receiver geometry prepass), lighting, and postfilter/reconstruction timings in addition to world
and entity ranges. Results are collected from fenced slots without waiting
for queries. Stage ranges can overlap and should not be summed as a frame
breakdown. Receiver sample counts cover submitted receiver frames only and can
differ from world/entity counts; a skipped postfilter records an empty range.
Whole-frame and stage commands also report p95.

Set `r_radiance_resolution_scale` to `0.666666667` (or `0.666`), `0.5`,
`0.333333333`, or `0.25` to evaluate direct illumination, shadows, and diffuse
bounce at two-thirds, half, one-third, or one-quarter width and height.
Two-thirds uses divisor `1.5` and an exact `ceil(2N/3)` grid: a 641×361 viewport
has 428×241 receiver cells. Its raster footprints alternate between one and
two pixels per axis, and reconstruction aligns with those nonuniform anchors.
The other scales use 2×2, 3×3, or 4×4 blocks. Classification checks every
present pixel, including partial blocks at odd viewport edges. Set
`r_radiance_checkerboard 1` as well to evaluate direct light and bounce on
opposite cells of the receiver grid and reconstruct their missing values
spatially. The result is interpolated onto world surfaces, with full-resolution evaluation at
incompatible geometry or invalid samples. Shadow edges on the same surface
are interpolated and may be softer or shifted. No temporal history is used.

This experimental path currently requires `r_indirect 0` (the engine default), single-sample
rendering (`vid_fsaa 1`, which restarts video when changed), and a supported opaque
world draw. Models, transparency, MSAA, and unsupported frames retain the
existing path. `radiance_stats` reports both requested and actually active
receiver modes; a requested mode can fall back. Set
`r_radiance_resolution_scale 1` for full resolution. Other positive values are
rounded to the nearest supported per-axis scale and invalid or nonpositive
values select full resolution. The scale and
checkerboard controls are archived. Existing configs and scripts can still use
`r_radiance_half_resolution 1` or `0` as a compatibility alias for scale `0.5`
or `1`. Reading the alias reports `0` for full, `1` for half, `2` for one-third,
`3` for one-quarter, and `4` for two-thirds resolution. Legacy writes of `1`
select half resolution, and writes of `0` select full resolution.
Distinct mirror values preserve legacy `0`/`1` writes after changing scale.
`radiance_stats` reports the actual recorded scale and numeric divisor,
including `1.5` for two-thirds and `1` for full-resolution fallback. Geometry
that cannot reuse receiver samples still requires fragment lighting, so performance depends on the view. The older
probe filter also performs conservative per-fragment visibility tests.

The following measurements describe earlier versions of the probe-filtered
reference path, before the texture filter was added. On an RTX 4080 at
1280×720, a short E4M1 spawn-view test with ray queries,
quality 0, Gaussian strength 1 and CPU-recorded world draws measured whole
frame GPU means of 7.61 ms (full), 3.10 ms (half) and 10.90 ms (half plus
checkerboard). Each figure is from one one-second sample after a one-second
warmup, so it is a diagnostic, not a general performance claim. The expensive
per-fragment probe-visibility checks remain a known checkerboard bottleneck.
Raw captures and measurement settings are in `build/receiver-bench-full-03`,
`build/receiver-bench-half-03` and `build/receiver-bench-checker-03`.
At the E4M1 shaft pose `(1312, 680, 688; pitch 55, yaw 90)`, two short
samples per mode gave paired median GPU frame deltas of 10.37 ms (full),
6.09 ms (half) and 6.06 ms (checkerboard); consecutive half and checkerboard
samples ranged from about 4.3 to 8.8 ms. Treat these as preliminary: the
scene and timing vary, and the results do not establish a speedup at other
positions or native display resolution. The corresponding runs are in
`build/receiver-bench-shaft-{full,half,checker}-01`.

The receiver reconstruction now rejects incomplete neighborhoods before
performing probe-visibility checks, and reconstructs direct and local-minimum
light together while preserving separate validity. In an isolated E4M1 shaft
prototype, two short 1280×720 checkerboard/Gaussian GPU samples improved from
4.550–4.553 ms to 4.315–4.334 ms. These samples do not establish a net
checkerboard speedup over full-resolution shading. A Vulkan shader test checks
the optimized reconstruction against the original, including independently
invalid direct and minimum samples. Run it with `QUIVER_TEST_VULKAN=1 meson
test -C build 'Radiance checkerboard reconstruction equivalence GPU'`.

A further shared-visibility prototype evaluates the receiver-side probe and
its visibility data once for up to four neighboring samples. On an RTX 4080,
an isolated randomized GPU comparison matched the existing individual tests
for 3,000 cases, one through four neighbors, filter strengths 0, 1 and 2,
and both merged and raw-distance paths. A simplified synthetic benchmark
favored the prototype, but it does not model real probe storage or rejection
patterns. The renderer now uses a batched visibility check with a conservative
fast path for fully visible neighborhoods. Non-finite, missing, occluded, and
ambiguous cases still use the individual visibility checks. Further close-wall
and moving-scene measurements are needed to assess register pressure and
establish whether checkerboarding helps gameplay.

A separate direction-bin prototype produced 3.906–3.964 ms baseline and
3.565–3.796 ms candidate whole-frame GPU means in short E4M1 shaft runs at
1280×720, checkerboard and Gaussian strength 1. The run was interrupted before
the full alternating comparison and the close-wall case, so these numbers are
preliminary. The receiver now uses the direction-bin fast path, with the
original calculation near bin boundaries, and reuses the directional basis
across filter taps. A sparse-brick address fast path covers the common layout;
other layouts use the general lookup.

On an RTX 4080 at the E1M1 doorway `(607, 143, 40; pitch 3, yaw 91)`,
two alternating 3-second checkerboard samples at 1280×720 with Gaussian
strength 2 measured 5.862–5.966 ms whole-frame GPU time, versus
6.627–7.421 ms with the preserved original executable. At 1720×1370 and
strength 1, candidate samples ranged from 5.039 to 6.466 ms and original
samples from 5.569 to 5.716 ms; the spread is too large to establish a
reliable improvement for that setting. Disabling Gaussian in the latter runs
measured roughly 3.0–3.9 ms. These are whole-frame GPU times and do not
isolate the filter pass. Captures and exact settings are in
`build/gaussian-{wide,default}-{baseline2,candidate3}-e1m1-20261105/` and
`build/gaussian-default-candidate5-e1m1-20261105/`.

The subsequent sparse-brick lookup in the batched visibility test was measured
against `/tmp/rc-gaussian-before-fast` on the RTX 4080 (driver 615.71.09).
Four alternating pairs used ray queries, quality 0, checkerboarding, direct
world draws, a two-second warmup and three-second samples. The camera and
simulation were held steady with `host_timescale 0.000001`. These are paired
changes in completed whole-frame GPU medians, not isolated filter timings:

| View | Size | Gaussian strength | Paired change (ms) | Paired change (%) |
| --- | --- | --- | --- | --- |
| E1M1 doorway, matched animation phase | 1280×720 | 2 | -0.0740 | -1.28 |
| E1M1 doorway | 1720×1370 | 1 | +0.6731 | +14.32 |
| E4M1 shaft | 1280×720 | 1 | +0.0052 | +0.15 |

The larger viewport varied substantially in both binaries (roughly 4.2–6.2 ms),
and the other paired changes were small. The lookup's performance benefit is
still unproven; the larger-view result also warrants caution about regressions.
Binary hashes, individual samples, GPU telemetry, camera settings, captures and
the harness are in `build/gaussian-sparse-fast-20261105/`. Both binaries stayed
loaded and the unmeasured process was suspended. Validation was disabled for
timing. The original 1280×720 doorway runs froze different animated-light
phases and are excluded from the comparison; their JSON records identify the
capture mismatch. The corrected wide-kernel run holds time from map startup,
warms each engine for five seconds before checking capture parity, then uses
the same two-second per-sample warmup and three-second samples. Its initial
captures differ by less than 0.00002 byte values per channel on average, with
no pixels differing by more than eight. Those results and the corrected
harness are in `build/gaussian-sparse-fixed-phase-20261105/`.

The retained follow-up replaces flattened tap decoding in the batched
visibility check with nested X/Y/Z loops, preserving the original X-fastest
order and all early rejection rules. This removes the per-tap integer division
and remainder operations. Four alternating RTX 4080 pairs, with time held
from map startup and capture parity checked before timing, measured:

| View | Size | Gaussian strength | Baseline median (ms) | Nested median (ms) | Paired change (%) |
| --- | --- | --- | --- | --- | --- |
| E1M1 doorway | 1280×720 | 2 | 5.5601 | 4.8267 | -13.19 |
| E4M1 shaft | 1280×720 | 1 | 3.5501 | 3.3790 | -5.77 |

These are whole-frame GPU medians from short fixed-view tests, with the
existing sparse lookup present in both binaries, measured before the grazing
compiler-light correction described below. They establish a local
improvement for these workloads, not a general gameplay speedup. Initial
doorway captures matched exactly; the shaft's mean channel difference was
0.0019 byte values and fewer than 0.007% of pixels differed by more than eight.
The settings, hashes, samples, telemetry and captures are in
`build/gaussian-nested-fixed-phase-20261105/`.
An intermediate constant-divisor branch decoder was rejected: it measured
about 7.9% slower in the shaft, and its corrected wide-doorway result was only
about 2.1% faster. Its measurements remain in
`build/gaussian-constant-decoder-20261105/` and
`build/gaussian-decoder-fixed-phase-20261105/`; mismatched animation captures
are marked invalid. The visibility regression now exercises 64 distinct probe
addresses and positions, removing evidence one cell at a time across all
filter strengths and neighbor counts, in addition to the randomized cases.

The receiver toggle, restart and map-change smoke passed NVIDIA ray-query
synchronization validation; a separate NVIDIA run also checked indirect and
4× MSAA fallback. The standard `smoke_radiance_receiver.py` now includes those
fallback transitions and passed with software Vulkan synchronization
validation. These checks cover the exercised scenes and configurations only.

The extended divisor path subsequently passed both software-traversal and
ray-query RTX 4080 smoke runs at 641×361 with explicit Khronos synchronization
validation. Every scale/checkerboard combination, legacy alias transition,
restart, map change, and indirect/MSAA fallback passed, with zero validation
errors through teardown. Five focused NVIDIA tests also passed, including
100,000 randomized sparse-address comparisons. The startup run exposed a
pre-existing MSAA/OIT unused-attachment layout-transition hazard, reproduced
with the preserved baseline binary. The frame barrier now uses broader scope
only for passes with such transitions; ordinary passes retain their original
scope. Three frame synchronization/failure tests cover that correction.
Final captures, logs, shader checks and the release hash are in
`build/radiance-receiver-scales-20261105/`.

The sky-adjacent stippling on `start` at `(544, 288, 32; pitch -15, yaw 90)`
with `r_lightmap 1` came from the compiler-light grazing-angle rejection.
The map's authored sunlight remains active when point, emissive and bounce
gains are zero. Its exactly grazing light receives compiler angle-scaled
illumination, while small negative derivative-normal errors previously
rejected the light entirely. Removing only `_sunlight` from a disposable
entity override removed the pattern identically on both traversal backends.
The fix accepts a cosine neighborhood of `1e-3` only for compiler lights and
clamps accepted negative cosines to zero before angle scaling. Local-source
early rejection uses the same distance-scaled threshold; ordinary Lambert
lights retain their strict backface rejection. GPU fixtures exercise both
sides of the numerical neighborhood, real backfaces, and non-compiler lights.
At the reported 1280×720 pose, the isolated lintel region changed from standard
deviation 13.54 and mean adjacent-row difference 5.634 to zero for both values.
The original point/emissive/bounce gains also produce a clean capture.
The 1720×1370 hardware view is clean as well, with zero standard deviation in
a corresponding lintel crop. Before, sunlight-zero, patched, original-gain and
larger-viewport captures, settings and the diagnostic script are preserved in
`build/radiance-sky-grazing-20261105/`.

Remaining acceptance work includes hardware and software in-game comparisons,
animated and moving geometry, lightstyles, transparent and emissive surfaces,
map/video changes, teleports and failure cases, and synchronization validation
on the reference device. Benchmark stock and ECS scenes with isolated,
alternating classic/GI runs, measuring whole-frame and lighting-specific GPU
times separately. The feature stays opt-in until correctness and the requested
2 ms median incremental target are demonstrated on the reference GPU. See
`radiance-status.md` for current limitations.
