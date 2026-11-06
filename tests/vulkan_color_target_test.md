# Transactional color target tests

On Linux with a compiler/linker supporting function sections and garbage
collection, Meson registers CPU-only tests. They compile the actual renderer
helpers against mocked Vulkan calls and need no Vulkan device or game assets:

```sh
meson test -C build 'Vulkan color target failure cleanup' 'Vulkan memory failure accounting' 'Vulkan framebuffer failure cleanup' 'Vulkan render-pass failure cleanup' --print-errorlogs
```

The first injects failure at every image creation, memory-type lookup,
allocation, binding and image-view stage for scene, MSAA, UI, depth and both
OIT modes.
It checks that every partial resource is destroyed exactly once and that an
immediate retry succeeds. It also injects descriptor allocation failures for
depth and scene descriptors, checking replacement rollback and retry. A debug
variant includes the ray-query descriptor in the same failure/retry matrix. The second
checks the real tracked allocation helpers:
a failed driver allocation, even one that writes a garbage output handle,
leaves the caller's memory object and global accounting untouched; a successful
allocation and free balances the counters. Descriptor failures likewise leave
the output handle and descriptor accounting unchanged.

The framebuffer test injects each host allocation and Vulkan framebuffer
creation failure across scene and UI passes in all transparency modes. It
checks rollback, rejection of a second creation over live resources, and
successful retry.

The render-pass test injects failure at each pass creation, checking layout and
binding rollback, persistent warp-pass ownership, and successful retry.

These tests do not simulate pipeline creation failure and do
not establish a complete HDR fallback. The optional
`smoke_radiance_variants.py --validation` test covers real Vulkan render-mode
and video-resource recreation separately.
