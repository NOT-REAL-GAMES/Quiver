# BSP meshlets

| Console setting | Static-world visibility |
| --- | --- |
| `r_meshlets 0` (default) | Stock indirect BSP renderer and Quake PVS. |
| `r_meshlets 1` | Meshlet frustum culling, then BSP PVS and facing tests on the source faces. |
| `r_meshlets 2` | GPU meshlet frustum, occlusion and facing tests, independent of the BSP PVS. |
| `r_meshlets 2` with `r_meshlet_occlusion 0` | Independent frustum and facing tests. Use as the image and timing reference for occlusion. |
| `r_meshlets 3` | Experimental Vulkan mesh shaders for eligible static opaque world batches, with independent GPU occlusion. |

You can switch modes on a loaded map. The engine bakes the compiled map's static surfaces at load time when meshlets are enabled, or on first use otherwise. Faces share a cluster only within a material and lightmap batch. Each cluster holds up to 64 vertices and 126 triangles. A 256-unit limit on grouped face bounds keeps a texture shared by distant rooms from producing one box spanning both rooms. Large individual faces retain their original geometry and winding. The GPU appends surviving indices to the existing indirect material draws. This uses Vulkan compute and requires no mesh-shader extension.

## Mesh shaders

Set `r_meshlet_task 1` with mode 3 to enable task-shader culling on supported devices. `r_meshlet_task 0` uses mesh shaders without the task stage. `meshlet_stats` reports which path is active.

Mode 3 bakes meshlet-local vertex and primitive streams from the compiled map. On capable devices, the Vulkan mesh shader performs frustum, optional depth-pyramid and per-surface facing tests, then emits geometry directly. Static opaque materials without texture animation use this path. Sky, liquid, cutout and animated materials, moving brush models and wireframe views use the indexed path. If the device, shader compiler, pipeline, resources or map cannot support the mesh path, the engine falls back to mode 2. `meshlet_stats` reports whether mesh shaders are active and how many clusters are eligible. Its compute-culling counters cover the indexed portion of mode 3, not the mesh-shader portion.

## Independent GPU occlusion

The engine selects up to 256 nearby opaque, stationary world faces from a map-local spatial index. It reuses a candidate list until the camera moves 64 units or turns about 11 degrees. Coverage and depth are rebuilt from the current camera every frame. Each frame compute shaders project those occluders with the **current camera** into a 64×64 depth grid. A cell contributes depth only if a real surface covers it completely, including a guard for raster precision and multisampling. Uncovered cells stay clear. Successive levels store the farthest guaranteed depth in each region, using the engine's reversed-Z convention.

The GPU projects each meshlet's bounding box and discards it only when the stored depth in every cell covering the box is closer than the nearest possible point of the box. A hole, an inconclusive depth comparison or a box crossing the near plane keeps the cluster eligible to draw. Camera cuts need no history reset. Doors, animated materials, fences, sky and liquids are excluded as occluders. The BSP still supplies map geometry, entity visibility and collision. Sorted translucent water retains its CPU draw order, with static water faces eligible independently of leaf PVS.

`meshlet_stats` reports the active mode and how many occluders were submitted. Enable `r_meshlet_occlusion_stats 1` to also see completed-frame counts for tested, frustum-rejected, occluded and emitted meshlets, plus emitted static triangles. The counters arrive after frame-slot reuse. Leave the counters off for benchmarks. `r_showtris 1` displays submitted wires through walls, including geometry the conservative test couldn't prove hidden. `r_showtris 2` depth-tests the overlay.

If the depth resources are unavailable, mode 2 keeps the independent frustum and facing tests. If indirect rendering or the meshlet cache itself is unavailable, the stock renderer handles the map. The meshlet cache has a 128 MiB GPU limit. Leave `r_indirect 1`, `r_gpulightmapupdate 1` and `scr_speeds 0` for comparisons. `gpu_frame_time start` and `gpu_frame_time stop` measure completed whole GPU frames without switching rendering paths.

## Reproducing the comparison

With a Vulkan display, SDL development headers, Pillow and local game paks, run the four rendering paths on the same demos.

```sh
meson setup build --buildtype=release
ninja -C build
meson test -C build
python3 tests/bench_meshlets.py --binary build/vkquake --basedir /path/to/Quake \
  --modes 0 1 2 --frustum-control --pairs 4 --gpu-pairs 4 --gpu-seconds .75
```

The harness uses an isolated game directory, warms each selected path, balances run order on identical demos, checks the rendered frame counts, and captures the same paused views on each path. GPU samples are measured separately from unprofiled timedemo results. Reports and images go to a disposable directory under `/tmp`.

Run `python3 tests/smoke_meshlet_occlusion.py --binary /path/to/debug/vkquake --basedir /path/to/Quake --validation` for the occlusion regressions. This requires the Vulkan validation layer with synchronization validation enabled. The harness compares the shaded images against independent frustum-only rendering, exercises MSAA, video restart and abrupt camera changes, and generates its own empty-PVS fixture from the local game data.

## Independent occlusion measurements

Measured at 1280×720 on a Ryzen 9 7900X and RTX 4080 (Linux/Wayland, NVIDIA 615.71.09). All four paths used the same release executable, VSync off and diagnostic counters off. Each route was warmed before four order-balanced rounds. The columns are median **whole-frame milliseconds** from unprofiled timedemos.

| Scene | Stock | Hybrid | Independent frustum | Independent Hi-Z |
| --- | ---: | ---: | ---: | ---: |
| id1 demo1 | 0.553 | 0.563 | 0.590 | 0.614 |
| id1 demo2 | 0.538 | 0.554 | 0.586 | 0.605 |
| id1 demo3 | 0.568 | 0.575 | 0.596 | 0.619 |
| mg1 hub route | 0.737 | 0.752 | 0.760 | 0.774 |
| mg1 mge2m2 route | 0.655 | 0.660 | 0.675 | 0.717 |

Separately, whole-frame GPU timestamps at fixed cameras gave these median times in milliseconds across four sampling windows per path.

| Scene | Stock | Hybrid | Independent frustum | Independent Hi-Z |
| --- | ---: | ---: | ---: | ---: |
| start | 0.193 | 0.210 | 0.219 | 0.238 |
| e1m1 spawn | 0.146 | 0.158 | 0.191 | 0.214 |
| e1m1 corridor | 0.199 | 0.215 | 0.220 | 0.241 |
| e1m2 | 0.188 | 0.204 | 0.218 | 0.244 |
| hub | 0.373 | 0.383 | 0.415 | 0.424 |
| mge2m2 camera A | 0.186 | 0.203 | 0.252 | 0.265 |
| mge2m2 camera B | 0.296 | 0.306 | 0.329 | 0.349 |

At the start-map door, occlusion rejected 115 meshlets beyond frustum culling and reduced emitted triangles from 5,160 to 4,473. In the e1m1 corridor it rejected 243 and reduced triangles from 4,946 to 3,790. Some sky- and water-facing views rejected none. The GPU images with and without occlusion matched pixel for pixel at the seven benchmark cameras. The reduction in submitted geometry did not offset the cost of independent culling on these routes. Stock BSP remains the default.

## Follow-up experiments

A compact shader that processed 64 meshlets per workgroup increased GPU frame time on the four original-map cameras. A pass that tested groups of 32 spatially sorted meshlets before individual occlusion tests also increased GPU time on the three larger-map cameras. Both experiments were removed. Bounds of 512 and 1024 units per cluster gave mixed results against 256 units and did not produce a consistent gain.

A visibility probe found that six of seven tested views needed every world lightmap atlas, and all seven needed every liquid texture. The proposed per-batch frustum checks were therefore not added. The current independent path reuses nearby opaque candidate lists across small camera movements and skips lightmap block scans when their styles and dynamic lights have not changed. A runtime regression exercises an active dynamic light and its removal across renderer switches. The full-frame results still favor stock on the measured routes, so further optimization is needed.

## Mesh-shader measurements

On the same Ryzen 9 7900X and RTX 4080 at 1280×720, the first mesh-shader implementation rendered the two moving `mg1` routes at 0.817 and 0.733 ms per frame. Stock rendered those routes at 0.771 and 0.668 ms. At three fixed cameras, whole-frame GPU times were 0.414, 0.287 and 0.365 ms for mesh shaders, versus 0.358, 0.189 and 0.300 ms for stock. These are separate timing windows and apply to the tested device and scenes.

A separate pass profiler measured Hi-Z generation at 0.012–0.020 ms per frame and compute meshlet culling at 0.014–0.026 ms across the larger-map cameras. The optional task shader culls up to 32 meshlets per workgroup before launching mesh work. It was slower on all three tested cameras and remains opt-in for evaluating larger maps. The mesh-shader path remains experimental while these costs and its material draw overhead are optimized.

## Earlier hybrid measurements

The earlier hybrid benchmark used a Ryzen 9 7900X and RTX 4080 (Linux/Wayland, NVIDIA 615.71.09, 1280×720, VSync off). It predates the independent path and the 256-unit cluster limit. These are median **whole-frame times in milliseconds** across six alternating pairs; the percentage column is the median of the per-pair changes, so it need not equal the ratio of the two medians. Positive means meshlets took longer.

| Timedemo | Stock | Meshlets | Paired change | Range of paired changes |
|---|---:|---:|---:|---:|
| id1 demo1 | 0.573 | 0.590 | +2.8% | +2.2% to +10.6% |
| id1 demo2 | 0.560 | 0.578 | +3.4% | +2.4% to +5.1% |
| id1 demo3 | 0.596 | 0.611 | +1.5% | +0.8% to +5.2% |
| Dimension of the Machine: hub | 0.777 | 0.790 | +1.8% | −1.4% to +11.4% |
| Dimension of the Machine: mge2m2 | 0.671 | 0.682 | +2.5% | −6.0% to +7.8% |

Separately, repeated **whole-frame GPU timestamps** at seven fixed cameras showed a 2.6–9.9% cost increase. On the hub the median was 0.364 ms for stock versus 0.375 ms for meshlets. Individual host pairs on the larger maps went both ways, but every GPU sampling window favored stock. These results apply to the tested hardware and scenes.

Batching visible indices by material helped. A version using one indirect command per meshlet and separate in-flight work buffers measured 0.354 versus 0.410 ms (stock versus meshlets) on the hub. Its meshlet allocation was 19.10 MB. The retained material-batched path used 7.90 MB for 12,473 meshlets covering 445,917 triangles in 77 material batches. The reported bake took about 34 ms, including CPU partitioning, resource setup and staging the upload; it is not total map load time.

The seven fixed cameras matched stock pixel for pixel. The bake unit test checks triangle ownership, bounds, material grouping and large faces. An additional offline comparison of 38 original maps matched all 485,257 world triangles with their original winding. For doors, sky, translucent water, show-triangle overlays, map changes, video restarts and fallback paths, run `tests/smoke_bsp_meshlets.py`. Its `--validation --no-rt-shadows` mode isolates meshlet synchronization from existing ray-tracing scratch-buffer hazards that also reproduce with the stock renderer.

To generate moving-camera comparisons with the rerelease assets installed:

```sh
python3 tests/bench_meshlets.py --binary build/vkquake --basedir /path/to/Quake/rerelease \
  --game mg1 --record-views --modes 0 1 2 --frustum-control --pairs 4 --gpu-seconds .75 --gpu-pairs 4 \
  --view hub 0 -1920 24 0 90 0 --view mge2m2 3424 -920 -532 0 180 0 \
  --view mge2m2 402 1515 419 24 231 0
```

The harness retains the recorded demos; pass `--demo-files` on subsequent runs to replay the exact same routes.
