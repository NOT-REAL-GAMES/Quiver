# Radiance ECS occluder smoke test

`smoke_radiance_ecs_occluders.py` starts a disposable local ripple game and
checks that Radiance is active and its batched aliases enter the same bounded
occluder selection as other entities. It needs an installed Quake PAK, a built
engine, and compiled ripple `progs.dat`; none are modified. For example:

```sh
python3 tests/smoke_radiance_ecs_occluders.py \
  --binary build/vkquake --basedir /path/to/quake \
  --progs build/quiver_ripple/progs.dat --count 1024 --backend hardware
```

Use `--count 32768` for the larger stress map or `--backend software` for the
software BVH. `--validation` requires a debug build and the Vulkan validation
layer. `--capture` saves a screenshot in the printed temporary directory. The
test requires an instanced ECS visual path: it reports an error instead of
claiming batch coverage if the renderer falls back to individual entities.

The test checks active renderer/backend, actual ECS batch and candidate counts,
the shared 256-object limit, and omission accounting. Its selection check uses
the number of ordinary candidates to bound the number of selected ECS aliases
from below. The CPU scene-runtime regression separately traces rays against
batched alias geometry before and after movement, and checks nearest-selection
overflow. Neither test proves which on-screen shadows are visible in this dense
scene, nor measures performance; use `bench_radiance.py` for timing.
