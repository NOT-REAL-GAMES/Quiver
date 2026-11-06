# Optional radiance source smoke test

`tests/radiance_map_smoke_test.c` reads a local Quake `pak0.pak` directly; it
never starts the game or reads or changes game configuration. It exercises
real `start`, `e1m1`, and `e1m2` BSP entity lumps and embedded indexed texture
pixels (including fullbright masks), builds the packed material atlas, and
passes each frame snapshot to a fake GPU uploader. The test requires a PAK
containing those maps and SDL3 and Vulkan development headers. It is not in
the default test suite because game data is not distributed with Quiver.

From the repository root, for example:

```sh
cc -std=gnu23 -O1 -Wall -Wextra -Wno-unused-parameter \
  -D_GNU_SOURCE -DUSE_SDL3 -IQuake $(pkg-config --cflags sdl3 vulkan) \
  tests/radiance_map_smoke_test.c Quake/radiance_coordinator.c \
  Quake/radiance_lights.c Quake/radiance_materials_quake.c \
  Quake/radiance_material_atlas.c -lm -o /tmp/quiver-radiance-map-smoke
/tmp/quiver-radiance-map-smoke /path/to/id1/pak0.pak
```

This is a source and atlas test, not an in-game rendering or performance test.
It does not exercise external `.ent` files, external replacement textures,
animated texture selection, alias models, or actual GPU uploads. Separate
focused tests cover those adapters and Vulkan uploads; full renderer
integration and visual validation remain necessary.

Reduced receiver coverage includes divisors 2, 3 and 4, odd viewport origins
and extents, checkerboard reconstruction, and legacy cvar transitions:

```sh
meson test -C build 'Radiance resolution scale' 'Prototype receiver*'
QUIVER_TEST_VULKAN=1 meson test -C build 'Radiance receiver scales GPU' \
  'Radiance receiver visibility batch*' 'Radiance checkerboard reconstruction equivalence GPU'
python3 tests/smoke_radiance_receiver.py --binary build/vkquake \
  --basedir /path/to/quake --backend hardware
```

The in-game smoke uses a 641×361 window, exercises every reduced scale with
and without checkerboarding, and checks restart, map-change, indirect-world
and MSAA fallback. Run with `--backend software` to exercise software traversal.
Vulkan synchronization validation can be injected with a locally installed
validation layer; use `--validation` so the harness verifies it loaded and
checks startup through shutdown for errors.

# Alternating whole-frame benchmark

With the game closed, run the release executable against the same stock maps
and ECS ripple scenes using isolated configurations:

```sh
python3 tests/bench_radiance.py --binary build/vkquake \
  --basedir /path/to/quake --progs build/quiver_ripple/progs.dat \
  --scenes start e1m1 ecs1024 ecs32768 --backend auto \
  --pairs 5 --warmup 5 --seconds 5 --capture
```

The harness alternates classic and radiance ordering, saves the binary hash,
sample counts, state and optional captures to a new results directory, and
refuses to run alongside another vkQuake process unless `--allow-existing` is
explicitly supplied. It reports elapsed frame throughput, completed-frame GPU
means and medians, and separately sampled radiance prepass GPU times. Receiver
shading is included in the whole-frame timer and excluded from the prepass.
The paired result is the median of each run's radiance-minus-classic GPU frame
medians; it is not a paired per-frame delta distribution. If sample history
was truncated or the GPU median is unavailable, the median comparison is left
unverified. Validation, when requested, changes the timing workload.

