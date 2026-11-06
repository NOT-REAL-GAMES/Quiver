# Optional synthetic shadow benchmark

This headless benchmark measures a fixed BVH shadow-ray workload using Vulkan
timestamps. It can run on lavapipe without a window or game data. Its timing
is a microbenchmark, not an in-game frame time or an RTX performance claim.

The separate scene/receiver correctness fixture is registered with Meson for
both software traversal and ray queries. It compiles by default, but does not
create a Vulkan device unless explicitly enabled. For example:

```sh
QUIVER_TEST_VULKAN=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  meson test -C build --no-rebuild 'Radiance scene software GPU' --print-errorlogs
```

Use `Radiance scene ray-query GPU` with a ray-query-capable Vulkan device for
the hardware variant. Initial shader compilation can take several minutes.
The fixture checks attenuation formulas, spotlight cones, scene tracing,
cutouts and emissive visibility; it is not an in-game image comparison.

```sh
glslangValidator -V --target-env vulkan1.1 -IShaders \
  -o /tmp/radiance-shadow-bench.spv tests/radiance_shadow_bench.comp
cc -O2 -std=gnu2x -Wall -Wextra -Werror -IQuake \
  tests/radiance_scene_gpu_test.c Quake/radiance_scene_vulkan.c \
  Quake/radiance_scene.c Quake/radiance_gpu_inputs.c -lvulkan -lm \
  -o /tmp/radiance-shadow-bench
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  /tmp/radiance-shadow-bench /tmp/radiance-shadow-bench.spv --bench
```

Compile with `-DRC_BENCH_AREA=1` for four-tap rectangular emitters or
`-DRC_BENCH_DIRECT=1` for a mixture of in-range and out-of-range lights. Each
run reports three samples and an output checksum; alternate baseline and
candidate runs, discard warm-up samples, and verify matching checksums.

`-DRC_BENCH_FACING=1` exercises local-light shading with half the surfaces
facing away from the light. Four alternating warmed pairs on llvmpipe (LLVM
21.1.8) measured approximately 16.6 ms before the early Lambertian rejection
and 14.1 ms after, with the same checksum (85967). This synthetic result does
not predict the speedup on an RTX or in an actual game scene.

For the area workload, `-DRC_BENCH_AREA=1` uses four shadow samples and adding
`-DRC_BENCH_SINGLE=1` uses the quality-0 center sample. Four alternating warm
pairs on the same llvmpipe driver measured approximately 15.0 ms and 3.8 ms,
respectively. The outputs differ as expected: the single sample produces hard
shadows instead of the four-sample approximation to area-light penumbrae.
