# Reduced-resolution Radiance texture filtering

The forward renderer evaluates eight visibility-tested probe corners per reduced
receiver. Horizontal and vertical geometry-guided passes smooth indirect
irradiance over a window scaled by world-space probe spacing, followed by a
copy into the resolved target. Four-tap upsampling uses prepass depth-derived
positions, normals, primitive identity and plane/distance checks; it does not repeat probe
visibility. Direct lighting and local minimums are reconstructed separately.
Shared receiver images are allocated at two-thirds size; smaller active grids
use the corresponding portion of those images.
Receiver capture waits for Radiance frame publication before testing eligibility;
a regression test covers the previously possible prepare/reset/capture race.

## Controls

```text
r_indirect 0
vid_fsaa 1
r_radiance 1
r_radiance_quality 0
r_radiance_receiver_filter 1
r_radiance_resolution_scale 0.25
r_radiance_probe_gaussian 1
r_radiance_checkerboard 0
radiance_stats
```

`r_radiance_receiver_filter 1` is the default for reduced receivers. Set `0`
for the original probe-filtered reference; set resolution scale
`1` for the original full-resolution path. Scales `0.666` (canonicalized to
`0.666666667`), `0.5`, `0.333333333`, and `0.25` select reciprocal scales
1.5, 2, 3 and 4, including partial blocks at viewport edges.
The old `r_radiance_half_resolution` control remains a compatibility alias.
`radiance_stats` reports what was actually submitted, including fallback.

Two-thirds rendering uses a 1280×720 receiver grid at a 1920×1080 viewport.
Odd dimensions round up: 641×361 uses 428×241 receivers. Classification partitions
each axis into alternating one- and two-pixel source blocks; all source pixels
belong to exactly one block. Upsampling follows the actual representative
positions rather than assuming equally spaced source anchors. Checkerboarding
uses receiver-grid parity, and the world-scaled Gaussian retains its footprint.
`radiance_stats` reports the active scale as approximately `0.666667` and its
reciprocal divisor as `1.5`. Legacy half-resolution writes of `0` or `1` still
select full or half resolution; reading its mirror yields `4` at two-thirds.

The validated two-thirds release is preserved as
`aa8122c6a31954e57518c4f8cb00001e070f09c94fcb9969f2230f2d059db105`.
It passed 19 CPU renderer/runtime checks, 21 Python harness tests, eight focused
Lavapipe/NVIDIA fixtures, 64 live Gaussian/scale/checker states, and 26 live
diagnostic/mode-switch checks. Both tracing backends passed SDK synchronization
validation through teardown with zero errors. Two-thirds diagnostics count
103,148 blocks at 641×361; unsupported rendering keeps the request while
reporting full-resolution fallback. Exact commands, frozen executable, source
hashes and verification are in
[`radiance-receiver-two-thirds-20261105`](../build/radiance-receiver-two-thirds-20261105/README.md).
To allow hot scale changes without another allocation, all nine shared reduced
images now reserve four-ninths of full-frame pixels, versus one-quarter before.
This increases their storage by about 78%; full-size prepass allocations are
unchanged. The new scale has not been performance-benchmarked.

Texture-mode Gaussian strength `0` skips smoothing. Positive strengths use nine
logical taps per axis, with support of 1.5 probe spacings through strength `1`
and 2.5 above `1`, capped at 448 reduced texels per axis. Compatible endpoint
interpolation avoids integer tap jumps. The horizontal pass draws from original
scheduled checker samples; the vertical pass filters its result. Failed
scheduled lookups stay unknown; black samples stay valid. Filtering precedes
material color, bounce gain, distance fade and direct/minimum composition.
Zero bounce skips the indirect lookup and filter. No temporal history is used.

Each axis clips its negative and positive supports to the connected visible
surface, redistributes four taps per side plus the centre, and weights them by
their world-space integration intervals. This prevents taps entering a wall
from producing bands, or a collapsed support from acquiring multiple full-weight
copies of its boundary sample. Manual interpolation uses at most eighteen RGB
endpoint fetches per axis. Cached geometry and edge masks use 16,512 bytes of
shared memory per 128-lane workgroup, below Vulkan's 32 KiB minimum. Normals
use octahedral SNORM16 encoding in an integer cache; reserved codes distinguish
invalid geometry from originally unavailable scheduled evidence. Interpolation
endpoints already lie inside the nearest-barrier segment, so the filter does
not repeat a path scan for every tap.

This is an approximation, not an equivalent reordering of the probe Gaussian.
Continuous coplanar triangles can share lighting. Geometry guides and cached
intervening reduced-grid edge masks reject visible boundaries,
but cannot establish hidden occlusion. Same-surface lighting and shadow edges can become coarser or stepped, soften,
or shift. If upsampling finds no usable sample, the fragment evaluates the
requested probe Gaussian. Models, transparency, multisampling and unsupported frames keep their
existing path. The experimental reduced path requires CPU world submission
(`r_indirect 0`) and single-sample rendering.

## Coplanar surface correction

The earlier texture path had three interacting faults: exact triangle identity
split continuous walls into islands, its 3×3/5×5 pixel window covered much less
world space than the probe Gaussian, and rejected fragments ignored the requested
Gaussian. Moving brush entities retained the original fragment path, explaining
their different response. The corrected path permits coplanar triangle reuse,
scales texture filtering by probe spacing, and preserves the requested Gaussian
on true fragment fallback. Exact forward/prepass identity still validates the
drawn fragment; upsampling does not repeat probe visibility.

Matched 1920×1080 E1M1 images use quality 0, coarse grid, single sampling,
checkerboarding off, highlight compression off, and held simulation. Captures,
frozen executables, source hashes, per-window submission guards and subsequent
validation are retained in
[`build/receiver-gaussian-surface-20261105/`](../build/receiver-gaussian-surface-20261105/).
The original `f8055b5b` build is retained as the visual baseline. These changes
restore the reduced Gaussian response; they do not make screen-space filtering
identical to the original 3D probe filter or improve the cascade transport itself.

The Gaussian-correction executable measured below is preserved as `17196baa`, SHA-256
`17196baa27bee7355858aab76f4584c213544bc90da87bf48a2a9a418e9b072e`.
The engine now defaults `r_indirect` to `0`; isolated startup checks verify the
default without setting the cvar in the boot script. Quality remains `0` by
default. Highlight compression remains a separate control; it is disabled in
these image comparisons because it compresses bright values rather than adding
lighting.

The final build passed 12 CPU receiver tests and both tracing backends' 24-state
Gaussian sweeps at 641×361: strengths `0`, `0.5`, `1.5`, and `2`, all three
divisors, and checkerboarding off/on. Both also passed aliases, unsupported
fallback, video restart and map change. The additional live-switch/diagnostic
test passed 26 checks, including zero bounce and restoration. No Vulkan
synchronization or validation errors occurred through teardown. These functional
runs shared the GPU with independent functional captures and supply no performance claims.
[`packed128-final-validation/verified-checks.json`](../build/receiver-gaussian-surface-20261105/packed128-final-validation/verified-checks.json)
contains the verified counts. Focused production-helper GPU fixtures pass on
Lavapipe and NVIDIA, including a nonconstant collapsed-support regression that
fails when world-space integration weights are removed.

The final visual comparison includes a brighter doorway case with point gain
`7` and bounce gain `8`. Previously the half/quarter Gaussian-2 floor response
was nearly absent (mean absolute GS2-minus-GS0 change of 0.012/0.029 byte values);
it is now 0.857/0.866. The full-resolution response is 3.069, so these results
establish restored smoothing, not equivalence. In the spawn floor region,
error against full Gaussian-2 falls about 40%; in the upper wall region it
increases about 11–12% despite smoother gradients. The introduced horizontal
wall bands are absent in the final captures. Exact regions, all controls and
the remaining differences are in
[`final-gaussian-comparison.json`](../build/receiver-gaussian-surface-20261105/final-gaussian-comparison.json).
The subsequent cache optimization preserves the visual correction: spawn
Gaussian-2 changes are at most one byte value; bright doorway changes have
99th percentile zero, with 31 half-scale pixels changing by more than eight.
Full Gaussian-2 controls remain bit-exact. The separate
[`optimized-final-image-audit.json`](../build/receiver-gaussian-surface-20261105/optimized-final-image-audit.json)
records all differences and the first full Gaussian-0 control's small UI overlay
exception.

Separate full-resolution Gaussian-2 source isolation retains the large sharp
lighting polygons with bounce gain `0`. Those residual shapes belong to
direct/minimum/source shading in this view; the indirect Gaussian does not
control them. This test does not identify which direct source creates each
boundary. Matched baseline and zero-bounce images, exact settings and active-path
guards are in
[`source-isolation/results.json`](../build/receiver-gaussian-surface-20261105/source-isolation/results.json).

## RTX 4080 measurements of the Gaussian-correction build

The final `17196baa` build uses the same world-scaled filter with a compact
integer geometry cache, 128-lane groups and no redundant endpoint path scans.
The initial quality correction's half-resolution Gaussian-1 regression was
real: its paired GPU time was 24.68% above full resolution. Stage profiling
identified the filter as the main cost. Cache packing/group sizing reduced
that instrumented range from 1.260 to 0.475 ms before removing the redundant
scans. The final paired measurements below include both optimizations.

At 1920×1080, the fixed E1M1 spawn view uses ray queries, quality 0, coarse grid,
point/emissive gains 1, bounce gain 8, sky fallback 0.15, lightmap-only display,
highlight compression off, single sampling and no checkerboarding. Full and
reduced modes all use `r_indirect 0`. Each Gaussian cohort has five rotating
paired runs, one-second per-mode warmups, initial ten-second pipeline-family
warmups and two-second samples. Simulation is held and its actual clock checked.

Times are the median of five run medians; FPS is the median measured engine
frame-count/wall-clock result. Savings and confidence intervals are paired run
effects, so they need not equal ratios of the displayed summary medians.

| Gaussian | Full GPU ms / FPS | Half GPU ms / FPS | Quarter GPU ms / FPS | Half GPU time saved (95% CI) | Quarter GPU time saved (95% CI) |
|---|---:|---:|---:|---:|---:|
| 1 | 2.8205 / 338.9 | 2.6556 / 362.4 | 2.0032 / 466.2 | 7.25% (4.24–8.17%) | 29.12% (26.24–31.37%) |
| 2 | 6.1075 / 162.6 | 4.0295 / 239.3 | 3.0248 / 316.5 | 34.32% (26.82–41.32%) | 46.28% (42.24–50.62%) |

All 30 measurement windows passed actual submission guards: 14,465 reduced and
5,294 full recorded submissions, zero wrong-mode submissions, and maximum held
simulation drift 2.02 microseconds. No other Quake process ran during timing.
Validation, stage queries and receiver diagnostics were disabled; timestamp
samples were not truncated. Paired median p95 and wall-clock changes also
improved. These are short fixed-view results, not gameplay-wide guarantees;
one-third resolution, checkerboarding and Gaussian zero were not rebenchmarked
on this filter. The subsequent two-thirds scale extension has not been
benchmarked; these numbers describe the preserved `17196baa` executable.
Complete run means, medians, p95s, order, state checks,
paired deltas and bootstrap intervals are in
[`timing-optimized-final-summary.json`](../build/receiver-gaussian-surface-20261105/timing-optimized-final-summary.json).

## Historical RTX 4080 measurements

The following measurements describe the earlier 3×3/5×5 texture filter. They
do not establish the performance of the subsequent world-scaled surface filter.

All eight cohorts are complete.

All 320 completed measurement windows passed the recorded-path guard: 187,467 reduced-frame selections contain 0 full-frame fallbacks. These CPU recording counts include command boundaries and differ from fenced GPU sample counts.

The table contains whole-frame GPU times: the median of five run medians.
Reduced ranges cover checkerboarding off/on. Savings are median paired changes
against the production full-resolution reference (`r_indirect 1`), not ratios of
the displayed summary medians. The additional full-resolution reference with
CPU world submission isolates the receiver change. Both references, run means, p95s, all pair deltas and bootstrap intervals are
preserved in the [machine-readable report](../build/receiver-filter-acceptance-q0-20261105/paired-bootstrap-summary.json).

| View | Size | Gaussian | Full ms | Half ms | Third ms | Quarter ms | GPU time saved |
|---|---:|---:|---:|---:|---:|---:|---:|
| doorway | 1280×720 | 1 | 2.030 | 1.648–1.715 | 1.558–1.578 | 1.516–1.558 | 13.1–24.8% |
| doorway | 1280×720 | 2 | 3.616 | 1.957–1.993 | 1.752–1.793 | 1.723–1.744 | 44.9–52.6% |
| doorway | 1720×1370 | 1 | 4.093 | 2.697–2.813 | 2.369–2.477 | 2.222–2.372 | 31.7–45.9% |
| doorway | 1720×1370 | 2 | 7.824 | 3.209–3.386 | 2.672–2.772 | 2.532–2.632 | 56.7–68.2% |
| shaft | 1280×720 | 1 | 3.331 | 2.346–2.409 | 2.218–2.268 | 2.200–2.291 | 28.6–34.3% |
| shaft | 1280×720 | 2 | 5.240 | 2.604–2.687 | 2.349–2.401 | 2.268–2.431 | 50.5–56.7% |
| shaft | 1720×1370 | 1 | 8.022 | 5.439–5.632 | 4.688–4.837 | 4.387–4.537 | 29.6–44.9% |
| shaft | 1720×1370 | 2 | 12.660 | 5.463–5.576 | 4.928–4.933 | 4.590–4.708 | 56.6–63.7% |

Measured frame rates below use the engine frame counter and wall clock, rather
than inverting GPU timestamps. Each is the median FPS of five runs; reduced
ranges cover checkerboarding off/on. All are uncapped fixed-camera measurements.

| View | Size | Gaussian | Full FPS | Half FPS | Third FPS | Quarter FPS |
|---|---:|---:|---:|---:|---:|---:|
| doorway | 1280×720 | 1 | 468 | 541–561 | 580–590 | 588–604 |
| doorway | 1280×720 | 2 | 256 | 467–477 | 503–525 | 526–538 |
| doorway | 1720×1370 | 1 | 236 | 341–358 | 387–406 | 405–427 |
| doorway | 1720×1370 | 2 | 125 | 286–296 | 348–360 | 366–379 |
| shaft | 1280×720 | 1 | 288 | 392–403 | 419–426 | 419–434 |
| shaft | 1280×720 | 2 | 184 | 356–370 | 400–406 | 387–423 |
| shaft | 1720×1370 | 1 | 121 | 173–178 | 200–205 | 211–220 |
| shaft | 1720×1370 | 2 | 77 | 174–178 | 196–198 | 204–209 |

Each cohort uses a frozen release executable, fixed camera, quality 0, coarse
grid, gains 1, sky fallback 0.15, ray queries, one-second mode warmups and
two-second samples. Each pipeline family also receives an initial ten-second
warmup. Simulation advances at `host_timescale 0.000001`, verified through the
actual client clock, to hold animation/lightstyles without the pause-frame cap.
Active camera, sources, backend, scale, filter and disabled diagnostics are
checked around measurements. Monotonic recorded-frame counters additionally
require zero full-frame fallback throughout each reduced sample, including
frames that endpoint snapshots alone could miss. Confidence intervals resample whole paired runs
10,000 times with seed 0. Intervals are per comparison. The paired order rotates;
five rows cover part of the eight-row balancing cycle. Short fixed views do not
establish gameplay-wide FPS.

The benchmark binary SHA-256 is `6583f927031814dc58c39810179832b2be16e49d15153e46e320e0f690876f3b` (NVIDIA driver 615.71.09). Coverage, GPU stage
queries and validation are disabled for these authoritative timings.
The [matrix manifest](../build/receiver-filter-acceptance-q0-20261105/matrix.json)
contains all settings, commands and references; its adjacent directory holds
the frozen executable, harness source snapshots and captures.

## Historical no-blur controls

The separate five-pair, quality-0 controls at 1280×720 show that reduced
receivers are not universally faster with `r_radiance_probe_gaussian 0`.
Half scale is 11.4–12.9% slower than production full in the doorway and start
views; half plus checkerboarding is about 18.6% slower. Quarter variants are
near parity (their paired confidence intervals include zero). With blur off,
full resolution avoids receiver prepass and upsampling overhead.

Compared with the old reduced probe path, texture mode improves the doorway
settings by 8.0–14.2% and the start quarter settings by 7.8–9.2%; the start half
settings are near parity. These controls are separate from the 48 Gaussian-1/2
primary settings. Exact pair effects and intervals are in the
[no-blur report](../build/receiver-filter-zero-controls-q0-20261105/paired-bootstrap-summary.json).

## Historical GPU diagnostics

The [stage/coverage report](../build/receiver-filter-stage-diagnostics-q0-20261105/stage-summary.json)
is diagnostic, with stage queries disabled during the authoritative matrix and
coverage disabled during the stage timings. Stage ranges overlap and are not
additive. At the 720p shaft/Gaussian-2 pose, instrumented whole-frame medians
were 4.644 ms full, 7.088 ms legacy reduced, and 2.503 ms texture reduced;
the new postfilter range was 0.041 ms. Estimated GI reuse was 91.0% there.
The start/Gaussian-1 case estimated 53.0% GI and 75.8% direct reuse.

The no-blur doorway diagnostic measured 1.473 ms full, 1.711 ms half and
1.492 ms quarter. Although world time fell from 1.077 ms to about 0.842 ms,
half added 0.090 ms receiver classification and 0.342 ms receiver shading;
quarter added about 0.109 ms each. No postfilter ran in these non-checker cases.
This explains why receiver overhead can outweigh the no-blur world savings.

## Earlier visual tradeoff

In the quality-0 doorway still at 1280×720 and Gaussian strength 1,
quarter/checker adds visible stepped lighting and shadow edges on the upper
beam and pit rim. Half resolution stays closer to full. Material textures are
still evaluated at full resolution. Quarter/full RGB error averages 0.0955/255,
with 0.293% of pixels differing by more than 8/255 in the compared region;
local edge artifacts remain visible despite the small average error.
No broad leaks, halos or renewed sky stippling were apparent in these stills.
The guarded Gaussian-0 control still shows additional stepped boundaries even
with checkerboarding off and no postfilter. That isolates a fast upsampling
tradeoff beyond shared resolution reduction and Gaussian smoothing: it accepts
partial compatible footprints and omits the old probe-visibility rechecks.
These stills establish fixed-view quality only; geometry guides can miss hidden
occluders.

## Earlier validation and sky diagnosis

[Validation artifacts](../build/receiver-texture-filter-validation-20261105/README.md)
include CPU lifecycle/failure tests, GPU texture-filter and eight-probe lookup
fixtures, and RTX 4080 in-game synchronization validation for both traversal
backends, both Gaussian strengths, all scales/checkerboard modes, alias writes,
video restarts, map changes and unsupported fallback at the odd 641×361 viewport.
The final two-backend test also exercises live filter changes, completed-frame
coverage publication, zero-bounce skip and restoration with diagnostics enabled.
All 26 transitions and eight settled mode-counter intervals passed with zero
validation errors through teardown.

`r_radiance_receiver_diagnostics 1` adds an optional GPU pass with completed-frame
block counts and estimated reuse/fallback for opaque prepass pixels. It runs
only for active texture receivers. The GI estimate describes potential reuse,
even when zero bounce means no actual fragment GI work. Leave it disabled for
timing. `gpu_stage_time` separately measures receiver geometry/classification,
shading and postfilter/reconstruction; stage ranges overlap and are not additive.

The exact sky pose also passed both backends on the fast quarter-resolution,
checkerboard, Gaussian-strength-2 path. With point, emissive and bounce gains zero,
the original lintel crop has mean 41, standard deviation 0 and row delta 0;
original-gain captures are saved alongside the [sky summary](../build/radiance-fast-sky-grazing-20261105/summary.json).

The original sky-adjacent stippling was traced to authored sunlight at a grazing
angle. Compiler lights now accept and clamp a cosine neighborhood of `1e-3`;
ordinary lights retain their original backface rejection. The original pose,
source isolation and before/after captures are described in the
[integration notes](radiance-runtime-integration.md).

## Earlier delivered build and spawn capture

After timing, the receiver-filter default was promoted from 0 to 1 and the
quality default was set from 1 to 0 at the user's request. Rendering logic did
not change after the frozen timings.
The delivered release SHA-256 is `f8055b5bbce637e1f782ff4c28e8b82a4d7bb69fe1719615da48bb914f64f2d1`. Its fresh-start
smoke verified texture filtering and quality 0 without explicitly setting either
cvar.
The [normal start-spawn capture](../build/radiance-start-spawn-delivered-q0-default-20261105/start-spawn-quarter.png)
is 1280×720 with 320×180 receiver lighting, quality 0, Gaussian strength 1,
checkerboarding off, and gains 1. The actual spawn is `(544, 288, 32; 0, 90, 0)`.
A single uncapped two-second sample after a ten-second warmup measured
390.5 FPS and 2.4349 ms median GPU time; this is a
separate capture diagnostic, not a paired speedup result.
