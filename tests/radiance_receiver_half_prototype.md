# Experimental reduced-resolution Radiance receiver

The renderer can capture direct world batches and use a full-resolution opaque
receiver prepass to classify 2×2, 3×3, or 4×4 blocks. Safe blocks receive reduced-resolution
lighting; incompatible or invalid samples use the existing full-resolution
forward path. Receiver identity, normal and plane tests protect geometry
boundaries. Probe visibility checks further limit bounce leakage, but they do
not prove that a direct shadow edge is constant within one surface.

`r_radiance_resolution_scale 0.5`, `0.333333333`, or `0.25` requests this path;
`r_radiance_half_resolution 1` remains an alias for `0.5`; `r_radiance_checkerboard 1`
also computes direct and bounce on complementary receiver-grid cells. A separate
compute pass reconstructs the missing checkerboard samples into distinct HDR
images. Samples that fail its strict tests are invalid and fall back. The
receiver images, descriptors, pipelines and alternate command buffers belong to
fenced frame slots. Outputs are transitioned between producer, reconstruction
and forward sampling. The classic world commands remain available if staging
fails.

The experimental path currently requires direct world draws (`r_indirect 0`),
single-sample rendering, supported formats and the geometry shader feature.
Indirect/meshlet world draws, MSAA, viewmodels, entities and transparency retain
full-resolution shading. The `radiance_stats` active flags describe actual
alternate world submission, not whether it was merely requested. Do not infer
GPU savings from the reduced sample count: prepass, reconstruction, forward
visibility checks and fallback all cost time. Benchmark complete frames at
matched settings, including close walls and Gaussian bounce filtering.

The classifier, target and descriptor lifetime, staging, alternate selection,
and frame submission have automated tests. Run them with:

```
meson test -C build 'Prototype receiver*' 'Radiance frame submission' --print-errorlogs
python3 tests/radiance_receiver_shader_test.py
```

The standalone Vulkan resolve and checkerboard fixtures can be run on Lavapipe;
they check interpolation, invalid values, visibility boundaries and odd viewports.
Hardware visual checks and Vulkan synchronization validation remain necessary
for changes to the complete frame graph, especially after video restart, map
change, viewport resize, changing lights or geometry, and resource allocation
failure. The validation layer may not be installed on all systems.
