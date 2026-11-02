# Sky performance and filtering

Clouds use half width and half height by default. Every reduced-resolution pixel
is evaluated each frame; nearest sampling preserves a crisp appearance without
temporal accumulation. A conservative depth test checks each block against all
covered opaque depth samples, including MSAA. Hidden blocks are skipped, clear
blocks reuse the reduced result, and partially occluded blocks use full-resolution
depth-clipped rays. Clouds composite into the multisampled scene when MSAA is on,
so later transparent rendering and the scene resolve preserve their contribution.

The dominant cloud-noise octave blends deterministic, spatially varied samples
to reduce visible repetition. The underlying 3D noise is periodic across texture
boundaries. These changes remain stable in world coordinates: stopped wind does
not introduce frame-to-frame randomness, nearest filtering stays crisp, and
returning to the same camera restores the same pattern after a floating-origin
change.

Lighting stops evaluating a sample's remaining shadow rays when their maximum
possible contribution is negligible. The omitted contribution is bounded by
`8e-4` premultiplied HDR units per color channel per complete ray. Empty-density
and unresolved-noise rejection avoid work without changing the retained density
model.

Reduced-resolution rays also coalesce distant integration intervals. Rays with
64 or fewer original intervals are unchanged; longer rays progressively merge
pairs in their far tail. The longest 80-interval ray keeps its first 32 intervals
and uses 56 density evaluations overall. Each merged interval retains its full
length for opacity integration and uses the corresponding noise footprint.
This changes fine distant cloud detail in exchange for lower cost; merged
intervals can also reach the existing opacity cutoff at a different point within
its 0.005 residual transmittance. This quadrature tradeoff is separate from the
bounded lighting omission above. Full-resolution and depth-boundary fallback
rays retain the original integration, so
`r_clouds_resolution 1` remains the reference quality mode.

| Control | Default | Effect |
| --- | --- | --- |
| `r_clouds_resolution` | `2` | Per-axis resolution divisor: `1` full, `2` half, `4` quarter. |
| `r_clouds_filter` | `0` | `0` uses nearest noise and upscaling; `1` uses linear filtering. |
| `r_atmosphere_lut` | `1` | Cache atmosphere integration in a 256×128 HDR texture; `0` integrates directly. |
| `r_sky_profile` | `0` | Collect completed-frame GPU pass timings and edge fallback coverage. |
| `sky_stats` | command | Print active paths, resolutions, allocation, GPU timings, and fallback coverage. |

The three rendering controls are archived. The profiling switch is temporary.
Atmosphere lookup contents depend on sun direction, not camera movement, sun
intensity, fog, or AgX settings. Sun intensity and display transforms are applied
when the lookup is sampled. Clouds are stored as premultiplied HDR radiance and
opacity; AgX and fog are applied once during composition. Unsupported sampled-depth formats
or sample counts select the full-resolution cloud path. `sky_stats` reports which
path is actually active.

## Measuring performance

Use a release build and the same GPU, viewport, quality settings, and background
workload for both runs. The graphical harness links existing game assets into a
new temporary directory and copies the executable there. Configurations, worlds,
captures, logs, and the executable remain in that directory for inspection.
The player's saved configuration is never loaded or changed. Vsync and the
normal `host_maxfps` limit are disabled for timing; screenshot capture briefly
caps at 60 FPS to let asynchronous capture complete.
If ordinary play still displays exactly 200 FPS, check `host_maxfps`; the saved
cap can hide a faster renderer. Use `host_maxfps 0` and `vid_vsync 0` when making
an uncapped comparison, then restore the preferred limits for normal play.

Before changing the renderer, preserve a baseline:

```sh
python3 tests/run_cloud_horizon_smoke.py --binary build/vkquake \
  --basedir /path/to/quake --baseline --benchmark-only
```

After rebuilding, compare against its printed `summary.json` location:

```sh
python3 tests/run_cloud_horizon_smoke.py --binary build/vkquake \
  --basedir /path/to/quake --benchmark-only \
  --reference /tmp/quiver-cloud-horizon-BASELINE/summary.json --require-speedup
```

Each of four fixed views—horizontal, slightly downward, inside the layer, and
above it—uses three alternating clouds-off/on pairs. Each pair measures two
seconds of normal application frames after terrain queues settle. Summaries use
the median of each pair's mean frame intervals and the median incremental cloud
cost (`on − off`). `--require-speedup` requires at least 50% lower incremental
cost in every view. A clouds-off increase above 5% is reported for investigation;
repeat the measurement before attributing small host-frame differences to a GPU
pass. `--pairs 1..5` and `--sample-seconds` adjust the sampling budget.

Host-frame intervals include presentation, scheduling, and CPU work; they are
not GPU timestamps. Profiling is disabled during these measurements. Separate
`sky_stats` captures briefly enable GPU profiling and retain both parsed values
and raw availability information. GPU diagnostics use completed-frame queries
without waiting for unfinished frames. Unavailable results must not be treated
as zero-cost passes, and profiling/readback overhead must not be included in the
headline frame comparison.
Query results are read only after frame completion and checked using Vulkan's
[query availability rules](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html).

At 1280×720, the current renderer reports 3,932,160 bytes for the two cloud HDR
targets and 262,144 bytes for the atmosphere lookup: 4 MiB of auxiliary image
allocations. Both cloud targets remain allocated while the cascade experiment
is disabled, allowing comparisons without reallocations. The earlier
single-target performance run below used 2.125 MiB before this extra target was
added. These figures exclude the existing 256³ R8 cloud noise texture (16 MiB),
scene attachments, and other renderer resources.

## Measured results

On 2026-10-26, the RTX 4080 with NVIDIA 615.71.09 at 1280×720 passed the
four-view acceptance run with default half-resolution, nearest-filtered clouds
and cached atmosphere. Each view used three alternating two-second off/on
pairs, stopped wind, an uncapped release build, and profiling disabled. Values
below are host-frame milliseconds; incremental values are the median of the
paired differences, so they need not equal the difference between the separately
reported off/on medians.

| View | Original off | Original on | Updated off | Updated on | Original increment | Updated increment | Increment reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Horizontal | 0.436 | 4.176 | 0.438 | 1.883 | 3.742 | 1.439 | 61.55% |
| Slightly downward | 0.447 | 3.760 | 0.428 | 1.798 | 3.314 | 1.371 | 58.62% |
| Inside layer | 0.460 | 3.780 | 0.466 | 1.590 | 3.320 | 1.130 | 65.97% |
| Above layer | 0.459 | 3.760 | 0.445 | 1.737 | 3.301 | 1.280 | 61.21% |

Every view exceeds the 50% incremental-cost target. Clouds-off changes range
from −4.10% to +1.17%; no final view triggered the 5% regression warning. A
separate three-pair recheck of the preserved original executable reproduced its
horizontal/downward incremental costs (3.767/3.308 ms), confirming that earlier
isolated slow samples were not a persistent baseline shift.

The following are **separate GPU timestamp measurements**, collected after the
host timing with profiling enabled. Each row is a rolling mean of 32 completed
frames. Composite time includes full-resolution rays at depth boundaries.

| View | Cloud trace (ms) | Cloud composite (ms) | Atmosphere draw (ms) | Full-resolution fallback pixels |
| --- | ---: | ---: | ---: | ---: |
| Horizontal | 1.396 | 0.266 | 0.0069 | 0.265% |
| Slightly downward | 1.292 | 0.318 | 0.0069 | 0.605% |
| Inside layer | 1.160 | 0.298 | 0.0029 | 0.908% |
| Above layer | 1.214 | 0.415 | 0.0059 | 3.576% |

The atmosphere lookup had already been built, so these steady-state samples
performed no LUT rebuild. Timestamps span `TOP_OF_PIPE` to `BOTTOM_OF_PIPE` and
may include pipeline overlap or queued work. They measure elapsed pass intervals,
not isolated execution costs; do not sum them or equate them with host-frame
increments.

The original binary, configuration, logs, and captures are preserved in
`/tmp/quiver-cloud-horizon-cr0rdva_`; the accepted build and complete measurement
data are in `/tmp/quiver-cloud-horizon-kickazr9/summary.json`. The original-binary
recheck is `/tmp/quiver-cloud-horizon-2m10x3l4/summary.json`. These temporary
artifacts describe this machine's run and are not portable performance guarantees.

## Correctness checks

Run the complete capture and lifecycle suite separately from performance claims:

```sh
python3 tests/run_cloud_horizon_smoke.py --binary build/vkquake \
  --basedir /path/to/quake --lighting-checks
meson setup build-debug --buildtype=debug
ninja -C build-debug
python3 tests/run_cloud_horizon_smoke.py --binary build-debug/vkquake \
  --basedir /path/to/quake --lighting-checks --validation
```

The suite covers all six resolution/filter combinations, direct/cached
atmosphere, sun-driven LUT invalidation, stationary pixels, wind, rotation,
travel through the layer, foreground terrain, dense clouds, fog, large
coordinates, empty worlds, BSP sky/roof intersections, viewmodels, transparency,
MSAA off/4×, odd window dimensions, render scales 1/2, partial viewports, and
video restarts. Additional captures cross a stochastic noise cell and the
canonical noise-phase wrap, then require an exact stationary image on return to
the original camera. Stopped-wind images and opaque foreground comparisons are
checked numerically. Captures still need visual review for crisp edges, sky
portals, sun/AgX highlights, and motion; a changed pixel alone cannot prove
correct appearance. Vulkan validation errors fail the validation run.
Validation runs omit host-frame timing so layer overhead is never presented as
renderer performance. The validation run requires the Khronos validation layer
and a debug engine build: release builds do not enable the layer with
`-validation`. For a locally unpacked SDK/layer, set `VK_LAYER_PATH` and its
library search path before launching. Optional synchronization checks can be
enabled with `VK_LAYER_VALIDATE_SYNC=1` on current Khronos validation layers.

The RTX 4080 run exercises the explicitly requested full-resolution path. Its
supported depth formats and sample counts do not exercise automatic fallback
on unsupported hardware; the capability guards were inspected separately.

The completed validation evidence for this change is:

- Release and debug builds succeeded. Both complete Meson regression suites
  passed all 12 tests; the full 256³ periodic-noise regression also passed after
  its final update in both builds.
- All 46 shader variants compiled and passed SPIR-V validation. The six affected
  cloud fragment variants were compiled and validated again after the final
  integration change.
- The complete debug suite passed four views and 76 checks with Khronos and
  synchronization validation, including all quality modes, depth occlusion,
  MSAA, odd-size/scale/partial-viewport restarts, and BSP roof/water/viewmodel
  captures. Its results are in
  `/tmp/quiver-cloud-horizon-4ovjsxoy/summary.json`.
- The final far-interval integration change then passed a focused debug run of
  four views and 23 checks, covering wind, rotation, through-layer travel,
  stochastic phase boundaries at full/half resolution, bright light, sunset,
  and AgX. Its results are in
  `/tmp/quiver-cloud-horizon-fcl9d9sl/summary.json`. Both validation runs reported
  zero Vulkan validation errors.

Stationary and camera-roundtrip images were pixel-identical. The full matrix
also found zero opaque-foreground cloud leakage, and visually reviewed BSP
captures included transparent water, the riverbed, bridge, sky, and viewmodel
together. Final full/half phase captures showed no straight tile seam. Comparing
the final coalesced horizontal capture with the preceding exact-integration
capture preserved the broad cloud forms and horizon, while changing fine distant
detail as expected (mean RGB difference 2.02 on a 0–255 scale over the whole
image). The complete lifecycle matrix predates this last shader-math change;
the focused run verifies its visuals using the same validated render-resource
and depth paths.

`--visual-only` keeps the four fixed views, stationary/dense clouds, wind,
travel, and stochastic phase captures without repeating the full quality and
compatibility matrix. `--benchmark-only`
keeps the four paired measurements and GPU diagnostics. `--compatibility-only`
repeats depth, viewport, restart, and map checks when investigating a lifecycle
failure. These focused modes do not substitute for the complete correctness run.

## Distance-cascade experiment

The optional distance-cascade path renders near and far cloud intervals into
separate targets. These experimental controls are temporary and are not saved:

| Control | Default | Effect |
| --- | ---: | --- |
| `r_clouds_cascades` | `0` | Enable the two-target experiment with `1`; `0` keeps the single-pass path. |
| `r_clouds_near_resolution` | `2` | Near interval resolution divisor, `2` or `4`. |
| `r_clouds_far_resolution` | `4` | Far interval resolution divisor, `2` or `4`. |
| `r_clouds_split` | `4096` | Near/far split distance along the camera ray, in world units. |
| `r_clouds_band` | `0` | Diagnostic interval selection: `0` both, `1` near only, `2` far only. |

`sky_stats` appends the applied split, target resolutions, and separate near/far
GPU pass intervals. The `2/2` layout measures the overhead of partitioning a
matched-resolution workload. The `2/4` and `4/2` layouts test which interval
benefits most from reducing pixel count. Isolated near-only/far-only workloads
are not an additive decomposition of the original front-to-back ray: removing
near clouds can remove early termination that previously avoided far work.
The experiment requires the reduced-resolution path and sampled-depth support;
`r_clouds_resolution 1` keeps the full-resolution compatibility path. A second
HDR target is allocated even while cascades are disabled, allowing comparisons
without reallocating resources between cases. At 1280×720 the cloud targets
therefore use 3.75 MiB instead of the single-target measurement above; the
atmosphere lookup remains 0.25 MiB.

Run a short matrix against a freshly preserved single-pass baseline:

```sh
python3 tests/run_cloud_cascade_benchmark.py --binary build/vkquake \
  --basedir /path/to/quake --reference /tmp/BASELINE/summary.json \
  --diagnostics --splits 4096 8192 16384 --layouts 2/2 2/4 4/2
```

The matrix uses the same four cameras as the horizon harness. It measures one
off/on pair per case by default and repeats the single-pass control before and
after each view's matrix to detect drift. Every case preserves a stopped-wind
capture and separate GPU statistics. Use `--pairs 3 --sample-seconds 2` for a
repeat measurement of selected layouts/splits, and visually review split
boundaries before judging a lower-resolution layout. `--validation --lifecycle`
skips host timings and adds odd-size, MSAA, render-scale, partial-viewport, and
video-restart checks for each requested layout. Add `--motion` to capture wind,
rotation, travel through the layer, split-radius approaches, and noise-phase
wraps for each layout, including an exact stopped-wind camera roundtrip.
`--linear` checks both filtering modes; `--scene-checks` adds foreground terrain,
clouds in front of ground, and BSP sky/roof/water/viewmodel captures for one
selected case. To repeat selected combinations without a full cross product,
use `--candidates 2/2@4096 2/4@4096 2/2@16384 4/2@16384`.

### Cascade measurements

The same RTX 4080/1280×720 setup was measured again before adding cascades.
The preserved executable's four captures were pixel-identical to the preceding
accepted version. A one-pair sweep then covered all nine layout/split combinations
and isolated near/far intervals in all four views. The following candidate
comparison uses three two-second off/on pairs per case. Times are **clouds-on
host-frame milliseconds**, with profiling disabled:

| View | Single pass | 2/2 at 4096 | 2/4 at 4096 | 2/2 at 16384 | 4/2 at 16384 | 4/2 total-frame reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Horizontal | 1.821 | 2.087 | 1.432 | 1.944 | 1.349 | 25.94% |
| Slightly downward | 1.751 | 2.172 | 1.450 | 1.880 | 1.353 | 22.71% |
| Inside layer | 1.553 | 1.896 | 1.901 | 1.631 | 1.142 | 26.48% |
| Above layer | 1.678 | 2.022 | 1.504 | 1.821 | 1.193 | 28.91% |

The single-pass controls repeated at each view's end changed clouds-on time by
less than 1%. Clouds-off controls were less stable: horizontal changed from
0.447 to 0.421 ms and downward from 0.428 to 0.452 ms, triggering the 5% drift
warning. The raw report retains every off/on pair and incremental difference;
the table emphasizes the more stable total frame times instead of attributing
small baseline fluctuations to cloud passes. Single-pass candidate captures
remained pixel-identical to the preserved executable in all four views.

Matched `2/2` cascades are slower than one target: splitting alone does not save
work. `2/4` at 4096 helps three views but costs more inside the layer. `4/2` at
16384 is the fastest repeated candidate in every view, at the cost of coarser
near-cloud pixels. It is an optional quality/performance tradeoff, and
`r_clouds_cascades` remains disabled by default.

These separately profiled **single-target diagnostic GPU trace intervals** show
where the work lies. Both isolated bands use half resolution (divisor 2), so
this table compares distance ranges at matching quality; it does not use the
best candidate's quarter-resolution near band. The values come from the
exploratory split sweep and are rolling means of 32 completed frames; the normal
front-to-back ray cannot be reconstructed by summing independently rendered
near/far costs.

| View | Split | Near-only trace (ms) | Far-only trace (ms) |
| --- | ---: | ---: | ---: |
| Horizontal | 4096 | 0.101 | 1.323 |
| Horizontal | 8192 | 0.679 | 0.624 |
| Horizontal | 16384 | 1.105 | 0.382 |
| Slightly downward | 4096 | 0.132 | 1.167 |
| Slightly downward | 8192 | 0.626 | 0.530 |
| Slightly downward | 16384 | 0.818 | 0.361 |
| Inside layer | 4096 | 0.907 | 0.194 |
| Inside layer | 8192 | 0.934 | 0.104 |
| Inside layer | 16384 | 0.902 | 0.070 |
| Above layer | 4096 | 0.222 | 0.799 |
| Above layer | 8192 | 0.838 | 0.310 |
| Above layer | 16384 | 0.963 | 0.102 |

At a 4096-unit split, distant clouds dominate the horizontal/downward views,
while the inside-cloud view is already dominated by nearby density. Increasing
the split transfers most work into the near interval. This explains why one
fixed near/far resolution choice does not suit every view.

The repeated candidates' separate GPU diagnostics are below. These are elapsed
pass intervals with the timestamp caveats described above. Fallback coverage
counts the union of near/far mixed-depth pixels, which receive one complete
full-resolution ray during composition.

| View | Layout/split | Near trace (ms) | Far trace (ms) | Total trace (ms) | Composite (ms) | Fallback pixels |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Horizontal | 2/4 at 4096 | 0.102 | 0.382 | 0.485 | 0.745 | 0.725% |
| Horizontal | 4/2 at 16384 | 0.341 | 0.285 | 0.626 | 0.504 | 0.806% |
| Slightly downward | 2/4 at 4096 | 0.133 | 0.334 | 0.467 | 0.775 | 1.141% |
| Slightly downward | 4/2 at 16384 | 0.314 | 0.267 | 0.582 | 0.567 | 1.184% |
| Inside layer | 2/4 at 4096 | 1.075 | 0.165 | 1.239 | 0.533 | 2.153% |
| Inside layer | 4/2 at 16384 | 0.477 | 0.066 | 0.543 | 0.346 | 1.093% |
| Above layer | 2/4 at 4096 | 0.224 | 0.284 | 0.507 | 0.893 | 4.536% |
| Above layer | 4/2 at 16384 | 0.400 | 0.118 | 0.519 | 0.467 | 3.726% |

For comparison, the repeated horizontal single-pass trace/composite intervals
were 1.346/0.265 ms, while matched `2/2` at 4096 used 1.456/0.461 ms. Reducing
far resolution to `2/4` saves substantial tracing, but composition rises to
0.745 ms and boundary fallback coverage increases from 0.265% to 0.725%.
The end-to-end frame improvement is therefore smaller than the tracing reduction.

The preserved pre-experiment binary, 57 source/build/test/document files and
reference captures are in `/tmp/quiver-cloud-cascade-before-684ce8pw`.
The fresh baseline is `/tmp/quiver-cloud-horizon-5lc6o789/summary.json`, the
complete exploratory sweep is `/tmp/quiver-cloud-cascades-r2cntlmz/summary.json`,
and the three-pair comparison is
`/tmp/quiver-cloud-cascades-z7r378fh/summary.json`.

Final debug validation enabled both Khronos and synchronization checks. All
three layouts passed 27 viewport/restore states (odd dimensions, MSAA off/4×,
scales 1/2, partial viewports, and video restarts), six near-only/far-only
cascade cases, and three nearest/linear comparisons. Results are in
`/tmp/quiver-cloud-cascades-dcllj0ju/summary.json`.

The best `4/2` at 16384 layout then passed 12 fixed-view mode captures, two
cascade-only cases, 17 motion checks, and eight scene checks. These include
wind, rotation, through-layer travel, approaches across the split radius,
full-resolution phase references, foreground terrain, ground obscured by clouds,
and BSP sky/roof/water/viewmodel layering. Camera-return images were identical;
opaque foreground leakage was zero. The water-alpha comparison changed visible
pixels by 14.68/255 on average, confirming that the capture exercised transparent
water. Results and reviewed captures are in
`/tmp/quiver-cloud-cascades-wv4rkrp4/summary.json`.
Both final validation runs reported zero Vulkan errors and omitted host-frame
timings. Visual review found no obvious split seam in the captured movement
or BSP scenes; quarter-resolution near-cloud detail remains visibly coarser.

## Wrapped-world cloud continuity

Cloud sampling uses the complete canonical camera position independently of the
renderer’s local origin. Planet worlds now fit a whole number of the cloud
field’s ten-UV periods around their wrapped circumference. This prevents a jump
when movement crosses either planet seam. The default planet changes the
effective cloud scale by less than 0.005%; `r_clouds_scale` remains the requested
value. Integer chunk reduction preserves phase even beyond double-precision
integer coordinates, and local origin shifts do not restart the pattern.

Clouds trace only the portion of the curved layer inside the camera's near and
far planes and in front of opaque scene depth. This applies to full, reduced,
cascaded and MSAA paths. `gl_farclip` is measured along the camera's forward
axis, as for world triangles. Atmosphere remains a background at far depth;
the generated dome cannot cover nearer terrain or transparent entities.
`r_fastsky 1` replaces the sky with its flat color and skips the volume. In a
streamed world `r_drawworld 0` hides both sky and terrain. `r_showtris` draws
the dome's mesh over the shaded image; the volume itself has no triangle mesh.

`tests/run_cloud_coordinates_smoke.py` freezes wind and raises the camera and
cloud layer together above the terrain, then captures local rebases, both
planet seams, and two periodic-phase controls. It covers full/half resolution,
nearest/linear filtering and cascades, and requires identical stationary frames
as well as a visible difference when clouds are disabled. Example:

```sh
python3 tests/run_cloud_coordinates_smoke.py --binary build/vkquake \
  --basedir /path/to/quake
```

The regression in `cloud_noise_test` also checks wrapped aliases, negative
coordinates, several planet extents/scales, zero scale and integer chunks beyond
2^53. These are coordinate/correctness checks, not performance measurements.

On 2026-10-27, the isolated 960×540 capture comparison measured average
absolute RGB differences across a two-unit seam crossing. Default half-resolution
nearest clouds changed from 44.515/59.241 levels (X/Y seam) to 0.880/0.908;
an ordinary two-unit origin rebase measured 0.936. All tested seam differences
were below one RGB level after the fix, versus 44.175–59.782 before it.
The complete stationary images remained identical. Both sides of the default
X seam were visually inspected and retained the same cloud formations.

Preserved baseline: `/tmp/quiver-cloud-coordinates-m93qatxc/summary.json`.
Updated release captures: `/tmp/quiver-cloud-coordinates-y33tn4lx/summary.json`.
Debug captures with active Vulkan synchronization validation:
`/tmp/quiver-cloud-coordinates-13teg7kp/summary.json` (zero validation errors).
The final harness requires every two-/four-unit control to remain below three
RGB levels; the earlier release report recorded a looser limit for four-unit
controls, but its largest actual difference was 1.389.
