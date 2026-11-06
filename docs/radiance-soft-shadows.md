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
An optional compute pass applies the three outer queries, so the additional
query count is at most three per receiver, independent of the number of lights.

`r_radiance_shadow_filter` adds a geometry-guided Gaussian to the resulting
visibility. It defaults to `1`, accepts fractional strengths from `0` to `2`,
and uses `0` to retain the original unfiltered soft-shadow result. Ray-sampled
contact hardening and this spatial filter can run together; the shadow filter
is also independent of the indirect-light `r_radiance_probe_gaussian` control.

The spatial filter compares only scheduled samples of the same selected light
on compatible geometry. It adjusts that light's visibility contribution while
preserving other additive lights, subtractive lights and local minimum lighting.
It uses a fixed nine-tap kernel, with checker holes reconstructed from up to
four filtered scheduled samples. It adds no visibility rays or render targets.
The existing texture-reconstruction dispatch performs the filter, including
when checkerboarding and indirect lighting are both off. GI smoothing still
uses its existing horizontal/vertical/copy passes.

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

This is a bounded approximation. Several similarly strong lights may retain
hard shadows, and broad penumbras can show the small sample count. Spot-cone
attenuation is unchanged: this feature filters visibility rather than cone
boundaries. Full-resolution receivers and fragment fallback can cost more
than reduced receivers. `radiance_stats` reports requested and actually active
controls, including unsupported or unavailable optional-pipeline fallback.
The `shadow filter` line reports requested and actually recorded filter
strength; its active/inactive frame counters can detect a skipped pass during
a comparison. Selecting the cvar alone does not prove the filter ran.

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
