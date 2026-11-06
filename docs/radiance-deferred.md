# Shared opaque Radiance lighting

`r_radiance_deferred 1` enables shared screen-space lighting for opaque world
and model surfaces. It captures depth, world normals and draw/primitive IDs,
evaluates Radiance on the selected receiver grid, then applies the lighting
while drawing each material. MDL, MD3 and MD5 receivers reuse the exact native
vertex poses, joint buffers, skin and transform.

This experimental path defaults to `0`. Initial RTX 4080 comparisons did not
establish a frame-time improvement; the latest hidden-fragment optimization
still needs a clean timing comparison. Saved explicit settings are honored.

With texture filtering selected, the indirect producer uses the eight-corner
probe lookup and a separable, geometry-guided Gaussian on the lighting texture.
The material pass uses geometry-guided taps without repeating probe visibility
checks. Incompatible guides retain the original fragment-lighting fallback.
This extends the existing world light prepass; material albedo and fullbright
textures stay on the native material path.

The texture Gaussian is a screen-space approximation of the world-space probe
kernel. It can differ from the reference Gaussian, and strict geometry guides
can leave short filter segments on curved surfaces. Reference probe filtering
remains available for comparison.

## Controls

```text
r_radiance_deferred 1
r_radiance_receiver_filter 1
r_radiance_resolution_scale 0.666666667
```

Supported per-axis scales are `1`, `0.666666667`, `0.5`, `0.333333333` and
`0.25`. Full resolution now uses the same shared lighting path. Checkerboarding
is disabled effectively at scale 1; its requested cvar is preserved. At smaller
scales the existing checker reconstruction and indirect filtering apply.

`r_radiance_deferred 0` restores the earlier behavior: reduced receiver
lighting for supported world draws, and per-fragment lighting for models.
It provides a reference for comparisons with the same map and light settings.
`r_radiance_receiver_filter 0` keeps the reference probe-filtering path and
does not enable model receiver reuse.

Use `r_radiance_stats` to check the actual selected path. The `deferred opaque`
line reports eligible cached model draws/triangles and selected/fallback frame
counters. A requested mode alone does not prove the renderer selected it.
These draw counts describe command selection, not per-fragment reuse. Optional
receiver coverage diagnostics estimate reuse from prepass data and are not
counters inside the actual material fragment shader.

## Coverage and compatibility

Opaque model surfaces and eligible alpha cutouts share the receiver prepass.
Blended surfaces, the viewmodel, source-editor previews, debug/fullbright draws
and indirect ECS draws retain their existing lighting. Mixed model materials
keep their native order: a forward chain or foliage surface remains between
the same opaque surfaces in both the original and cached command streams.
Player minimum lighting, fullbright textures, alpha, fog and highlights retain
their existing composition. Shadow-caster eligibility is unchanged.

Receiver reconstruction captures its camera, viewport and render extent before
draw recording. The end-of-frame task uses that snapshot even when setup for
the next camera overlaps submission of the previous frame. This avoids using
a new camera matrix to reconstruct the previous frame's depth.

Native blended alias/MD5 fragments whose sampled alpha is exactly zero now
skip lighting and return zero contribution. The host enables this only for draws that do not write
depth; opaque, editor and wireframe coverage retain their previous behavior.
This optimization applies to both deferred 0 and deferred 1. Its benefit must be
reported separately from the incremental benefit of shared lighting.

The current path requires single-sample rendering and the direct world path
(`r_indirect 0`). Unsupported resources or failed capture, pipeline creation,
preflight or recording select the complete original draw stream.

The receiver images reserve full-resolution capacity so switching scales does
not recreate descriptors or targets. Across two frame slots, their nominal
image payload at 3440×1440 is about 907 MiB before driver alignment. Smaller
active grids dispatch and sample only their active extents.

## Measurement

Compare equal camera, map, lights, gains, quality, probe spacing, resolution and
checker settings. Warm both paths and verify the actual drawable size before
timing. `gpu_frame_time` measures complete GPU frame execution; presentation and
CPU work are separate. The Radiance `GPU prepass` counter covers scene/cascade
preparation and excludes the receiver and material passes.

Compare the same executable with deferred 0 and 1, and inspect actual selected
features as well as cvars. In particular, requested soft shadows were inactive
on the original full-resolution model path, while shared full-resolution
lighting can execute that pass. Disable soft shadows on both paths for a direct
cost comparison, or label the additional work in the result.

The `entities` GPU stage includes both the ordinary entity stream and the
selected opaque-model stream. Receiver classification includes its geometry
prepass, receiver shading includes any soft-shadow pass, and receiver filtering
includes Gaussian axes and checker reconstruction. Stage queries are diagnostic
and can perturb timing; use uninstrumented whole-frame comparisons for the
headline result.
