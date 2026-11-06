# Radiance backend lifecycle checks

The no-GPU regression compiles the actual `RCFrame_EnsureGraphics` helper with
stubs, checking deferred creation, reuse, missing hardware resources, and
remembered creation failures:

```
TMPDIR="$PWD/build" python3 tests/radiance_backend_lifecycle_test.py
```

For automated live Vulkan checks, build the engine and run the optional smoke
test on a display with a registered Quake PAK. The harness creates an isolated
game directory and leaves saved settings unchanged:

```
TMPDIR="$PWD/build" python3 tests/smoke_radiance_backend.py \
  --binary build/vkquake --basedir /path/to/quake
TMPDIR="$PWD/build" python3 tests/smoke_radiance_backend.py \
  --binary build/vkquake --basedir /path/to/quake --inject-hardware-failure
```

The first run checks software → hardware → software → hardware. If the device
has no ray-query support, hardware requests must remain on software. The second
run injects `VK_ERROR_OUT_OF_DEVICE_MEMORY` for one ray-query fragment pipeline
and checks that software fallback remains active on subsequent hardware
requests. It exercises the injection only when ray queries are available.

With a debug build, `--validation` additionally checks that the Vulkan
validation layer was loaded and no validation or synchronization errors were
reported. For lifecycle coverage not yet automated, repeat after a video restart
and a map change. The first switch may compile the previously unused backend's
pipeline family and take time.

To exercise Radiance across transparency mode changes and video restarts:

```
TMPDIR="$PWD/build" python3 tests/smoke_radiance_variants.py \
  --binary build/vkquake --basedir /path/to/quake --validation
```

This tests standard, WBOIT, MBOIT and a return to standard. Pipeline creation
is limited to the current mode; other modes are built only on first use.

For startup comparisons, time menu readiness separately from map signon and the
first completed Radiance frame, keeping map, resolution, sample count, shader
cache state and edict limit consistent. Report warm and cold runs separately;
CPU software Vulkan timings do not predict performance on a dedicated GPU.
