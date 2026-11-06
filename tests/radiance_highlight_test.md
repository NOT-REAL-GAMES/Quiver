# Radiance highlight GPU test

The fragment shader tests the same helper used by world and model receivers.
It checks low-light identity, threshold continuity, monotonicity, hue and the
post-contrast ceiling. The small raster test harness is shared with the
world-normal test; its success message refers to that original use.

From the project root (using a disposable build-local temporary directory):

```sh
mkdir -p build/inspector-build-tmp
glslangValidator -V --target-env vulkan1.1 -IShaders -o build/inspector-build-tmp/hl.vert.spv tests/radiance_world_normal.vert
glslangValidator -V --target-env vulkan1.1 -IShaders -o build/inspector-build-tmp/hl.frag.spv tests/radiance_highlight.frag
TMPDIR="$PWD/build" cc -std=c11 -Wall -Wextra -Werror tests/radiance_world_normal_test.c -lvulkan -lm -o build/inspector-build-tmp/hl-test
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json build/inspector-build-tmp/hl-test build/inspector-build-tmp/hl.vert.spv build/inspector-build-tmp/hl.frag.spv
```

This isolated numerical test does not validate the full renderer's gamma,
fog, transparency, fullbright behavior, image quality or performance.

For an end-to-end capture with registered game data and a Vulkan display:

```sh
TMPDIR="$PWD/build" python3 tests/smoke_radiance_highlight.py --binary "$PWD/build/vkquake" --basedir /path/to/quake
```

Use a debug build with `--validation` to require the validation layer. The test
runs at point gain 10 in a fixed, paused E1M1 view, captures the same scene with
the shoulder off and on, checks that clipped-channel pixels reduce, and checks
that classic and debug-fullbright captures do not change with the toggle. It
writes all captures to a disposable build-local directory. It does not compare
other maps, color settings, transparent geometry or a hardware GPU.
