# Quiver / vkQuake MD5 exporter

A Blender 4.2+ exporter for vkQuake's Quake rerelease variant of MD5 version 10. Version 0.4 exports meshes and skins with automatic material baking, temporary geometry cleanup, and bake UV generation, plus optional rest skeletons and skin weights. Animation clips are a later step.

## Install

Build the extension archive from the repository root:

```sh
python3 Tools/blender/package_addon.py
```

In Blender, open **Edit → Preferences → Add-ons**, use the dropdown menu's **Install from Disk**, and select `/tmp/quiver-md5-exporter.zip`. Enable **Quiver / vkQuake MD5** if necessary. The extension archive contains `blender_manifest.toml` and the add-on's Python files at its root.

For traditional add-on installation, build a ZIP containing the `io_export_quiver_md5/` package:

```sh
python3 Tools/blender/package_addon.py --legacy --output /tmp/quiver-md5-exporter-legacy.zip
```

Install that archive through the same **Install from Disk** command. Use either installation, not both. Packaging uses Python's standard library and does not require Blender.

## Export a model

1. Model your mesh and assign materials. If you have existing texture UVs, set the intended map as the render UV map (camera icon). The exporter can generate separate bake UVs without editing this source map.
2. Use a **Principled BSDF** material. Image textures, constant colors, and procedural color networks are supported. Leave **Material Baking → Automatic** enabled to bake channels that need evaluation during export.
3. Select the mesh objects to include.
4. Choose **File → Export → Quiver / vkQuake MD5**.
5. Set **Game Model Directory** to the model's eventual directory inside your Quake game directory, for example `progs` or `progs/props`.
6. Export into that directory, or copy all resulting files there together.

For a model exported as `crate.md5mesh` with texture prefix `progs/props`, a single surface produces files like:

```text
my_mod/
└── progs/
    └── props/
        ├── crate.md5mesh
        ├── crate.md5anim
        └── <surface-stem>_00_00.png
```

Each MD5 mesh surface's `shader` contains `progs/props/<surface-stem>`. Textures are written beside the exported mesh; the prefix changes the in-game reference, not the disk export location. Therefore the prefix must match where you install the files. A material's shader-name override supplies a filename stem only; leave out directories, extensions, and `_00_00` suffixes.

The companion `.md5anim` contains one bind frame and must be installed with the mesh even for a static prop. There are no animation clips in this version. Use frame `0` for static models.

The exporter also keeps a hidden `.crate.md5mesh.quiver.json` ownership file beside the bundle. Keep it in your working export directory: subsequent exports use it to remove obsolete generated PNG skins and fullbrights, including after changing texture stems. Unrelated images are preserved. Texture stems must be unique across exported models in the same directory; ownership conflicts are reported before writing. This file is not needed by the engine and does not need to ship with the model. Output files are staged first, and existing files are restored if publishing fails.

## Skins and fullbright textures

By default, the exporter copies directly usable Principled base-color images and emission images, and converts constant colors to flat-color skins. It automatically bakes materials that use procedural colors, Mapping nodes, alternate source UV maps, or linked opacity/emission strength. PNGs support packed and generated source images. Principled Alpha determines diffuse coverage independently of Base Color's alpha channel.

For explicit control, use the material's **Quiver MD5 Skins** panel. Add skins in index order, starting with skin `0`, and assign their diffuse and optional fullbright images. An explicit list overrides automatic material-node discovery. Every exported surface must have the same number of skins, with no gaps, so QuakeC's `self.skin` selects a consistent variant across the model.

A mesh can use several material slots. Each used slot exports as a separate surface, and temporary bake materials preserve every face's original slot assignment, including meshes with unused slots.

vkQuake loads textures using these names:

| Purpose | Filename |
| --- | --- |
| Skin 0 | `<surface-stem>_00_00.png` |
| Skin 1 | `<surface-stem>_01_00.png` |
| Skin 2 | `<surface-stem>_02_00.png` |
| Skin 0 fullbright | `<surface-stem>_00_00_glow.png` |

The first number selects the skin; the second is the texture-animation frame and is `00` in this exporter. MD5 does not use MD3-style `.skin` mapping files. Fullbright images are separate glow masks, not PBR emission shaders. Roughness, metallic, and normal maps are not exported; procedural color networks are flattened to PNGs. If texture copying is disabled, supply the correctly named textures yourself.

## Automatic material baking

The export dialog's **Material Baking** controls offer:

- **Automatic** (default): preserve directly usable image textures and constant colors; bake other supported material channels as needed.
- **Always Bake**: bake every node material without an explicit MD5 skin list, including directly usable textures, at the chosen resolution.
- **Off**: require directly usable image textures and constant colors, with a clear error when a channel needs baking.

**Bake Resolution** sets each baked skin's square dimensions (default 2048 pixels; use 4096 for more detail). **Bake Margin** pads UV islands (default 8 pixels), and **Bake Samples** controls Cycles sampling (default 16). Each material gets its own texture rather than sharing the entire object's texture space. Enable **Export PNG Skins** for baking. Cycles must be available; baking uses the CPU independently of your configured GPU renderer.

**Bake UVs → Automatic** (default) reuses an existing valid bake layout. Otherwise it unwraps only the material's faces on a temporary copy. If Smart Project still overlaps, it tries separate-face Lightmap packing and a disjoint face atlas as a final fallback. Every resulting layout is validated. The baked MD5 surface uses those new UVs, while material inputs continue sampling the original render UVs. This preserves repeating textures and Mapping nodes. **Use Existing** keeps strict validation and requires a non-overlapping layout inside 0–1. Larger textures improve detail; non-overlapping UVs are still required at any resolution.

Baking exports unlit Base Color, Alpha coverage, and Emission Color multiplied by Emission Strength. It excludes scene lighting, shadows, metallic reflections, and the display view transform, allowing vkQuake to light the model. Nonzero emission becomes an additive `_glow.png` texture; HDR emission above the PNG range is clamped. A direct Emission surface exports black diffuse plus emission in the glow map. Upstream color nodes and color-output groups are supported; final Mix/Add shader closures and shader-output groups are not converted to a Quake material. Identity-dependent Object Info Random inputs are rejected. Apply modifiers that generate or reassign materials before baking.

Procedural materials using Generated/Object coordinates can be baked even without an existing UV map. Materials that read UV coordinates still need their source map, and explicit MD5 skin-image lists require an existing UV map. Automatic generation affects only baked surfaces; directly copied skins retain their authored UVs. The generated layout is evaluated through the modifier stack; if a modifier duplicates UVs or changes topology incompatibly, apply that modifier on a working copy before exporting.

Baking keeps the full object's coordinate space and modifier evaluation so Generated/Object textures remain aligned. Temporary scenes, objects, materials, and images are removed after export or failure; your render settings, node trees, selection, poses, source UVs, and source images are preserved.

## Geometry, transforms, and optional rigging

- **Default static export:** exports the current evaluated mesh, including its current posed shape when modifiers are enabled, and binds it to a generated root joint. This is suitable for props and baked poses.
- **Selection:** exports selected mesh objects by default; disable selection filtering to include meshes in the current view layer. Export from Object Mode.
- **Modifiers:** enabled by default. Triangulation and UV seam splitting happen during export without changing the source mesh.
- **Cleanup:** zero-area faces are removed from temporary mesh copies before baking and export. The saved mesh stays unchanged, and the export report gives the number removed. Large surfaces are split automatically to fit vkQuake's 16-bit vertex indices; chunks keep the same skin textures.
- **Coordinates:** Blender `+Z` remains up. The default forward direction is Blender `-Y`, which becomes Quake `+X`. Scale `1` means one Blender unit equals one Quake game unit; use the export scale to fit your asset to the game.
- **Origin:** world origin becomes model origin, with object transforms baked in. Position the asset around world origin before exporting.
- **UVs:** uses the render UV layer for copied skins, and the selected or generated bake layout for baked skins. Automatic baking handles Mapping nodes and alternate source UV maps; copying an image directly requires Flat projection, Repeat extension, and the exported render UV map.
- **Rest rig:** enable rig export to preserve an armature's rest skeleton and vertex-group weights. All armature bones, including parent and control bones, are included in parent-first order. Mesh geometry is evaluated without armature deformation for this mode, and temporary evaluation changes are restored. The strongest four bone influences per vertex are normalized before writing.
- **Rig scope:** every mesh must share one enabled Armature modifier using vertex groups. Disable envelopes, Preserve Volume, masks, and multi-modifier blending. Bone parenting, unweighted vertices, reflected armatures, and nonuniform or sheared joint transforms are rejected. This exports a bind skeleton and a single matching bind frame. It does not export actions, NLA strips, constraints as runtime behavior, animated scale, or animation clips.

MD5 does not store authored normals. vkQuake reconstructs smooth normals from positions, so Blender's custom split normals, sharp-edge shading, and normal maps cannot be preserved by this format.

## Engine integration

Install the model, companion animation, and skins inside a game directory loaded by vkQuake. Use game-relative forward-slash paths in QuakeC:

```c
precache_model ("progs/props/crate.md5mesh");
// Later, on an entity:
setmodel (self, "progs/props/crate.md5mesh");
self.frame = 0;
self.skin = 0;
```

For a replacement of an existing `.mdl`, use the same basename and preserve the original model's expected animation-frame layout before using it for an animated actor. This version's one bind frame is primarily for static assets; it does not replace a monster's animation set. Standalone MD5 models do not carry Quake's legacy model trail/rotation flags.

The implementation follows the local engine's loader in `Quake/gl_model.c`: MD5 texture names append `_skin_frame`, the animation uses the mesh's basename, bind joints are absolute transforms, animation joints are parent-relative, and quaternions reconstruct a negative `w` component.

## Validation limits

The exporter targets conservative vkQuake compatibility:

| Item | Export limit |
| --- | --- |
| Bones | 256, including non-deform ancestors |
| Bone name | 31 UTF-8 bytes |
| Split vertices per surface | 65,535 |
| Bone influences per vertex | Strongest 4, normalized |
| Skins per surface | 32, contiguous and consistent across surfaces |
| Shader reference | 48 characters, reserving room for texture suffixes |

UV seams increase the split-vertex count. Oversized surfaces are chunked automatically. Missing source UVs, unsupported image/material setups, invalid paths, and incompatible rig data should be corrected using the export error message rather than relying on engine fallbacks.

## Python entry point and future format

Inside Blender, the exporter can also be called directly after importing the installed package:

```python
from io_export_quiver_md5 import exporter

exporter.export(
    context=bpy.context,
    filepath="/path/to/my_mod/progs/crate.md5mesh",
    selected_only=True,
    scale=1.0,
    forward_axis="NEG_Y",
    export_rig=False,
    apply_modifiers=True,
    texture_prefix="progs",
    copy_textures=True,
    bake_mode="AUTO",
    bake_size=2048,
    bake_margin=8,
    bake_samples=16,
    bake_uv_mode="AUTO",
)
```

Add `import bpy` to your script. Extension installations use Blender's extension namespace; obtain the `exporter` module through that installed package instead of the legacy import shown above.

The format-neutral structures in `model.py` are separate from Blender extraction and the MD5 writer in `md5.py`. Extend those structures for animation clips and richer material data, then add a writer for Quiver's own asset format while retaining MD5 as an interchange/export target.

## Verification

From the repository root:

```sh
python3 -m unittest discover -s tests -p test_blender_md5_format.py -v
blender --background --factory-startup --python-exit-code 1 --python tests/blender_md5_export_test.py
blender --background --factory-startup --python-exit-code 1 --python tests/blender_md5_bake_test.py
blender --background --factory-startup --python-exit-code 1 --python tests/blender_md5_uv_test.py
python3 Tools/blender/package_addon.py
blender --factory-startup --command extension validate /tmp/quiver-md5-exporter.zip
```

The integration tests create temporary scenes and verify exported geometry and bind transforms using an independent MD5 reader. No Quake assets are required. This version was exercised with Blender 5.4.0 Alpha; the minimum supported API target is Blender 4.2.
