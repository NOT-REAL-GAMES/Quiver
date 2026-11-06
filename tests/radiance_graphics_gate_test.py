#!/usr/bin/env python3
"""Exercise the real frame publication gate with no renderer or user data."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'Quake/radiance_graphics.inc').read_text()

def function(name):
    match = re.search(r'(?:bool|const rc_graphics_t \*)\s*' + name + r'\s*\([^;]*?\)\s*\{', source)
    assert match, name
    depth, pos = 1, match.end()
    while depth:
        if source[pos] == '{': depth += 1
        elif source[pos] == '}': depth -= 1
        pos += 1
    return source[match.start():pos]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef unsigned long VkDescriptorSet;
#define VK_NULL_HANDLE 0
typedef struct { bool ready; } rc_graphics_t;
static const rc_graphics_t *rc_graphics_frame;
static VkDescriptorSet rc_graphics_descriptor;
static bool runtime_active;
static bool R_RadianceActive(void) { return runtime_active; }
''' + '\n'.join(function(name) for name in ('RCGraphics_SetFrame', 'RCGraphics_Active', 'RCGraphics_Frame')) + r'''
int main(void) {
    rc_graphics_t ready={true}, unready={false};
    assert(!RCGraphics_Active() && !RCGraphics_Frame());
    assert(RCGraphics_SetFrame(&ready, 7));
    assert(!RCGraphics_Active());
    runtime_active=true;
    assert(RCGraphics_Active() && RCGraphics_Frame()==&ready);
    assert(!RCGraphics_SetFrame(&unready, 7));
    assert(!RCGraphics_Active() && !RCGraphics_Frame());
    assert(RCGraphics_SetFrame(&ready, 7));
    assert(!RCGraphics_SetFrame(&ready, 0));
    assert(!RCGraphics_Active());
    assert(RCGraphics_SetFrame(&ready, 7));
    runtime_active=false;
    assert(!RCGraphics_Active());
    assert(RCGraphics_SetFrame(NULL, 0));
    runtime_active=true;
    assert(!RCGraphics_Active());
    puts("PASS: radiance frame gate and stale-descriptor fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='quiver-radiance-graphics-') as tmp:
    path = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    path.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + ['-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
