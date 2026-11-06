# Radiance world-normal GPU test

This test rasterizes a world-space wall through the same GLSL helper as the
world receiver, using Quake's positive-height viewport and camera convention.
It checks that the visible side points toward the camera and accepts a light
on that side. It requires Vulkan graphics and fragment storage writes, but no
window or game data.

From the project root:

```sh
glslangValidator -V --target-env vulkan1.1 -IShaders \
  -o /tmp/radiance-world-normal.vert.spv tests/radiance_world_normal.vert
glslangValidator -V --target-env vulkan1.1 -IShaders \
  -o /tmp/radiance-world-normal.frag.spv tests/radiance_world_normal.frag
cc -std=c11 -Wall -Wextra -Werror tests/radiance_world_normal_test.c \
  -lvulkan -lm -o /tmp/radiance-world-normal-test
/tmp/radiance-world-normal-test /tmp/radiance-world-normal.vert.spv \
  /tmp/radiance-world-normal.frag.spv
```

It can also be run with Vulkan synchronization validation enabled. The test
does not exercise the full engine renderer.
