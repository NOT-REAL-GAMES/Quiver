# QuiverScript

QuiverScript is a small C-style scripting language that is transpiled to
QuakeC and compiled together with bundled id1 1.01 game sources. Scripts run
in the existing server QuakeC VM. The current language is an initial subset,
not full C: it supports named functions, globals and locals, `float`, `bool`,
`vector`, `string`, and `entity` values, assignment, arithmetic and comparison
expressions, function calls, `if`/`else`, `while`, `for`, and `return`. Declare
loop variables before a `for` loop. `bool` is represented by a QuakeC float.
Vector members use QuakeC's `_x`, `_y`, `_z` spelling, such as `position_z`.
`sin`, `sqrt`, and standard id1 QuakeC builtins such as `setorigin` are
available. Arrays, structures, classes, and switch statements are not yet
supported. The FTEQCC compilation step checks expression and function types;
its diagnostics point back to the `.qs` source.

For maps with a `_quiver_script` property, `void quiver_map_start()` is called
once after a fresh map starts and `void quiver_map_tick(float tick_time)` runs
once per server physics tick. Both callbacks are optional. Loading a save
restores game state without running the start callback again.

The ripple example is `Misc/scripts/ecs_ripple.qs`. Each server tick the
engine calls `quiver_ripple_tick(entity target, float tick_time)` for each
stress entity. The callback receives the actual authoritative entity, and
`setorigin` updates it. The script must retain that signature for this demo.

A normal Meson build produces `build/quiverscript`, `build/fteqcc`, a staged
`build/id1-quakec` source directory, and `build/quiver_ripple/progs.dat`. GNU
Make on Unix stages corresponding files beside `Quake/vkquake`. No Python is
needed by the compiled script builder or at game runtime. To compile another
script into a separate game package, pass absolute tool and source paths:

```sh
build/quiverscript build --script "$PWD/Misc/scripts/ecs_ripple.qs" \
  --game-dir "$PWD/build/my_script_game" \
  --qcc "$PWD/build/fteqcc" --qc-source "$PWD/build/id1-quakec"
```

The generated `quiverscript/generated.qc` is retained in the package for
inspection. A failed transpilation or QuakeC compile does not replace an
existing `progs.dat`. To run the bundled demo from a standalone game directory,
copy `build/quiver_ripple` into the directory holding your `id1` folder, then
start vkQuake with `-game quiver_ripple +map ecs1024` (or `ecs32768` for the
larger workload). This package is separate
from and does not replace the active mod. Quake game assets remain required.

FTEQCC is vendored from fte-team/fteqw commit
`f09ac7cccd2d713ddf881d3f1324703082c2cfe4`; the id1 sources are from
id-Software/Quake-Tools commit `c0d1b91c74eb654365ac7755bc837e497caaca73`,
`qcc/v101qc`. Both are distributed under GPLv2; see the LICENSE in each
vendored directory. FTEQCC is built without zlib support because this workflow
compiles plain source files.
