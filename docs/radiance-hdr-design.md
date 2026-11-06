# Radiance floating-point scene target

`r_radiance_hdr 1` requests an FP16 scene while Radiance is enabled;
`r_radiance_hdr 0` selects the classic UNORM target. The renderer checks the
FP16 format features before trying it and retries the complete scene-resource
sequence with UNORM if allocation or creation fails. A failed attempt is
latched so it is not retried every frame; switching the setting off and on
allows a new attempt. The renderer reports the selected scene format at startup
and video restart.

The FP16 scene includes OIT and MSAA resolution, then applies the optional
Radiance highlight shoulder in screen effects, before palette quantization,
polyblend and menu dimming. UI is drawn afterwards into a separate UNORM target
at native resolution. Fullbright and lightmap debug views bypass the shoulder;
turning off polyblend also disables its tint when HDR forces screen effects.
Palette lookup and its edge filter clamp their inputs after highlight mapping.
Classic rendering uses the original UNORM path.

The current implementation is opt-out, and the extra full-screen pass and HDR
format can have a measurable GPU cost. Palettization necessarily limits colors
to the palette; it is not an HDR display output. Classic lightmap and gameplay
compatibility paths are unchanged. Hardware GPU performance and visual quality
still require separate measurement on the target device.

Local software Vulkan checks in `build/quiver-radiance-variants-3967yuup/`
covered native-resolution FP16, MSAA, OIT modes, resource recreation and a
video restart with synchronization validation. The isolated screenshots in
`build/quiver-highlight-smoke-6ki4o6_z/` verified that highlight compression
changes clipped pixels, classic and fullbright toggles are unchanged, disabled
polyblend suppresses a persistent red tint, enabled tint is visible, and palette
mode affects the image. These fixtures hide the HUD; they do not establish
pixel-perfect native HUD or menu parity on hardware. The separate 4× MSAA
transparency test in `build/quiver-radiance-transparency-axw9obg7/` verifies
native HUD/fullbright shoulder-toggle parity, visible OIT composition and a
clean synchronization-validation log across scene restarts and screenshots.
The corresponding no-MSAA run is in
`build/quiver-radiance-transparency-xeauo0bf/`.
An RTX 4080 synchronization-validation smoke subsequently confirmed the FP16
target and ray-query backend at 4× MSAA across OIT 0/1/2/0 and a real video
restart (`build/quiver-radiance-variants-mnf3a532/`). That run checked active
paths and resource lifetimes, not hardware image parity or performance.
An RTX 4080 ray-query run in `build/quiver-radiance-variants-mnf3a532/`
also passed synchronization validation with FP16, 4× MSAA, all three OIT
modes and a confirmed video resize and restart. It checks pipeline/resource
lifetimes and active paths, not image parity or timing.

The classic scene target is UNORM, so radiance can clip before transparent
geometry is composited. Its per-receiver highlight shoulder reduces clipping;
the FP16 path instead maps the composited scene in one pass.

A local FP16 prototype used separate scene and UI formats, mapped the scene
before the UI pass, and selected matching FP16 storage-image shaders for screen
effects and ray debug. It recreated targets when the experimental setting or
Radiance changed, and checked sampled, linear-filtered, blendable, color and
storage format support before choosing FP16. A software Vulkan test exercised
mode toggles, four-sample MSAA (confirmed by the renderer log), odd render size,
palette effects, transparency modes, and video restarts with synchronization
validation. An opaque HUD crop remained identical when the scene shoulder was
toggled. These captures are in `build/quiver-highlight-smoke-0vi9mlmh/` and
`build/quiver-radiance-variants-1nsl6pjb/` when those local artifacts exist.

The original prototype was removed because feature checks alone are not sufficient for
safe fallback. The scene and UI color-image helpers now have nonfatal creation
paths that clean up partial images, memory and views; their existing classic
callers still treat failure as fatal. Framebuffer creation likewise provides a
nonfatal transactional path, with classic callers retaining their existing
fatal policy. Render-pass creation also offers transactional rollback while
preserving the independently owned warp pass. Linux fault-injection tests
exercise image, memory-type, allocation, binding, view, framebuffer and
render-pass failures, including successful retry and tracked allocation
accounting. Scene and UI depth images and both OIT image sets also have
transactional creation helpers; their classic callers retain fatal handling.
Scene and depth descriptor allocation is recoverable, with old scene descriptors
retained until replacement succeeds. Required renderer shader-module creation
also rolls back previously created modules on failure; optional meshlet and ECS
modules may fail independently. A fault-injection test exercises the renderer's
actual module list in MSAA, SSAO, ray-query and FP16-compute configurations,
including mesh/task shaders where built, and checks retry. The descriptor pool
includes extra per-type and set capacity for the temporary replacement sets.
Renderer graphics and compute pipeline creation also has an attempt-local
rollback path: failures release newly created pipelines and compatible pass
alternatives without resetting Radiance or ECS. Fault injection covers every
actual Vulkan pipeline creation point in debug and release, with SSAO enabled
and disabled, optional-pipeline degradation, allocation failure and retry.
SSAO image, memory, view, sampler and descriptor creation is also transactional;
its Hilbert texture upload is deferred until a caller explicitly finalizes the
successful attempt. A fault-injection test covers all 97 allocation/binding
failure positions and immediate retry, including undefined driver outputs.
The classic wrapper still treats required-resource failure as fatal. The
complete scene-resource attempt now uses those helpers and retries the UNORM
path after a failed FP16 attempt. An integration fault-injection test covers
each mocked stage, rollback, latching, and retry.

For HDR input the palette octree clamps values before indexing and uses the
same mapping for its Sobel neighborhood and center pixel. WBOIT and MBOIT retain
HDR color while still clamping alpha; the classic variant retains its existing
clamping. Further hardware testing of native UI, transparencies and moving
content remains necessary.
