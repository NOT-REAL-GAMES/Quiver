# Sparse cascade merge regression

The headless test compares a bounded GPU merge against a forced full merge,
checks coarse-to-fine propagation, and exercises topology changes and discarded
recordings. A topology-change case retains an open coarse raw interval with a
stale valid merge while its far support is invalid, then traces only the finest
level. The synthetic topology trace checks that coarse validity is cleared
before tracing, not merely before receiver shading. It requires a Vulkan
implementation and the built compute shaders.

```sh
QUIVER_TEST_VULKAN=1 \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
  meson test -C build --no-rebuild 'Radiance sparse GPU merge' --print-errorlogs
```

Set `RC_MERGE_BENCH=1` to run eight alternating, warmed timestamp sets on a
512-brick finest level with a 256-interval trace. It clears the hierarchy once;
the otherwise invalid, empty probes make this a synthetic lower-bound workload,
not an in-game benchmark. On llvmpipe (LLVM 21.1.8), the complete trace plus
merge averaged 0.141 ms bounded, 0.353 ms with a dirty finest level, and
0.608 ms with the entire hierarchy dirty. No RTX or frame-time conclusion
follows from this software-renderer measurement.
