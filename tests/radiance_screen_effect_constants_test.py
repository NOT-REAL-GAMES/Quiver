#!/usr/bin/env python3
"""Exercise the screen-effects constants when HDR forces the pass on."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Quake/r_passes.c').read_text()
struct_start = source.index('typedef struct screen_effect_constants_s')
struct_end = source.index('} screen_effect_constants_t;', struct_start) + len('} screen_effect_constants_t;')
init_start = source.index('const screen_effect_constants_t push_constants = {')
init_end = source.index('};', init_start) + 2
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include "r_passes.h"
''' + source[struct_start:struct_end] + r'''
static screen_effect_constants_t build(end_rendering_parms_t *parms) {
    uint32_t screen_effect_flags = 32;
''' + source[init_start:init_end] + r'''
    return push_constants;
}
int main(void) {
    end_rendering_parms_t parms = {0};
    parms.render_width = parms.vid_width = 640;
    parms.render_height = parms.vid_height = 360;
    parms.hdr_tonemap = true;
    parms.v_blend[0] = 255;
    parms.v_blend[3] = 128;
    parms.polyblend = false;
    screen_effect_constants_t c = build(&parms);
    assert(c.flags == 32 && c.poly_blend_a == 0.0f && c.poly_blend_r == 1.0f);
    parms.polyblend = true;
    c = build(&parms);
    assert(c.poly_blend_a > 0.50f && c.poly_blend_a < 0.51f);
    parms.v_blend[3] = 0;
    assert(build(&parms).poly_blend_a == 0.0f);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='radiance-screen-effects-') as tmp:
    test = Path(tmp) / 'test.c'
    test.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'Quake'), str(test), '-o', str(Path(tmp) / 'test')], check=True)
    subprocess.run([str(Path(tmp) / 'test')], check=True)
print('PASS: screen effects honor disabled polyblend during HDR tone mapping')
