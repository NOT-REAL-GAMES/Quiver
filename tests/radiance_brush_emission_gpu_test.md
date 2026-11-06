# BSP brush emission GPU fixture

This headless fixture constructs an `msurface_t`/`glpoly_t` brush with an
emissive face whose initial winding opposes its visible BSP plane, plus an
asymmetric opaque blocker. It runs the real brush extraction and cached
emissive-proxy builder, uploads the resulting scene and light, and evaluates
them in the production tracing and lighting shaders. It checks an unblocked
front receiver, a blocked front receiver, a rear receiver, and the prior
two-sided behavior for an unoriented light. It also traces both sides of the
actual uploaded face and verifies emitted-hit suppression behind the face.

Meson builds both software-BVH and ray-query fixtures and their shader
variants. GPU execution is opt-in to avoid unexpectedly using a user's GPU
during the ordinary test suite:

```sh
QUIVER_TEST_VULKAN=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  meson test -C build 'Radiance brush emissive software GPU' --print-errorlogs
QUIVER_TEST_VULKAN=1 VK_ICD_FILENAMES=/path/to/hardware_icd.json \
  meson test -C build 'Radiance brush emissive ray-query GPU' --print-errorlogs
```

Enable Vulkan synchronization validation using the locally installed
validation layer and its configuration when available. Ray query returns
Meson's skip status if the chosen hardware lacks the required features. The
ordinary suite skips both GPU runs unless explicitly enabled.

This uses a constructed brush-model fixture, not a loaded `.bsp` file or a
rendered screenshot. It does not verify animated materials, rotated models,
arbitrary cutouts, or full gameplay visibility.
