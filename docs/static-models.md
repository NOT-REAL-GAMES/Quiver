# Compiled static models

Put the model and its textures in the active game's asset directory, for example
`quiver_ripple/progs/sponza.md5mesh`. MD5 exports also need their companion
single-frame `.md5anim`. The compiler accepts single-frame MDL, MD3 and MD5
models with retained geometry; animated models are rejected.

In the map editor, open the Properties inspector with **I**, enter the
game-relative asset path under **Place static model**, then choose
**Add model ahead of view**. The console equivalent is:

```text
editor_model progs/sponza.md5mesh
```

Select the model marker to edit its path, origin, pitch/yaw/roll, positive scale,
skin and **Collision** checkbox. Collision defaults to enabled. Models support
move, duplicate, delete, undo and redo in both brush and sector documents.
The source view shows the textured model in perspective, top, front and right
views. Neutral directional shading makes its shape visible while editing;
playtest lighting still comes from the map. Perspective meshes share depth
with source faces, and orthographic source wires remain overlaid. The bounds,
origin marker and asset label remain available for selection and dragging.
Unavailable meshes keep their placement marker and asset label.

Save with `editor_save <name>` and use **F9** or `editor_play` to compile and
play. Model placements remain editable point entities in the source document.
The compiler removes those entities from its temporary QuakeC entity snapshot,
then appends placement records and exact transformed collision triangles
after QBSP, VIS and LIGHT. No special QuakeC spawn function is required.
Scripted playtests inherit the original game's asset directories.

The output retains the existing BSP29 or BSP2 header and adds the
`Q_STATIC_MODELS` BSPX lump, version **1**. It does not assign Quake version 30,
which the engine already uses for Valve BSP. Unrelated BSPX data is preserved.
Keep a sealed world sector/brush enclosure around imported architecture:
the model extension supplies mesh collision and runtime shadows, not QBSP
sealing or precomputed visibility. Model assets and skins remain referenced
game resources and must ship with the compiled map.

Compiled placements use the exact file format named by their path. An explicit
`.mdl` placement therefore stays MDL when the enhanced-model preference changes.
Skin and geometry animation are fixed at frame 0. Positive float scale
and rotation use the same transform for rendering, bounds, lighting and baked
collision. Export changes require refreshing/reloading the model assets and
recompiling the map; model maps always run a fresh editor compilation.

With collision enabled, server physics sweeps the player's box or projectile
against the actual triangles. It works with classic lighting, Radiance, and
dedicated servers. Mesh surfaces are two-sided and do not require convex brush
proxies; turning collision off preserves the model and its shadows. The
Collision setting applies to the whole placement.

All compiled model geometry enters the immutable world shadow batch once,
outside the ordinary 256 dynamic-occluder limit. Projectile creation or removal
only rebuilds the small mutable suffix. Compiled models keep the normal alias
rendering/material path; this change batches their shadow geometry rather than
claiming one raster draw for every material.

Use `static_models_stats` to inspect instance/triangle counts, collision flags,
client/baked bounds agreement and the active Radiance scene's actual compiled
triangle count.

The version 1 wire format is explicitly little endian: a 16-byte header contains
version, instance count, triangle count and record size. Each 300-byte instance
stores a 256-byte relative model path, float origin/angles/scale, skin, flags and
a contiguous triangle range. Each 36-byte triangle stores three world-space
float positions. Readers validate bounds, counts, paths and finite values
before publishing; collision BVHs are runtime acceleration data and are not
serialized.
