# Quiver handoff to Claude

Prepared 2026-11-06, after the native-resolution Sponza mesh-shader comparison.

## Introduction

You are continuing work on **Quiver**, a customized Vulkan vkQuake engine in `/home/tremor/Desktop/Quiver`. It includes experimental Radiance Cascades lighting, software BVH and hardware ray-query backends, an ECS, an integrated map editor, and compiled static-model rendering and collision. The user cares about smooth, convincing lighting and practical frame rates on an **RTX 4080**, including a large Sponza **MD5** model. They want real measurements and screenshots, with existing functionality preserved.

The latest work implemented a Gaussian over the combined direct lighting from **every light**, alongside the existing indirect/probe Gaussian and bounded contact-hardening shadow samples. The next discussion concerned using the experimental mesh/task shader meshlet pipeline to improve performance. **That combination is not implemented yet.** The current mesh/task pipelines explicitly fall back when Radiance is active; meshlets also operate only on BSP world geometry, not Sponza's MD5 geometry. Do not mistake a requested cvar for an active rendering path.

The latest user request was to write this handoff. The technical directions below describe suggested continuation work, not features already implemented.

## Preserve this working state

- Checkout: `/home/tremor/Desktop/Quiver`, branch `master`, HEAD `7d2acbfa` (`Map editor, BSPX file format, Radiance GI, ECS`). The working tree contains substantial **uncommitted follow-up changes**, especially the current all-light filter. Inspect `git status` and `git diff`; do not reset or replace them with HEAD.
- Published executable: `/home/tremor/Desktop/Quiver/build/vkquake`.
- Tested SHA-256: `2f16e0fa894138dcebb8b79e08e89eec36912d4267b9de2f92eabc17facf381c`.
- Durable frozen copy: `/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/vkquake`.
- Additional frozen copy: `/tmp/rc-all-light-filter-2f16e0fa`. Prefer the durable copy if temporary files disappear.
- Release/build/backup provenance: [release.json](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/release.json).
- Development build: `/home/tremor/Desktop/Quiver/build-soft-shadow-prototype`. Build experiments there or in another isolated build; preserve the published executable until a replacement passes checks.
- No benchmark or capture processes owned by this work remain running. User map/model assets and the main executable were unchanged by the latest benchmark.

The user prefers action, concise updates, and few repetitive confirmation requests. They previously authorized closing Quake while AFK and repeatedly provided clean benchmark windows. Our actual recent harnesses used isolated game directories and aborted if another game appeared; they did not signal user games. Preserve unsaved editor work and distinguish private benchmark processes from a user session.

## What already works

### Receiver lighting and reconstruction

`r_radiance_resolution_scale` supports **1, 2/3, 1/2, 1/3 and 1/4**; `0.666` selects two thirds. Classification, dispatch, reconstruction, sampling, odd viewports and checkerboarding were updated. The legacy `r_radiance_half_resolution` remains a compatibility alias. Current receiver backing accommodates full-grid reuse; do not assume only the old half-size allocation from early notes.

The fast producer samples eight probes and then smooths the resulting texture. Reconstruction uses geometry-guided cached lighting without repeating scene visibility queries. Unsupported fragment fallbacks can still use the expensive probe neighborhood, so reduced resolution does not eliminate all full-fragment lighting work.

`r_radiance_deferred 1` extends shared receiver lighting to eligible opaque world and MDL/MD3/MD5 model geometry, including full resolution. This is a **hybrid renderer**, not fully deferred material shading: depth, normals and exact draw/primitive IDs guide a compute lighting cache; ordinary fragments still draw materials, fullbrights and fog and can fall back to native lighting. Blended surfaces, viewmodels and unsupported paths remain forward shaded. Sponza has 22 receiver-eligible surfaces/227,326 triangles, plus four blended surfaces/34,940 triangles.

Camera/viewport/divisor state is captured immutably for receiver reconstruction, avoiding a race with next-frame camera setup. Live probe spacing now accepts 8–128 world units, including fractions, and applies transactionally at a synchronized frame boundary. Smaller spacing increases density over a smaller field; the probe count stays fixed. Invalid/nonpositive spacing uses 32.

### Three different shadow/filter mechanisms

1. **Contact-hardening samples:** `r_radiance_soft_shadows 1` enables an optional receiver postpass that selects one eligible dominant positive light, reuses its original hard sample, and adds at most three outer visibility rays. Other lights retain their native visibility samples. This is finite-area ray sampling, **not shadow-map PCSS with a per-light blocker search**.
2. **All-light Gaussian:** `r_radiance_shadow_filter` smooths the complete signed direct-light sum and the local-minimum field **separately**. Every point, spot, emissive and directional contribution participates. There is no source-ranking or source-ID barrier. It also smooths attenuation/color gradients, so it is a surface-lighting approximation rather than a physical penumbra calculation for every light.
3. **Indirect Gaussian:** `r_radiance_probe_gaussian` independently smooths the indirect lighting texture. It can run together with both mechanisms above.

The all-light filter uses existing Raw/Resolved Indirect/Direct/Minimum images and horizontal/vertical/copy dispatches. It adds no visibility rays or render targets. The master soft-shadow setting, texture receiver mode, positive filter strength and positive radius are required. Gaussian filtering can run even if the optional extra-ray pipeline is unavailable.

Strength is 0–2. The independent world radius is 0–128, default 8; zero disables the filter. Kernel support is 1.5 radii at strength ≤1 and 2.5 radii above 1, capped at 448 receiver texels per axis. Geometry and unknown-data barriers restrict propagation. Valid black is data; missing data must stay invalid. GI/direct/minimum validity barriers are independent.

The axis cache includes exact draw/primitive IDs. Matching nonzero native triangle IDs bypass the inappropriate plane test caused by smooth shading normals, while finite/normal-angle checks remain. Cross-triangle filtering retains strict plane checks. Shared memory totals **24,960 bytes**. Keep descriptor layouts and all six ping-pong image transitions correct even when only one field is filtered.

### Other completed work to preserve

- `r_indirect` and `r_radiance_quality` source defaults are 0. Deferred reuse remains opt-in. Saved user preferences may differ from source defaults.
- Static single-frame MD5 geometry, including all Sponza surfaces and cutouts, casts through the tracing backends. Animated MD5 casters remain unsupported.
- Projectile firing previously caused costly Sponza acceleration rebuilds. Static geometry is now retained as an immutable base with a small dynamic/projectile suffix, and decoded material atlas data is reused. Do not reintroduce full static rebuilds when a nail appears.
- Editor-placed static models compile into **BSPX `Q_STATIC_MODELS` version 1**, on existing BSP29/BSP2 containers. No invented next BSP version was needed. MDL/MD3/MD5 frame-zero geometry, transforms, skin, shadows, and player/projectile mesh collision are supported.
- The editor has light RGB inspector controls, undo/save/compile support, and textured model previews in 3D/orthographic views.
- The original start-map sky/lintel stippling was isolated to grazing-angle direct sunlight and fixed with a compiler-light cosine tolerance of `1e-3`, retained consistently in hard and query-free lighting. Both tracing backends passed the reported camera regression, including quarter-resolution/checkerboard/probe-Gaussian-2 captures. Preserve [the grazing-sun smoke test](/home/tremor/Desktop/Quiver/tests/smoke_radiance_sky_grazing.py) and [its regression evidence](/home/tremor/Desktop/Quiver/build/radiance-fast-sky-grazing-20261105/summary.json).
- ECS batch shadows are a separate experimental feature. Its local point/emissive coverage can exceed the ordinary 256-occluder selection; GI/sun coverage remains bounded. The striking black/yellow ECS stress-test ceiling was reproduced as actual checker-cube geometry, including with Radiance off; it was not established as an ECS transform bug.

## Current Sponza reproduction

Game directory: `/home/tremor/Desktop/quiver_ripple`.

- Source: `/home/tremor/Desktop/quiver_ripple/maps/the_sponza.map`. **This is QSECTOR despite the `.map` extension.**
- Model: `/home/tremor/Desktop/quiver_ripple/progs/sponza.md5mesh`, 26 surfaces, 262,266 triangles, scale 5, origin `-16 0 16`, one joint/frame. This is not MD3.
- Current model SHA-256: `10272c70eb54877b46cc4067f5dde2e09b6e59f6a1fb93870b9fa03e4ad8d46f`. Older fixtures used a different export; do not silently substitute them.
- Compiled outputs: `/home/tremor/Desktop/quiver_ripple/maps/the_sponza.bsp` and `.lit`. The compiled scene includes 1,004 BSP triangles plus the model.

The actual editor workflow was `editor_open the_sponza`, then **`editor_play`**. The latter runs QBSP/VIS/LIGHT and plays the compiled map; there is no `editor_compile` command. The result was published as `the_sponza.bsp/.lit`, so `map the_sponza` loads it. [Compile evidence](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/sponza-from-editor/results.json).

Use these known-good `setpos` arguments:

| View | Arguments: x y z pitch yaw roll |
| --- | --- |
| Courtyard | `-16 -300 80 -5 90 0` |
| Behind curtains | `-200 -240 64 20 90 0` |
| Open balcony-height overlook | `50 -220 224 15 145 0` |
| E1M1 point-shadow hallway | `480 -128 88 0 90 0` |

The balcony position is a free-flying photo camera on the courtyard side of the edge. **Do not reuse the old x=250 balcony camera:** it is obstructed/behind the wall. x=100 has a foreground pillar. Inspect the actual image before claiming a screenshot is good.

Representative capture settings, not a request to overwrite the user's saved configuration:

```text
r_indirect 0
r_radiance 1
r_radiance_backend 2
r_radiance_quality 0
r_radiance_grid 1
r_radiance_probe_spacing 59
r_radiance_resolution_scale 1
r_radiance_receiver_filter 1
r_radiance_deferred 1
r_radiance_probe_gaussian 1
r_radiance_bounce_gain 25
r_radiance_point_gain 1
r_radiance_emissive_gain 1
r_radiance_soft_shadows 1
r_radiance_shadow_radius 5
r_radiance_shadow_sun_angle 0.5
r_radiance_shadow_filter 1
r_radiance_shadow_filter_radius 8
r_radiance_highlight 0
r_lightmap 0
r_ssao 0
```

Bounce gain 25 was an explicit user request to exaggerate the screenshots. The saved user config had bounce gain 10 when checked; the private captures did not persist 25. `r_lightmap 1` is useful for isolating lighting, while 0 shows materials. Highlight compression was off in the photos because the user dislikes its brightness reduction.

Corrected balcony image: [GI25 overlook](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/sponza-corrected-balcony2f16-sdk/balcony-open-overlook-GI25.png).

## Measurements and what they actually establish

**Latest native Sponza pilot:** RTX 4080, NVIDIA 615.71.09, actual 3440×1440, fixed balcony camera, full receiver scale, quality 0, GI gain 25, both Gaussians, soft source radius 5, direct-filter radius 8. Three rotating rounds with two-second windows after warmup:

| Actual path | Median GPU ms | Median measured FPS |
| --- | ---: | ---: |
| Filtered Radiance reference | 24.3437 | 41.25 |
| Radiance off, stock indirect BSP | 0.2467 | 2971.91 |
| Radiance off, mesh shaders | 0.2565 | 2870.44 |
| Radiance off, mesh + task shaders | 0.2578 | 2871.60 |

Only the last three rows are matched lighting comparisons. Mesh/task GPU costs increased **4.13%/4.33%** versus stock. Their images matched pixel for pixel. Before/after states confirmed active mesh/task selection, 378 eligible meshlets, and Hi-Z with 117 active occluders. Eligibility/batch statistics are preparation counts, not literal GPU emitted-primitive counters. The Radiance reference differs in lighting and **cannot establish a mesh-shader speedup**. This is one-camera local evidence, not a general benchmark suite.

The requested combined Radiance + mesh3/task1 state was checked separately: the full filter stayed active, but actual mesh/task flags stayed 0. [Frozen harness, results and qualifications](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/meshlet-comparison/README.md), [raw report](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/meshlet-comparison/results.json), [capture/Hi-Z audit](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/meshlet-comparison/capture-parity-audit.json).

**Final all-light filter cost pilot:** E1M1, 1280×720, half scale + checkerboard, quality 0, bounce/emissive 0, point gain 1, source radius 5, filter radius 8. Three clean off/on pairs measured **0.9602 → 1.4040 ms GPU**, paired median **+0.4419 ms/+46.02%**, wall FPS **910.4 → 657.2**. Filtering has a real cost; it is not a free improvement. [Raw report](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/all-light-final2f16-timing/results.json).

## Recommended next directions, in order

1. **Profile the expensive Sponza lighting before redesigning geometry submission.** Measure receiver production, both filters, soft-shadow post, forward model fallback, and especially the four blended surfaces. Use optional stage timings diagnostically, then disable them for clean whole-frame comparisons. Stage intervals overlap and must not be added as if sequential. Compare normal GI gain and the exaggerated GI25 setup; compare full/2⁄3/half scale at the same camera. BSP meshlets touch only 0.38% of this scene's triangles and do not remove MD5 fragment lighting.
2. **Make mesh/task shaders Radiance-compatible if continuing that feature.** `/home/tremor/Desktop/Quiver/Quake/r_brush.c` requires actual indirect rendering for meshlets and explicitly clears mesh/task selection after successful Radiance preparation. Existing mesh graphics pipelines use classic fragment modules. Radiance mesh-stage SPIR-V sources already exist, but Radiance-compatible mesh/task pipeline/layout families and scene/camera descriptor bindings are not wired up. **Deleting the disable gate alone is incorrect.**
3. **Preserve receiver reuse when adding indirect/mesh submission.** `/home/tremor/Desktop/Quiver/Quake/gl_rmain.c` capture currently requires `!indirect`, and world capture records direct indexed batches. A combined path needs stable draw/primitive IDs, geometry/prepass replay parity, descriptors, synchronization and transactional native fallback. Success means actual mesh/task execution **and** actual receiver/model/filter activity advancing in the same frame windows.
4. **Treat static MD5 meshlets as separate work.** Compiling a model into the BSPX lump does not turn it into BSP meshlets. A useful model path would consume retained static mesh geometry, build per-material clusters, conservatively cull them, preserve exact transforms/skins and alpha ordering, and integrate receiver capture. Start with single-frame static geometry. Animated skinning and animated caster support are separate scopes. Benchmark any benefit rather than assuming it from reduced submission counts.
5. **Continue visual-quality diagnosis.** Remaining stipple/probe patterns must be isolated from raw visibility, GI, direct filtering, smooth-normal/plane rejection, alpha cutouts and fallback shading. Fine Sponza column stipple remains unconfirmed; no speculative ray-bias or alpha-threshold fix was made. Keep this separate from the **fixed** grazing-sun regression at `setpos 544 288 32 -15 90 0`, `r_lightmap 1` on start. Its original persistence with point/emissive/bounce gains zero did not exclude authored direct sunlight. Retain that smoke test when changing shared lighting helpers.

## Source map and verification

| Area | Starting points |
| --- | --- |
| Runtime controls and actual reporting | [radiance_runtime.c](/home/tremor/Desktop/Quiver/Quake/radiance_runtime.c), [radiance_engine.c](/home/tremor/Desktop/Quiver/Quake/radiance_engine.c) |
| Frame capture, camera snapshot, model capture | [gl_rmain.c](/home/tremor/Desktop/Quiver/Quake/gl_rmain.c), [r_world.c](/home/tremor/Desktop/Quiver/Quake/r_world.c), [r_alias.c](/home/tremor/Desktop/Quiver/Quake/r_alias.c) |
| Pass planning, ping-pong and descriptors | [radiance_receiver_stage.c](/home/tremor/Desktop/Quiver/Quake/radiance_receiver_stage.c), [radiance_receiver_frame.c](/home/tremor/Desktop/Quiver/Quake/radiance_receiver_frame.c), [radiance_receiver_descriptors.c](/home/tremor/Desktop/Quiver/Quake/radiance_receiver_descriptors.c), [r_passes.c](/home/tremor/Desktop/Quiver/Quake/r_passes.c) |
| All-light/GI Gaussian and geometry guides | [radiance_receiver_filter.comp](/home/tremor/Desktop/Quiver/Shaders/radiance_receiver_filter.comp), [radiance_receiver_filter_axis.glsl](/home/tremor/Desktop/Quiver/Shaders/radiance_receiver_filter_axis.glsl), [radiance_receiver_guides.glsl](/home/tremor/Desktop/Quiver/Shaders/radiance_receiver_guides.glsl) |
| Contact hardening and visibility | [radiance_receiver_soft.comp](/home/tremor/Desktop/Quiver/Shaders/radiance_receiver_soft.comp), [radiance_receiver_shadows.glsl](/home/tremor/Desktop/Quiver/Shaders/radiance_receiver_shadows.glsl), [radiance_receiver_visibility.glsl](/home/tremor/Desktop/Quiver/Shaders/radiance_receiver_visibility.glsl) |
| Mesh/task integration | [r_brush.c](/home/tremor/Desktop/Quiver/Quake/r_brush.c), [gl_rmisc.c](/home/tremor/Desktop/Quiver/Quake/gl_rmisc.c), [radiance_graphics.inc](/home/tremor/Desktop/Quiver/Quake/radiance_graphics.inc), [gl_vidsdl.c](/home/tremor/Desktop/Quiver/Quake/gl_vidsdl.c) |
| Current explanatory docs | [soft shadows](/home/tremor/Desktop/Quiver/docs/radiance-soft-shadows.md), [BSP meshlets](/home/tremor/Desktop/Quiver/docs/bsp-meshlets.md), [static models](/home/tremor/Desktop/Quiver/docs/static-models.md), [map editor](/home/tremor/Desktop/Quiver/docs/map-editor.md), [projectile rebuilds](/home/tremor/Desktop/Quiver/docs/radiance-projectile-rebuild.md) |

Some older receiver/status/deferred documentation predates full-grid/model reuse and all-light filtering. Statements that Direct/Minimum are never blurred, models always bypass reuse, allocations only reserve reduced dimensions, or shared filtering storage is 16,512 bytes are stale or refer to earlier/GI-only paths. Prefer current code, the latest sections above, and exact-hash artifacts. Historical benchmark sections are not current acceptance reports.

Build and focused host checks, from the repository:

```sh
cd /home/tremor/Desktop/Quiver
meson compile -C build-soft-shadow-prototype vkquake
python3 /home/tremor/Desktop/Quiver/tests/radiance_receiver_texture_filter_shader_test.py
meson test -C build-soft-shadow-prototype --print-errorlogs \
  'Prototype receiver frame gate' \
  'Radiance receiver camera capture lifetime' \
  'Radiance live probe spacing transaction' \
  'Radiance frame submission' \
  'Radiance shadow controls and ABI' \
  'Radiance completed receiver diagnostics'
```

The Meson GPU tests are gated: set `QUIVER_TEST_VULKAN=1` and explicitly choose the appropriate Vulkan ICD and SDK validation environment. Relevant names are `Radiance receiver all-light Gaussian GPU`, `Radiance receiver separable Gaussian axis GPU`, and `Radiance receiver contact-hardening shadows GPU`. Run GPU tests separately from timing. Changes to GLSL includes must be registered in Meson/common.make/Windows embedding dependencies as appropriate, or embedded SPIR-V can silently remain stale.

Final evidence:

- [Final acceptance index](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/all-light-acceptance-final2f16.json).
- [Final Sponza application SDK/synchronization validation](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/sponza-final2f16-sdk/results.json).
- [Corrected balcony SDK/synchronization validation](/home/tremor/Desktop/Quiver/build/radiance-shadow-filter-20261106/sponza-corrected-balcony2f16-sdk/results.json).
- [Exact-triangle shader fixtures on Lavapipe and NVIDIA](/home/tremor/Desktop/Quiver/build/receiver-all-light-filter-shader-20261106/exact-id-gpu-results.json).
- The eight odd-641×361 application states belong to the earlier `15c2` build; do not relabel them as exact-final `2f16` application coverage. Final shader fixtures, model captures, corrected-camera validation and timings are recorded separately. Focused host lifecycle ASan/UBSan checks also passed.

For benchmarks, freeze the executable, harness and asset hashes; use a held simulation/camera, warm pipelines, alternate/rotate order, verify actual viewport and active-mode/frame counters, and reject competing-game contamination. Disable validation, stage profiling and diagnostic readbacks during timing. `radiance_stats` and `meshlet_stats` must be checked before/after each window. Whole-frame timestamps and measured engine FPS are different metrics; do not report `1000 / GPU_ms` as measured FPS.

At native widescreen, use `vid_unlock` **before** fullscreen/video settings and verify the PNG size: Wayland can clamp a requested 3440×1440 window to 3440×1371. Cold NVIDIA shader compilation has taken tens of seconds; that is not steady-state frame cost. Preserve launch/teardown failures rather than calling partial runs accepted. The final runs used a five-second idle grace before clean exit; earlier NVVM teardown failures remain historical rejected evidence.
