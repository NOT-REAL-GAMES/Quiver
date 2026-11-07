# Receiver contact-hardening shadows

The experimental receiver shadow path samples a finite emitting area through the
existing scene visibility queries. Contacts stay sharp and shadows spread as
the receiving surface moves away from a blocker. Classic PCSS instead searches
and filters a shadow map; this implementation uses ray-sampled area visibility
without introducing shadow maps or a deferred renderer.

```text
r_radiance_soft_shadows 1
r_radiance_shadow_radius 2
r_radiance_shadow_sun_angle 0.5
r_radiance_shadow_filter 1
r_radiance_shadow_filter_radius 8
```

The feature defaults to off. `r_radiance_shadow_radius` defaults to 2 and is an explicit source
radius approximation in world units for point and spot lights, clamped to
0–64. It is separate from light attenuation range. `r_radiance_shadow_sun_angle`
is an angular **radius** in degrees, clamped to 0–15. Emissive rectangles retain
their actual emitting geometry. Quality 0 emitters can use this path; higher
quality emitters keep their existing four-sample visibility.

The brightest eligible positive additive source retains its original
centre/closest visibility sample and receives three stable, balanced outer samples.
The correction fades to zero when the two strongest candidates approach equal
strength, avoiding an abrupt change when their ranks switch. Other sources,
subtractive lights and local minimums keep their original visibility. Candidate
selection records the winner and its existing hard visibility in the producer.
A separate build of the direct pipeline applies the three outer queries, so the
additional query count is at most three per receiver, independent of the number
of lights, and hard shadows pay nothing for it.

`r_radiance_shadow_filter` applies a geometry-guided Gaussian to the combined
direct-light result from **every** point, spot, emissive and directional light.
There is no dominant-source selection or source-ID barrier in this filter.
Signed additive/subtractive direct lighting and minimum lighting are filtered
as separate fields, preserving their separate composition. It defaults to `1`,
accepts fractional strengths from `0` to `2`, and uses `0` to retain the original
unfiltered soft-shadow result. The master `r_radiance_soft_shadows` setting must
be on. Gaussian filtering can still run if the optional contact-ray pipeline
is unavailable.

`r_radiance_shadow_filter_radius` defaults to `8` world units and accepts `0` to
`128`; zero disables the Gaussian. Increasing it widens the fade. It is separate
from the finite light-source radius and from probe spacing. The kernel support
is 1.5 radii through strength 1 and 2.5 radii above 1, capped at 448 receiver
texels per axis and stopped by geometry or unknown-sample barriers.

Ray-sampled contact hardening, the all-light Gaussian and indirect-light
`r_radiance_probe_gaussian` smoothing can all run together. The all-light filter
uses nine logical interpolated taps per axis, with one shared geometry cache
and separate validity barriers for each field. Missing GI data cannot block
valid direct lighting. It adds no visibility rays or render targets and uses
the existing horizontal/vertical/copy dispatches, including when checkerboarding
and indirect lighting are both off.

The cache retains exact draw/primitive IDs. Samples on the same native triangle
can use its known plane even when smooth shading normals tilt away from that
plane. Other triangles allow the slope that the normal-angle limit permits over
the sample distance, so curved surfaces filter across their triangle edges while
real steps stay barriers. Finite and normal-angle
checks remain in both cases. This avoids falsely blocking filtering inside
curved model triangles. Shared storage totals 34,328 bytes at 512 lanes, or 31,988 at the 416 lanes used on devices limited to 32 KiB. `RC_AXIS_RADIUS` is the storage limit, not the load size: each tile loads its core cells, derives an upper bound on its lanes' radii, then loads only that reach per field. A barrier at the edge of the loaded range stops segment searches, and the GPU fixtures compare every result bit for bit against a full-reach load.

Opaque brush entities (doors, platforms, trains) replay through the world receiver stream with their own `mvp`, so they receive the same filtered lighting as the world. Their native draws sit in `SCBX_RECEIVER_BRUSHES`, which runs only when the world replay does not. Translucent entities, water and sky stay on the native path, and an entity that would overflow its capture stays native for that frame. `radiance_stats` reports `brush entities receiver draws`, `brush_frames_active` and `brush_frames_fallback`. `gpu_stage_time` also splits `receiver_filter` into `filter_horizontal`, `filter_vertical` and `filter_copy`.

Two controls shape receiver lighting at reduced scales. `r_radiance_gi_normal_bias` (default 0.1, 0 to 1) lifts each receiver's GI lookup along its normal by that fraction of probe spacing; bounce transport keeps its own offset, so changing it leaves probe history alone. `r_radiance_upscale_blur` (default 1, 0 to 1) blends the upscale kernel for GI and direct light from one receiver texel of radius up to `r_radiance_upscale_blur_radius` (default 2, 1 to 16). The kernel is measured in receiver texels, so its size on screen grows with the divisor, and it has no effect at full scale. Taps on other surfaces are excluded, so light does not bleed across edges, but shadow edges soften by the same amount. Zero is plain bilinear. Beyond three texels the taps move to a coarser fixed lattice rather than multiply, capped at six per axis. Above bilinear, geometry fades a tap's weight instead of rejecting it, so curved surfaces do not stipple. Direct light keeps the bilinear upscale. `r_radiance_depth_aa` (default 0) keeps the majority surface in a block that crosses a silhouette, instead of dropping the block to the per-pixel path. At 1 the vote uses the pixels the prepass already has. At 2, 4 or 8 the prepass renders that many samples per pixel, and the classifier votes across them and rebuilds each position at its true sample location, so a one-pixel block can tell which surface really covers it. Higher values use the largest level the device can render and read back, which is 8 on most hardware, so 16 behaves as 8. A collapse pass then keeps each pixel's largest facet for the stages that read one sample per pixel. Multisampled targets cost 20 bytes per sample per pixel in each of the two frame slots, about 1.6 GB in total at 8x and 3440x1440. Depth is never averaged across surfaces, because that would invent points between them.

Opaque world liquids join the world receiver stream, with their native draw in the last `SCBX_RECEIVER_BRUSHES` context. Their token range lets the filter recognize liquid texels, and `r_radiance_water_shadow_scale` (default 4, 1 to 16) multiplies the all-light filter radius there for a subsurface look. Lava stays unlit through the replay. Translucent liquids and liquid surfaces on brush entities keep native lighting. `radiance_stats` reports `water_draws`.

The feature applies to the fast texture-filtered receiver path. With
`r_radiance_deferred 1`, supported opaque world and model surfaces share this
lighting at every supported scale, including full resolution. It evaluates
visibility on the scheduled receiver grid. Upsampling reuses the result without
further visibility queries. Incompatible fragment fallback and unsupported
rendering modes retain their native visibility. Probe transport retains its
original sampling. The optional pipeline is created
only when requested; normal startup does not compile the soft-shadow pass.
The original visibility helper still handles cutouts, liquids, sky boundaries
and direct-only ECS shadow casters.

This is a bounded surface-lighting approximation. The all-light Gaussian also
smooths attenuation and color gradients; it does not perform a separate blocker
search or physical penumbra estimate for every light. The optional contact-ray
pass retains its three-extra-ray budget. Full-resolution receivers and fragment
fallback can cost more than reduced receivers. `radiance_stats` reports requested and actually active
controls, including unsupported or unavailable optional-pipeline fallback.
The `shadow filter` line reports requested and actually recorded filter
strength and world radius; its active/inactive frame counters can detect a skipped pass during
a comparison. Selecting the cvar alone does not prove the filter ran.

The all-light implementation passed eight SDK synchronization-validated states
at odd 641×361, including full/half/quarter grids, checkerboarding, GI and
direct filtering together, disabled controls and reference-path fallback.
An exact E1M1 hallway comparison with bounce and emissive gains zero confirms
that the point-light triangular boundaries become broad fades at radius 8.

A clean three-pair RTX 4080 comparison at 1280×720, half scale, checkerboarding,
quality 0, bounce 0, point gain 1, emissive gain 0, soft source radius 5 and
Gaussian radius 8 measured median whole-frame GPU times of 0.9602 ms off and
1.4040 ms on. The median paired cost was 0.4419 ms (46.02%); measured wall FPS
medians were 910.4 and 657.2. This includes the final exact-triangle guide cache.
The final five-second idle grace and clean exit
were included outside the timing windows. This is a small scene-specific
comparison, not a native-widescreen performance guarantee. Earlier source-local
filter timings below describe superseded implementations.

## Earlier validation

The first all-forward prototype is rejected: its driver compilation delayed
startup. Its cross-binary image comparison also had different active light
counts and is invalid. A teardown NVVM diagnostic was also observed in the
immutable baseline; that diagnostic alone does not identify a regression in
the optional shader.
`build/vkquake` was restored to the validated two-thirds `aa8122c6` executable;
prototype builds remain separate until startup, visual and performance checks
pass. The restored executable reached active two-thirds E1M1 rendering in
18.76 seconds in an isolated startup check.

The isolated compute prototype `47a06cd2` subsequently passed default-off
pixel parity against that baseline and held off/on captures with actual-pass
frame counters. At 1080p E1M1, radius 2 keeps a narrower beam/pit penumbra and
avoids the low-contrast right-wall lobes seen with radius 4. Four visibility
samples can still show steps in broad penumbras. These captures establish
functionality, not a performance result.

A separate isolated RTX 4080 run of `47a06cd2` used five alternating 2-second
off/on pairs at 1920×1080, scale 2/3, quality 0, texture receivers, radius 2,
checkerboarding off, Gaussian 0, bounce 0, point/emissive gains 1 and
`r_lightmap 1`, at the held E1M1 doorway camera. Median whole-frame GPU time
was 1.738 ms off and 1.981 ms on; paired median overhead was 0.253 ms / 14.36%
(95% paired-bootstrap interval 12.54–20.66%). Wall-frame FPS medians were
529.5 and 465.1. All ten actual-pass counter windows passed, the renderer
exited cleanly, and final off parity differed only at two pixels by one code
value, within the explicit 1-code/32-pixel allowance. This measures that frozen
prototype and configuration; it is not a guarantee for other scenes, resolutions,
models or the subsequent MD5 material-cache changes.

The published `44cfcca0` build also passed 20 live SDK synchronization-validated
states across both tracing backends at odd 641×361, including checkerboarding,
quarter resolution, full/reference fallbacks and `r_indirect` transitions.
Four completed 1080p/two-thirds radius-2 timing pairs measured 1.713 ms off /
1.958 ms on and a paired median 0.244 ms / 14.29% overhead (95% interval
12.11–24.00%). A separate five-sample baseline-off block measured 1.722 ms;
these blocks were sequential, so their small difference is not causal evidence
of an off-mode speedup.

The requested fifth pair remains rejected and the original report remains
incomplete: its last off image differed at 2,237 pixels by one code value,
exceeding the explicit 32-pixel limit, despite matching sources and valid mode
windows. The exact cause of this small drift is unconfirmed. The four-pair
result must not be called a completed five-pair benchmark. Initial bounded
off parity passed, but the historical harness overwrote that first PNG; future
runs retain every image and gate separately. Native widescreen performance
remains unmeasured because Wayland changed the requested window sizes.

The release manifest and immutable binary are in
[`radiance-md5-soft-release-20261105`](../build/radiance-md5-soft-release-20261105/).

NVIDIA describes the three PCSS stages—blocker search, penumbra estimation and
visibility filtering—in its
[PCSS integration guide](https://developer.download.nvidia.com/assets/gamedev/docs/PCSS_Integration.pdf).
Finite-area ray sampling obtains contact hardening directly through the
different visibility of its emitting points.
