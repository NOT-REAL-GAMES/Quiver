# Radiance scene collection Vulkan test

This optional headless integration test uses a real Vulkan device, with real
scene and lighting uploaders and in-memory Quake texture sources. It checks
packed material rectangles, source texels, unsignaled-fence rejection, submission,
abandoned recordings and retry, dynamic-brush refit, material removal and
replacement without dereferencing stale textures, atlas generation reuse and
wraparound, failed source fallback, map replacement and a sky-only world with
no source texels. It does not activate
the renderer, test ray queries, or
measure in-game performance. A Vulkan implementation and SDL3 development
headers are required.

From the repository root on Linux, run:

```sh
mkdir -p build/inspector-build-tmp
cc -std=gnu2x -Wall -Wextra -Werror -Wno-unused-parameter -DUSE_SDL3 \
  -IQuake -IQuake/mimalloc $(pkg-config --cflags sdl3) \
  tests/radiance_scene_collect_test.c Quake/radiance_scene_collect.c \
  Quake/radiance_scene_runtime.c Quake/radiance_scene_bsp.c Quake/radiance_scene.c \
  Quake/radiance_scene_vulkan.c Quake/radiance_scene_rt.c \
  Quake/radiance_coordinator.c Quake/radiance_lights.c \
  Quake/radiance_emissive.c \
  Quake/radiance_materials_quake.c Quake/radiance_material_atlas.c \
  Quake/radiance_gpu_inputs.c -lvulkan -lm \
  -o build/inspector-build-tmp/radiance_scene_collect_test
build/inspector-build-tmp/radiance_scene_collect_test
```

To select Mesa's software implementation, set `VK_ICD_FILENAMES` to the
installed lavapipe ICD JSON. With the Vulkan validation layer installed, set
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and
`VK_LAYER_VALIDATE_SYNC=1` for synchronization validation. The test asserts
on setup or capability failure and therefore is not registered as a mandatory
Meson test on machines without Vulkan.
