#!/usr/bin/env python3
"""Compile the renderer's real lazy-backend helper with controlled Vulkan stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'Quake/radiance_engine.c').read_text()
start = source.index('static bool RCFrame_EnsureGraphics (')
brace = source.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
helper = source[start:end]
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int VkDescriptorSetLayout;
typedef struct { bool ready; unsigned index; } rc_graphics_t;
typedef struct {
    rc_graphics_t graphics[2]; bool graphics_attempted[2];
    struct { VkDescriptorSetLayout software_layout, hardware_layout; } receiver;
    struct { bool trace_hardware; } cascades;
} radiance_engine_t;
static unsigned create_calls[2];
static unsigned ensure_calls[2];
static bool fail[2];
static bool variant_fail[2];
static bool RCGraphics_EnsureCurrentVariant(rc_graphics_t *g) {
    unsigned index = g->index;
    ++ensure_calls[index];
    return !variant_fail[index];
}
static bool RCGraphics_Create(rc_graphics_t *g, VkDescriptorSetLayout layout, bool hardware) {
    unsigned index = hardware ? 1 : 0;
    assert(layout);
    ++create_calls[index];
    if (fail[index]) return false;
    g->ready = true;
    g->index = index;
    return true;
}
''' + helper + r'''
int main(void) {
    radiance_engine_t e = {0};
    e.receiver.software_layout = e.receiver.hardware_layout = 1;
    e.cascades.trace_hardware = true;
    assert(RCFrame_EnsureGraphics(&e, false));
    assert(create_calls[0] == 1 && create_calls[1] == 0);
    assert(RCFrame_EnsureGraphics(&e, false) && create_calls[0] == 1);
    assert(ensure_calls[0] == 1);
    assert(RCFrame_EnsureGraphics(&e, true));
    assert(create_calls[0] == 1 && create_calls[1] == 1);
    assert(RCFrame_EnsureGraphics(&e, true) && create_calls[1] == 1);
    assert(ensure_calls[1] == 1);
    variant_fail[1] = true;
    assert(!RCFrame_EnsureGraphics(&e, true) && create_calls[1] == 1);
    variant_fail[1] = false;
    assert(RCFrame_EnsureGraphics(&e, true) && create_calls[1] == 1);
    radiance_engine_t failed = {0};
    failed.receiver.software_layout = failed.receiver.hardware_layout = 1;
    failed.cascades.trace_hardware = true;
    fail[1] = true;
    assert(!RCFrame_EnsureGraphics(&failed, true));
    assert(create_calls[1] == 2 && failed.graphics_attempted[1]);
    fail[1] = false;
    assert(!RCFrame_EnsureGraphics(&failed, true) && create_calls[1] == 2);
    assert(RCFrame_EnsureGraphics(&failed, false));
    assert(create_calls[0] == 2);
    radiance_engine_t unsupported = {0};
    unsupported.receiver.hardware_layout = 1;
    assert(!RCFrame_EnsureGraphics(&unsupported, true) && create_calls[1] == 2);
    assert(!RCFrame_EnsureGraphics(&unsupported, true) && create_calls[1] == 2);
    puts("PASS: lazy backend creation, reuse, fallback and failure suppression");
}
'''
with tempfile.TemporaryDirectory(prefix='radiance-backend-test-') as tmp:
    code = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    code.write_text(test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(code), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
