# Moving brush shadow smoke test

`smoke_radiance_moving_shadow.py` builds a small sealed room and compares
paused captures before and after a native `prop_dynamic` brush moves. It checks
that Radiance is active, exactly one dynamic occluder is selected with none
omitted, and the prop's server-side origin moves. Two fixed image regions on
the back wall are outside the visible prop in both poses: one must become
unshadowed as the other becomes shadowed. A no-prop variant supplies the
reference; its BSP geometry and submodels are otherwise identical.

The test requires an installed Quake PAK, a Vulkan-capable display, Pillow and
the engine's bundled map tools. It uses a temporary game directory and reads
the user's PAK without modifying it. For software traversal with 4× MSAA:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  python3 tests/smoke_radiance_moving_shadow.py \
  --binary build/vkquake --basedir /path/to/quake --msaa 4
```

Use `--backend hardware` on a ray-query-capable device. `--validation` requires
a debug engine and Vulkan validation layers; it fails if validation is not
active or reports errors. The captures and generated map are retained in the
printed temporary directory. The test asserts that the requested MSAA and FP16
scene target were actually enabled.

This is a direct-light shadow test: emissive and bounce lighting are disabled
to isolate dynamic occlusion. It covers one translating brush within the
dynamic-occluder budget, not rotated geometry, a moving alias model, falling
props, network visibility changes, or all possible map geometry. The fixed
pixel regions depend on the fixture geometry, viewport, and camera; failures
should be investigated using the captures rather than blindly weakening the
thresholds.
