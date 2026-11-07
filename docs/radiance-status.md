# Radiance lighting status

The graphics menu's **Radiance GI** preference is also available as
`r_radiance`. Set it to `1` to request real-time lighting, or `0` to use
classic lighting. It defaults to `0` while correctness and reference GPU
performance are still being evaluated. If resources or a required renderer
path are unavailable, the frame falls back to classic lighting;
`radiance_stats` reports the active path and any failure reason. CPU
QuakeC light-at-point behavior remains unchanged.

The frame integration includes camera-centered sparse probes, runtime map
lights and lightstyles, emissive surfaces and dynamic lights, a software BVH
and optional Vulkan ray-query backend, and lighting receivers for world and
models. Runtime lighting need not reproduce a map's baked appearance: some
original source data may be unavailable. The number of dynamic occluders is
bounded; `radiance_stats` reports the total omitted count and separates the
capacity limit, unsupported models and transparent entities. Batched ECS
instances are included in the candidates and their count is reported separately;
instances beyond the shared limit are included in the capacity omission count.
Static, single-frame MD5 models now retain their skinned geometry and can cast
through either tracing backend, including multi-surface models and cutout
materials. Animated MD5 casters remain unsupported. The actual Sponza export
and its material/geometry caching checks are documented in
[the static MD5 caster report](radiance-static-md5-casters.md).
Texture sources in memory-backed embedded packages are reopened through the
engine's file-handle API using offsets relative to the package member. An
isolated ECS1024 run with synchronization validation remained in Radiance and
reported 256 selected of 1,024 batched candidates; this verifies the embedded
map path, not its frame-time target.

`r_radiance_probe_spacing 64` experimentally doubles the linear extent of the
receiver probe field compared with the default `32`, using the same number of
probes at one eighth of the spatial density. Trace intervals grow with spacing.
The saved setting now accepts spacing from `8` to `128` world units, including
fractional values. Positive values outside that range clamp to its endpoints;
invalid or nonpositive values use `32`. Changes apply at the next synchronized
frame boundary without a video restart. The renderer replaces the sparse
mapping and invalidates the old probes while retaining the scene, texture atlas
and GPU allocations. `radiance_stats` reports the raw requested, effective and
active spacing. Smaller spacing gives denser probes over a smaller field;
wider spacing increases coverage and may miss fine details or increase leaks.
The probe-record count stays fixed. Earlier versions silently selected only
`32` or `64` and required video resources to be recreated; a plain `vid_restart`
could be a no-op when no video setting changed.
An isolated RTX 4080 run with synchronization validation exercised live
`32→16→24→64→128→32` changes. CPU metadata and both GPU uniform slots matched
at every step; buffer, descriptor and pipeline handles, allocation capacities
and scene geometry stayed unchanged. This verifies live application and
lifetime safety, not an image-quality or performance improvement.
In two short alternating RTX 4080 runs at 1280×720, quality 1, adding the
batched ECS occluders changed ECS32768 Radiance wall-frame times from
1.32/1.24 ms to 2.38/2.38 ms (about 758–807 to 420 FPS). Whole-frame GPU
medians changed from 0.754/0.738 ms to 0.771/0.773 ms. This was a matched
diagnostic control with the same embedded texture fix and topology path; the
extra CPU/scene work is significant. A bounded batch-update prototype was
slower in these scenes and was removed. Instanced acceleration structures are
an experimental hardware ray-query path now offers broader local-shadow
coverage, as described below.

`r_radiance_batch_shadows` (default 1, saved) uses shared-mesh acceleration
for eligible opaque ECS visual batches. Point, emissive and directional shadows
come from all eligible instances, while indirect/GI hits stay limited to the
ordinary 256 selected occluders because batch instances carry no material data.
Eligibility is per instance: a rotated, scaled, translucent or otherwise
unsupported entity stays with the bounded selection without disabling the rest.
The software backend and allocation failures retain the bounded fallback.
`radiance_stats` reports requested/active status, direct-shadow and GI
coverage, and acceleration allocations. For 32,768 instances the acceleration
allocation reached about 32.8 MB in one RTX 4080 run, and short fixed-budget
whole-frame GPU timings were variable and sometimes slower; use this as a
coverage experiment, not a performance optimization. A paused 32,768-instance
test confirmed that local shadows appear when enabled and disappear again when
disabled. A separate hardware run exercised repeated toggling, a video restart,
and a map change from the 32,768- to 1,024-instance scene. The path has not
been validated for every map or material.
When selected dynamic entities stay in the same order, their existing cache
entries are now matched directly; reorders fall back to the original
first-unclaimed search. In five alternating synthetic CPU measurements,
scene-update medians changed from 29.7 to 21.3 microseconds for 256
candidates and from 182.0 to 172.6 microseconds for 32,768 candidates with
256 selected. These are scene-manager timings, not whole-frame results.
Client-known static entities are considered even when outside the
current potentially visible set, so they can cast shadows into view; moving
entities absent from the server's potentially visible set cannot be recovered
from unavailable client state. Brush emissive surfaces use the BSP face plane
to emit only from the visible side, including when the emitter budget omits a
direct-light proxy. Unoriented model emitters remain two-sided. The user can tune `r_radiance_point_gain`,
`r_radiance_emissive_gain` and `r_radiance_bounce_gain` independently.
The bounce gain has no fixed upper limit for receiver lighting; negative or
non-finite values do not produce negative or non-finite gain. The gain used
inside the recursive probe feedback remains capped at 0.95 to keep repeated
bounces bounded, independently of the receiver gain.
Emissive-source attenuation falls linearly to zero at the source radius, with
distance measured from the closest point on the emitting rectangle. This is
separate from map point-light attenuation and dynamic-light falloff.
The emissive gain applies both to direct-light proxies and to emissive surfaces
that are omitted from the proxy budget and reached by probe rays. Zero disables
both contributions for newly evaluated lighting; previously traced indirect
light can persist while the budgeted refresh catches up. A surface already
represented by a proxy is still excluded from traced emission to avoid counting
it twice. The software-Vulkan brush fixture verifies gain 0 and 2 with
synchronization validation.
At quality 0 the single visibility ray targets that closest point too, so a
nearby visible edge can contribute even when the emitter's center is hidden.
This remains a single-sample approximation with hard shadow boundaries; the
higher-quality area samples can yield different visibility.
Quake's indexed liquid warp textures do not have separate fullbright images.
The material adapter extracts their fullbright palette pixels into emission
masks, and emissive liquid faces can produce the same bounded area-light
proxies as other brush faces. Liquids remain transparent to scene visibility
rays. When an external RGBA replacement has no explicit glow image, the
original Quake indexed BSP or WAD texture supplies its fullbright mask. The
mask and replacement are sampled at the same normalized UVs, including when
their dimensions differ; artistic changes in the replacement may not align
with the original mask. Valve palette liquids and replacements without an
available original indexed source are not inferred to emit. For appearance, all pixels of a lava
warp surface retain Quake's unlit look in Radiance mode; this approximation
also affects any non-fullbright pixels in a mixed lava texture. Water and
slime continue to receive Radiance lighting. Both opaque and translucent lava
use the Radiance pipeline and do not sample baked lighting.
Focused CPU tests cover liquid masks, empty water, source selection, caching,
and the `gl_fullbrights` setting. A read-only smoke test against the installed
PAK found one luminous lava texture in start and none in E1M1/E1M2. The
optional `tests/smoke_radiance_lava.py` builds disposable start-map variants
with other fullbright pixels and point entities removed, then compares a fixed
in-world view with lava emission enabled and disabled. It also checks a 128×128
RGBA replacement against the map's 64×64 indexed lava mask and verifies that
the replacement is actually loaded. A software-Vulkan run found 18,195 neutral
wall pixels receiving red light for that replacement with Khronos validation
enabled and no reported validation errors.
With both opaque lava
and `_lavaalpha 0.5`, an isolated 320×240 software-Vulkan run with synchronization
validation found 6,513 neutral wall and ceiling pixels receiving red light;
the opaque and translucent lava captures also differed in the lava region.
The fullbright texture pixels themselves differ between the two variants and
are excluded from the receiver comparison. This checks a single stock-map
view; it is not a general proof of lighting quality or hardware performance.

Vulkan graphics and radiance compute pipelines use a bounded cache in the
engine's write root. It is reused across video restarts and saved on a clean
exit; vendor, device, driver, cache UUID and checksum are checked before use.
Missing, incompatible or corrupt caches fall back to normal compilation.
In one isolated RTX 4080 run at 640×360 with the driver's shader disk cache
disabled, startup to an active ray-query Radiance frame took 14.842 seconds cold
and 0.660 seconds with the saved application cache. This is one wall-clock
pair, not a median or a frame-time measurement; driver behavior varies. The
optional `tests/smoke_vulkan_pipeline_cache.py` exercises cache loading, video
restart, corrupted-cache rejection and recovery in a disposable game directory.

Map lights default to intensity 300 and support signed `light`, `wait`,
`delay` 0–5, and the linear-light `_falloff` distance. Local minimum lights
(`delay 4`) use line of sight and set a per-channel floor after other direct
and indirect lighting; negative map lights subtract before the floor is
applied. Compiler-style lights honor world and per-light `_anglescale` /
`_anglesense` and explicitly authored spotlight `_softangle`. World `_dist`
scales light distance, while
`_range` scales reconstructed map sources relative to its compiler default.
Runtime values are normalized for this renderer and do not exactly reproduce
ericw's baked output: `_range` is applied to the completed lightmap by the
compiler, inverse falloffs have finite bounds near one lightmap unit, and
separate local-minimum lightstyle layers are not reconstructed. Compiler command
line overrides cannot be recovered when they are absent from the map.
For compiler-style spotlights with no explicit `_softangle`,
`r_radiance_spot_softness` defaults to `0.5`, adding a transition between the
inner half and outer edge of the authored cone to reduce hard discs. This is an
artistic approximation; `0` restores the compiler's hard default cone and `1`
extends the transition to the center. Explicit `_softangle` values, including
zero, take precedence. The outer cone, distance falloff and conservative light
grid are unchanged; settings take effect without a map reload.
Rerelease dynamic lights also honor `_shadowlightstartfadedistance` and
`_shadowlightendfadedistance` from the current camera position, as in the
classic entity-light path. Fading changes light intensity and cascade refresh,
while preserving the source identity and its conservative grid membership.
KEX map and live dynamic sources use their linear radial falloff and linear
cosine-space spotlight falloff; ordinary synthetic and compiler-authored light
curves retain their own behavior.
Before the spotlight-softness control was added, an isolated software-Vulkan
A/B on the installed start map removed only the two `light 4000`, `delay 3`,
`mangle "90 0 0"` lights; both circular spots on the central pillars
disappeared. These sources have infinite distance falloff and an authored
40-degree cone without `_softangle`. That older test used the compiler's hard
default, equivalent to the current `r_radiance_spot_softness 0`; it did not
explicitly set the control, which did not yet exist. The comparison does not
test their appearance in every map or renderer mode.
The map's baked `_bounce` and `_bouncescale` settings are not currently used
to configure the runtime cascade transport, so matching those baked indirect
contributions remains separate work.

`r_radiance_quality` defaults to `0` and selects an interval budget ceiling. The adaptive budget
starts at 2,048 ray intervals, may fall to 256, and has ceilings of 4,096,
16,384 or 32,768 for quality 0, 1 or 2. A ray interval is one direction at
one probe, not a whole probe. A completed-frame timestamp adjusts the budget
against a 2 ms **prepass** target; receiver shading is outside that timer, so
this is not a guarantee of 2 ms total GI cost. `radiance_stats` reports the
current interval budget, updated level and interval count, scene upload bytes
and acceleration-structure builds for the last frame. The tracked memory total
and its input, scene, cascade and acceleration breakdown count Vulkan memory
allocations in both frame slots, including mapped host-visible allocations;
CPU data and driver-internal overhead are excluded. Light-grid build count
is cumulative. Static map ray-query geometry is retained per fenced frame slot;
lightstyle or texture metadata changes alone do not require rebuilding it.
When sparse topology changes, dirty levels merge from far to near before the
bounce trace reads them; the newly traced data is then propagated to receivers.
A GPU readback regression detects stale coarse data during the trace itself,
and a post-merge-only negative control fails it. This adds work on topology
changes: an empty-probe Lavapipe microbenchmark measured 0.608 ms for a dirty
hierarchy versus 0.141 ms for a stable bounded merge. Those figures are not an
RTX measurement or an in-game cell-crossing cost. A short matched RTX 4080
start-map test observed 32 topology revisions in each crossing and teleport
run. Median GPU maxima in approximate event windows were 2.778 versus 2.754 ms
for crossings and 2.772 versus 2.739 ms for teleports, corrected path versus
deliberately incorrect post-merge-only control. These windows include other
rendering work and lag between CPU events and GPU completion, so they do not
isolate merge cost or establish performance on other maps.
An existing light grid is also retained while each changed non-spot light's
new cell bounds fit within its previously indexed bounds. This deliberately
leaves harmless extra candidates for shrinking or moving lights; a light that
reaches a new cell, a changed spotlight or a changed light count rebuilds the
grid. In an isolated four-second RTX 4080 test with a recurring explosion,
the fine-grid rebuild count fell from 6,238/6,561 to 80/80 across two short
alternating runs. The small 14-light test scene showed no repeatable FPS or
GPU-time change; it does not establish a performance gain on other scenes.
Each fenced GPU frame slot also retains its grid buffers while their unique
revision and sizes match, avoiding repeated host copies of unchanged indices.
Unversioned inputs still copy every update, and map or coordinator resets
assign a new identity before publishing a rebuilt grid.
Two short alternating RTX 4080 pairs with the fine grid did not show a
repeatable whole-frame speedup from this copy avoidance alone.
The CPU grid builder uses row-prefix counts to avoid counting every covered
cell individually; a 3,000-case randomized comparison produced identical CSR
offsets and indices to the previous builder. In isolated fine-grid rebuild
microbenchmarks, medians fell from 1.490 to 1.190 ms on start, 0.487 to
0.398 ms on E1M1, and 0.472 to 0.401 ms on E1M2. Unchanged grids were
already cached, so these are rebuild costs, not per-frame savings.
Set `r_radiance_profile 1` to add CPU preparation timings to `radiance_stats`.
The reported per-frame pipeline, scene, lights, upload, sparse and dispatch
times exclude map loading, GPU execution, and unrelated engine work. Set it
back to `0` to remove the profiling overhead.
The atlas upload uses a generation per completed CPU atlas build so unchanged
atlas pixels need not be read back from mapped Vulkan staging memory each
frame. On an RTX 4080 at 1280×720, quality 0 and the UNORM scene path, two
alternating pairs of isolated builds differing only in this fast path measured
55–58 versus 355.5–388.5 FPS on start, and 107.5–113 versus 345–355 FPS on
E1M1. Sampled CPU upload preparation fell from 17.2–17.3 to 0.065–0.066 ms
and from 7.72–8.44 to 0.010–0.015 ms respectively. These are short runs, not
the requested whole-frame GPU acceptance measurement. In those same two fast
runs, the difference between the Radiance and classic per-run median GPU frame
times was approximately 1.93–2.07 ms on start and 2.23–2.25 ms on E1M1;
additional runs and scenes are needed and E1M1 exceeds the 2 ms target here.
Reusing the sparse brick lookup across the directional samples at each receiver
corner improved two short alternating RTX 4080 pairs at 1280×720, quality 0:
the median incremental GPU frame time on start went from 2.072/2.039 ms to
1.853/1.944 ms; on E1M1 it went from 2.197/2.302 ms to 2.161/2.157 ms.
These pairs are not full acceptance testing and E1M1 still misses the target.
With the FP16 scene enabled and the then-default quality 1, two later short RTX 4080
alternating pairs at 1280×720 measured a median whole-frame GPU difference of
2.25 ms on start and 3.92 ms on E1M1 relative to classic UNORM. The separate
prepass timer excluded receiver shading. These measurements miss the 2 ms
acceptance gate; they do not establish performance in other views or maps.
After the cross-frame synchronization fix, two further short alternating
pairs measured 2.27 ms and 3.99 ms respectively; these results do not establish
whether the small difference from the preceding runs is a regression.
With an earlier one-source-per-face version of surface-template reconstruction
and closest-point visibility, two short alternating RTX 4080 pairs at 1280×720, quality 1, produced median
paired whole-frame GPU deltas of 9.42 ms on start and 5.39 ms on E1M1.
Classic/Radiance median GPU times were 0.34/10.81 and 1.17/9.52 ms on start,
and 0.33/6.73 and 0.91/5.29 ms on E1M1. The corresponding median paired
wall-frame deltas were 14.98 and 7.53 ms; classic and Radiance use UNORM and
FP16 targets respectively. These two-second samples used an isolated Xvfb/X11
session with a two-second warmup, and their FPS and timing variance differ
substantially from earlier runs, so they are not a controlled comparison to
the previous implementation. They do not meet the 2 ms acceptance target and
do not identify how much cost comes from the additional surface lights.
In a later default-quality grid comparison, the finer grid reduced candidate
counts at the camera from 48 to 18 on start and from 66 to 45 on E1M1.
Whole-frame GPU medians improved on start (about 2.60–2.67 to 2.08 ms), but
were much closer on E1M1 (about 3.97–4.02 to 3.92–3.93 ms). Camera-cell
counts do not measure the fragments actually shaded. The fine grid remains
opt-in; the short comparison did not verify visual parity or meet the gate.
In another isolated default-quality RTX run, disabling emissive gain reduced
the measured E1M1 whole-frame GPU time from 4.09 to 1.65 ms. This removes
all emissive evaluation, not just shadow rays. A prototype that reused a
proven shadowing triangle across area-light samples reduced E1M1 median time
from 4.03 to 3.60 ms but increased start from 2.58 to 3.85 ms; it was not
retained. These short measurements identify an expensive path, not an
acceptance result.
Another prototype conservatively omitted cells wholly behind one-sided
emitters. It reduced stored grid indices from 27,899 to 27,311 on start and
from 10,465 to 9,628 on E1M1, but E1M1 GPU medians remained about 4.04 ms.
It was removed; the result does not rule out different scene-dependent grid
strategies.
An additional one-pair RTX E1M1 run reported a 2.26 ms median difference and
2,160,768 bytes of acceleration-structure allocations; this short sample is
not a substitute for the full acceptance benchmark.
Short-lived dynamic lights prioritize fine bricks within their influence near
the source's horizontal position and the camera's height. Up to eight bricks
can be expedited at once, and distinct sources can queue separate bricks;
at least one in four completed queued bricks is ordinary work when available.
Previously expedited bricks in the source's former influence are queued for
prompt refresh when it disappears or moves. This uses the existing ray budget
and visibility tests, but short effects and crowded scenes can still expire
before their indirect lighting converges.
Quality 0 also uses one deterministic visibility ray toward the nearest point
on each emissive rectangle. All represented sources still illuminate and cast shadows,
but their penumbrae are harder and less accurate. Quality 1 and 2 use four
visibility samples per rectangle. Changes refresh the affected cascade data.
When bounce gain is zero, world and model receivers and the cascade trace skip
the otherwise unused probe lookup. The setting still removes indirect light;
this shortcut does not change the default gain. Short E4M1 shaft comparisons
were too variable to establish a frame-time improvement from this path.
For diagnostics, `tests/bench_radiance.py` records the selected light grid
and point, emissive and bounce gains, as well as an optional fixed camera pose.

The in-game software path and resource lifecycle have been exercised with
Vulkan synchronization validation. Focused standalone tests cover software
traversal and hardware ray-query shaders, including a moved-triangle
acceleration-structure update on the RTX 4080 with synchronization validation.
An isolated RTX 4080 in-game E1M1 smoke run exercised the ray-query backend
with synchronization validation, 4× MSAA, OIT modes 0/1/2/0 and a real
640-to-800-pixel video restart. It reported no Vulkan validation errors and
kept Radiance active; it does not establish visible transparent-object
coverage or correctness on other maps. The broader quality comparison and
2 ms median incremental whole-frame acceptance measurement remain open. When
supported, the Radiance path now uses an FP16 scene target with an optional
highlight shoulder after transparency and before display effects; it falls
back to UNORM if creation fails. An isolated 4× MSAA menu/HUD comparison
with Vulkan synchronization validation passed on software Vulkan and RTX ray
queries: HUD pixels matched across scene-target toggles, with small menu
differences attributable to cursor animation. A separate two-brush overlap
fixture passed on both backends with HDR, 4× MSAA and synchronization
validation: both layers affected over 27,900 shared pixels in each OIT mode,
and reversing entity order did not alter the capture. The fixture disables
bounce for repeatability; entity order may not be actual draw order. Broader
hardware visual coverage remains open. Dynamic occluder triangles are
cached per selected entity. Interpolation-only changes for unchanged aliases
are limited to eight updates per frame with age-based fairness; cached
triangles remain in use until replacement. Removals, new/reused entities,
brushes and changes to an alias's identity or transform take effect immediately
and may exceed that budget. `radiance_stats` reports cached, updated and
deferred objects. Dynamic triangles share a GPU acceleration structure per
frame slot. When its topology is unchanged, changed geometry refits that
structure and the top-level structure; topology changes and periodic refreshes
rebuild it. This is not yet an independent GPU update per entity. An isolated
software-Vulkan one-shot explosion test observed indirect response before
expiry and no pixels at least four luminance levels above baseline one simulated
second later at the 256-interval budget; smaller residual differences remained.
This is a focused fixture rather than a guarantee for all maps, viewpoints, or
transient sources.

With `r_radiance_highlight 1` (the default), the FP16 scene applies a
hue-preserving highlight shoulder after compositing transparency and before
palette, tint, menu effects and native UI. If the FP16 path is unavailable,
the UNORM fallback applies the earlier per-receiver approximation, so
fullbright additions can still clip and transparent surfaces blend after that
compression. Fullbright and lightmap debug views bypass the adjustment. Set
`r_radiance_highlight 0` to disable it, or `r_radiance_hdr 0` to select the
UNORM target explicitly. This is a floating-point intermediate, not HDR display
output; see `radiance-hdr-design.md` for validation and limitations.
`radiance_stats` reports the actual scene target, the HDR request and format
support, highlight toggle and spotlight softness, so an HDR fallback can be
distinguished from a change in light intensity or shadowing.

Map light entities with `_surface` are treated as templates instead of point
lights at their authored origin. The runtime creates sources at matching brush
faces, preserving style, color, distance settings and `_surface_offset`; an
optional `_surface_spotlight` follows the face normal. Static sources are cached
for the map, while selected moving brush sources follow their transforms.
Directional sun visibility traces beyond the former 8192-unit cutoff, so
distant geometry can block sunlight on large maps.
Fullbright proxy extraction avoids counting the same textured surface twice.
`radiance_stats` reports template, generated static/dynamic and unmatched
counts; resource or source-capacity failure selects the classic fallback.
The runtime reconstructs polygon boundaries from brush triangles and splits
them on 128-unit axis planes using the compiler’s eight-unit margin, placing a
source at each subdivision’s vertex-average center. Degenerate fan triangles
are omitted, so their collinear vertices can affect exact compiler parity.
Moving brush faces are subdivided in world space after transformation, rather
than in original map coordinates. An isolated software-Vulkan E1M1 smoke test
found 9 templates, 206 static sources and no unmatched templates. These results
do not measure the hardware performance target or establish visual parity.
For the subdivided sources, a further two-pair RTX 4080 comparison found that
the finer light grid reduced camera-cell candidates and produced pixel-identical
captures at the two tested spawn views, but variable probe budgets and timings
prevented a reliable overall speedup. The fine grid used substantially more
indices and remains opt-in; other views and animated scenes were not compared.

An isolated E4M1 spawn-view baseline on RTX 4080 at 1280×720, quality 1,
used three alternating four-second samples with five-second warmups and default
gains. The median paired difference in whole-frame GPU medians was 8.38 ms;
classic and Radiance used UNORM and FP16 scene targets respectively. The Xvfb
session showed low classic frame rates and timing variability, so these numbers
are a local diagnostic rather than a representative native-display frame rate.
The sampled Radiance frame had about 557 lights, including 227 reconstructed
surface lights, and 102 light-grid candidates in the camera cell. One surface
template, `tlight03`, has no matching texture in this BSP's texture lump.
Completed
prepass timings excluded receiver shading and fluctuated significantly. This
view does not meet the 2 ms target; other E4M1 viewpoints remain unmeasured.
Captured comparisons and raw samples are in
`build/radiance-e4m1-rt-20261104/`.
At the shaft edge (`--pose 1312 680 688 55 90 0`), a separate short pair
measured 1.15 ms classic versus 7.74 ms Radiance whole-frame GPU median, with
default gains and the same isolated display. A prototype that reused cosine
weights across probe corners measured 8.04 ms in its corresponding Radiance
run and was removed; one pair is insufficient to establish a general regression.
The shaft captures and raw results are in
`build/inspector-build-tmp/cosine-ab/edge-baseline/`.

A later shaft diagnostic used point gain 7, bounce gain 8 and sky fallback
0.15 at the same resolution and quality. With probe filtering at 1, two
alternating classic/Radiance pairs had whole-frame GPU medians of
0.446/9.212 and 1.113/12.475 ms; sampled Radiance prepass medians were
0.489 and 1.123 ms. In a separate run with filtering at 0, the corresponding
medians were 0.469/8.718 and 0.861/12.913 ms, and the prepass medians were
0.670 and 0.067 ms. The filtering settings were not alternated against each
other, and the short runs varied substantially, so they do not establish a
filtering speedup. They do confirm that the reported prepass omits substantial
receiver and other frame work. Raw timings and captures are in
`build/radiance-e4-shaft-followup/` and `build/radiance-e4-shaft-filter0/`.
For a better controlled filter comparison, `tests/bench_radiance.py` now
accepts `--probe-gaussian 0 --compare-probe-gaussian 1`. It alternates the
two Radiance-only settings within each scene session, checks the active
setting and backend, and reports paired deltas as the comparison strength
minus the baseline strength. These deltas compare filter settings, not the
incremental cost of Radiance relative to classic lighting. The harness checks
for other vkQuake processes at startup and around each timed sample; if one is
found, it stops its own game and leaves the report incomplete unless
`--allow-existing` was explicitly requested. These checkpoints cannot detect a
short-lived competing process that starts and exits entirely between checks.

Sky lighting fallback: `r_radiance_sky_fallback` controls a low-energy diffuse
upper hemisphere for Radiance GI on maps with sky portal geometry but no
worldspawn `_sunlight`, `_sunlight2`, or `_sunlight3` intensity key. The default
is `0.15`; `0` disables it and values are clamped to `0..1`. An explicitly
authored zero is respected. It does not affect classic lighting. The color is
an average in linear RGB of the classic foreground/background sky layers, or of
the upper and side faces of the active loaded skybox. It does not infer a sun
position. It is an approximation rather than a directional environment map;
missing or unsupported skybox images are excluded. Fallback light is returned
only at sky-portal hits, never arbitrary ray misses, to avoid illuminating
sealed interiors through incomplete acceleration geometry. Probe updates
converge over time and the result is subject to `r_radiance_bounce_gain`.
`radiance_stats` reports whether sky lighting is authored, texture-derived, or
absent. A runtime sky change clears the old probe history and schedules a
budgeted rebuild. While it converges, receivers use runtime direct light;
old sky radiance cannot continue feeding subsequent bounces. A GPU regression
covers clearing both validity buffers and retry after a discarded recording.

Sky faces remain two-sided: stock maps contain coincident faces with opposite
orientations, so the acceleration structure can return either side first.
Probe visibility uses coarse directional distances, and cascade merging does
not test geometry between neighboring probes, so light can leak through walls.
The receiver tests the stored distance along a representative ray in the
probe-to-receiver angular bin, not an exact ray between those points. A thin
wall can block the actual segment while the representative ray misses it;
the 2×2 base direction grid makes this especially visible. Wider filtering
can draw from more probes on the far side of a wall. An exact segment query
would address that receiver case at additional tracing cost, but would not on
its own fix leakage introduced during cascade merging.

Reduced opaque-world receivers also support `r_radiance_receiver_filter 1`:
eight visibility-tested probe corners per reduced receiver, followed by a
geometry-guided irradiance texture filter and four-tap upsampling. This keeps
the forward renderer. Upsampling uses prepass depth, normals and primitive
identity rather than another probe-visibility scan. Direct light and local
minimums are not Gaussian-blurred. Use `r_radiance_resolution_scale 0.666666667`
(`0.666` also selects two-thirds), `0.5`, `0.333333333`, or `0.25`;
checkerboarding and odd viewport dimensions are
supported. `r_radiance_receiver_filter 0` retains the probe-filtered reference.
The texture filter uses a separable approximation scaled by world-space probe
spacing, with nine interpolated taps per axis. Connected coplanar triangles can
share lighting; intervening visible boundaries and unavailable scheduled
evidence stop the filter. Fragments without usable reduced lighting retain the
requested probe Gaussian.
See [the receiver integration notes](radiance-runtime-integration.md#experimental-reduced-resolution-receivers)
for requirements, geometry rejection, fallback, diagnostics and timings.

On full-resolution and reference receivers,
`r_radiance_probe_gaussian` adjusts experimental receiver-side bounce
interpolation from `0` (the original eight-corner interpolation) through `1`
(a tapered window over up to 27 probes) to `2` (a wider window over up to 125
probes). Intermediate values blend continuously; values outside this range are
clamped. Strengths above `1` can be substantially more expensive and have not
been performance-validated in gameplay. Missing
or occluded probes retain their weight in the normalization to avoid bright
leaks, so incomplete fields can still darken. This setting does not change
probe transport. A headless GPU regression covers uniform fields, a cell
boundary, isolated impulses and absent/occluded probes. It increases receiver
work and has not yet been visually or performance validated; compare frame
timings before enabling it for play.

`r_radiance_gi_distance` sets the camera distance in Quake units at which
indirect receiver lighting fades to zero. It defaults to `0` (disabled); when
enabled, the smooth fade starts at 75% of the chosen distance; positive values
below one unit are treated as one unit. Direct
lights and probe transport are unchanged. Receivers beyond the fade skip their
indirect lookup, but this setting does not reduce cascade update work or extend
the roughly 1024-unit-wide level-zero probe volume around the camera.

An isolated RTX 4080 E4M1 shaft capture at 1280×720 (ray query, quality 1,
point gain 7, emissive gain 1, bounce gain 8, Gaussian receiver blending on)
measured 0.713 ms classic versus 11.649 ms Radiance median whole-frame GPU
time over one three-second pair after a four-second warmup. The Radiance
prepass median was 0.737 ms and excludes receiver shading. These are diagnostic
measurements from an Xvfb session, not a representative gameplay FPS or a
Gaussian on/off comparison; the 2 ms incremental target is not met in this
view. The capture and machine-readable results are in
`build/radiance-final-capture/`.

After the sky-history and clear-barrier changes, a second short Xvfb run at
the same shaft pose and settings used two alternating two-second pairs after
three-second warmups. Classic GPU frame medians were 0.446 and 1.113 ms;
Radiance medians were 9.212 and 12.475 ms, for a 10.064 ms median paired
difference. The three-sample Radiance prepass medians were 0.489 and 1.123 ms
per run and exclude receiver shading. These short, variable runs do not prove
a performance change from the earlier measurement. The 2 ms incremental
target remains unmet. Captures and full results, including wall-clock times,
are in `build/radiance-e4-shaft-followup/`. The benchmark accepts the full
continuous `--probe-gaussian` range `0..2`.
