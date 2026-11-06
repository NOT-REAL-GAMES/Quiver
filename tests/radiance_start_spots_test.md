# Radiance start-map spotlight attribution

`smoke_radiance_start_spots.py` is an optional image diagnostic. It requires
an installed Quake PAK containing a version 29 `maps/start.bsp` with the two
specific `light 4000`, `delay 3`, `mangle "90 0 0"` entities at the positions
specified by the script. It uses no bundled map or other copyrighted assets.
The PAK and the user's configuration are read-only; generated overrides,
configuration, and captures remain in an isolated temporary directory.

For example, on a Linux system with a display and Vulkan software renderer:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  xvfb-run -a python3 tests/smoke_radiance_start_spots.py \
  --binary build/vkquake --basedir /path/to/quake
```

`--basedir` accepts a directory containing `id1` or the PAKs themselves.
The script forces the software Radiance backend, captures the paused spawn view,
removes only those two entities from a temporary external `.ent` file, reloads
the map, and captures the same view. It checks that each side of the central
doorway has a substantial number of strongly brightened pixels in the first
image. Emissive and bounce contributions are disabled to isolate direct light.
Both image paths are printed for inspection. `--validation` additionally
requires the Vulkan validation layer and rejects reported validation errors.

This test establishes which authored lights create those circles. It does not
measure performance, test the hardware ray-query backend, compare against a
fresh ericw light bake, or establish that every Quake distribution contains
this modified `start.bsp`. It fails explicitly if the expected entities are
absent. Pixel thresholds assume the fixed spawn camera and default 90° FOV;
unrelated changes to projection or map assets may require updating them.
