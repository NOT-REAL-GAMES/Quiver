# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Quiver is a modern Vulkan-based port of id Software's Quake engine, derived from vkQuake/QuakeSpasm. The project aims to evolve into a C++20 game engine based on Valve's original FPS concept that became Half-Life. Future goals include replacing BSP with kd-tree visualization and implementing SDF voxel-based global illumination.

**Current state:** C11 codebase with Vulkan rendering, ray tracing support, and multithreaded architecture.

## Build Commands

### Linux (Primary - Meson)
```bash
# Install dependencies (Ubuntu)
apt-get install git meson gcc glslang-tools spirv-tools libsdl2-dev libvulkan-dev libvorbis-dev libmpg123-dev libx11-xcb-dev

# Build
meson build && ninja -C build

# Debug build
meson build --buildtype=debug && ninja -C build
```

### Linux (Alternative - Make)
```bash
cd Quake && make -j
```

### Windows (Visual Studio)
Open `Windows/VisualStudio/vkquake.sln` and build.

### Windows (MinGW)
```bash
cd Quake
make -f Makefile.w64    # x64
make -f Makefile.w64a   # ARM64
```

### macOS
```bash
brew install molten-vk vulkan-headers glslang spirv-tools sdl2 libvorbis flac opus opusfile flac mpg123 meson pkgconfig
meson build && ninja -C build
```

### Running
The built executable is `vkquake`. It requires Quake game data files in lowercase (e.g., `id1/pak0.pak`).

## Code Formatting

Run clang-format before commits. CI enforces formatting via `.github/workflows/clang-format-check.yml`.

Key style rules (`.clang-format`):
- Allman brace style
- Tabs for indentation (width 4)
- 160 column limit
- Pointer alignment: right (`char *ptr`)
- Space before parentheses

## Architecture Overview

### Source Organization
- `Quake/` - Main engine source (~138k LOC C code)
- `Shaders/` - GLSL 4.6 shaders compiled to SPIR-V
- `Windows/` - Windows-specific files and dependencies

### Rendering Pipeline (Vulkan)

**Primary Command Buffer Contexts** (`glquake.h:184-190`):
1. `PCBX_BUILD_ACCELERATION_STRUCTURES` - Ray tracing setup
2. `PCBX_UPDATE_LIGHTMAPS` - GPU lightmap computation
3. `PCBX_UPDATE_WARP` - Water/lava texture effects
4. `PCBX_RENDER_PASSES` - Main frame rendering

**Secondary Contexts** for parallel submission:
- `SCBX_WORLD` (6 instances) - BSP world geometry
- `SCBX_ENTITIES` (6 instances) - Model rendering
- `SCBX_SKY`, `SCBX_WATER`, `SCBX_PARTICLES`, `SCBX_VIEW_MODEL`, `SCBX_GUI`, `SCBX_POST_PROCESS`

**Pipeline counts:**
- 16 world pipelines (material variants)
- 6 model pipelines (MDL/MD3/MD5 with alpha modes)

### BSP and Visibility System

Core structures in `gl_model.c` and `bspfile.h`:
- `qmodel_t` - Complete model (brush, sprite, alias)
- `mnode_t/mleaf_t` - BSP tree nodes and leaves
- `msurface_t` - Surface with lightmap and texture chains
- PVS (Potentially Visible Set) for leaf-based culling

Surface flags (`SURF_DRAW*`) control rendering paths for opaque, transparent, sky, and fence surfaces.

### Multithreading (`tasks.c/h`)

Task system with work queues supporting up to 32 worker threads:
- Task dependency tracking
- CPU core affinity support
- SIMD-accelerated surface marking (SSE2/NEON)

Parallel phases: acceleration structure builds, lightmap updates, surface marking, render pass submission.

### Ray Tracing

Enabled via `r_rtshadows` cvar. Requires VK_KHR_ray_query extension.
- TLAS/BLAS acceleration structures per `qmodel_t`
- Compute shaders: `update_lightmap_*_rt.comp` for ray-traced lighting
- `ray_debug.comp` for visualization

### Model Formats

| Format | Use | Key Files |
|--------|-----|-----------|
| MDL | Original Quake | `r_alias.c` |
| MD5 | 2021 Re-release skeletal | `md5.vert`, joint buffers |
| MD3 | Quake 3 style | MD3 loader in `gl_model.c` |

All support true color skins (PNG/TGA/JPG) with fullbright emission.

### Shader Compilation Pipeline

1. GLSL 4.6 source in `Shaders/`
2. `glslangValidator` → SPIR-V
3. `spirv-opt` optimization (with `--canonicalize-ids` for glslang 16.0+)
4. `bintoc` embeds binary into C source

Shaders use push constants for MVP/fog/alpha and descriptor sets for textures/lightmaps.

### Key Configuration

**Compile-time flags** (`quakedef.h`):
- `PSET_SCRIPT` - FTE-style scriptable particles
- `BSP29_VALVE` - Half-Life map support
- `MAX_GLTEXTURES=4096`, `MAX_SANITY_LIGHTMAPS=256`

**Vulkan requirements:**
- SDK 1.2.162+ (Linux), 1.4.321.1+ (Windows)
- MoltenVK on macOS

## Key Files for Common Tasks

| Task | Files |
|------|-------|
| Rendering entry | `gl_rmain.c`, `gl_vidsdl.c` |
| BSP/world | `r_world.c`, `r_brush.c`, `gl_model.c` |
| Model rendering | `r_alias.c`, `gl_mesh.c` |
| Lightmaps | `gl_rlight.c`, `update_lightmap_*.comp` |
| Particles | `r_part.c`, `r_part_fte.c` |
| Task system | `tasks.c`, `tasks.h` |
| Memory | `gl_heap.c`, `mem.c` |
| Vulkan setup | `gl_vidsdl.c` |

## Dependencies

- **Graphics:** Vulkan SDK, SDL2
- **Audio:** libvorbis, libmpg123, libflac, libopus
- **Build:** meson (0.47+), glslangValidator, spirv-tools
