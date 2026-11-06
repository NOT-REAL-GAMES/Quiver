# Radiance visible-transparency smoke test

`smoke_radiance_transparency.py` copies E1M1 from the supplied game PAK to a
disposable directory and appends two runtime light entities to make a slime
pool visible. It runs the game with Radiance enabled at a fixed, paused camera
position. For each transparency mode (`r_oit` 0, 1 and 2), it captures opaque
and translucent slime and checks that changing opacity changes at least 500
scene pixels. It also compares the single translucent layer against ordinary
blending, allowing a small rounding tolerance. The saved captures show the
actual geometry used by the test.

Run with a display and game data, for example:

```sh
TMPDIR="$PWD/build" python3 tests/smoke_radiance_transparency.py \
  --binary build/vkquake --basedir /path/to/quake --backend software --validation
```

Use `--msaa 4` to repeat with four samples, or `--backend hardware` on a
ray-query-capable device. The test requires Pillow, a compiler for its input
shim, and the Vulkan validation layer when `--validation` is specified. The
MSAA variant checks that the renderer reports four samples. Validation output
is checked after shutdown.

Add `--hdr` to require an RGBA16F scene target and compare fullbright frames
with the highlight shoulder on and off, including the native-resolution HUD.
It also opens the real main menu and compares fullbright HDR, UNORM, and HDR
after toggling back, allowing a small tolerance for the animated menu cursor.
Use a debug engine build for `--validation`; release builds may omit the layer.

This fixture establishes that a visible translucent liquid is rendered and
that the three composition modes agree for one layer while Radiance is active.
It does not establish physically correct liquid lighting, overlapping
transparent surfaces, moving objects, HDR lighting accuracy, all menu screens, or performance.

For overlapping layers, run the separate generated brush-map fixture:

```sh
TMPDIR="$PWD/build" python3 tests/smoke_radiance_overlap.py \
  --binary build/vkquake --basedir /path/to/quake --backend software --validation --msaa 4
```

It compiles one sealed room and produces five maps by changing only the entity
lump: no panes, each pane alone, both panes, and the two panes with reversed
entity order. With the RGBA16F scene target and runtime Radiance lighting
active, it verifies that each translucent pane changes at least 500 of the same
screen pixels, that adding the other pane changes those pixels again, and that
OIT modes 1 and 2 are stable when the entities are reordered. The two panes
share a texture with different offsets. Bounce gain is zero for deterministic
capture; this is a composition test, not a bounce-lighting test. Entity
reordering exercises map and entity traversal order; it does not prove that a
renderer which sorts draws submits them in the opposite order. Use
`--backend hardware` to exercise ray queries where supported. All generated
maps and screenshots are retained in the printed disposable test directory.
