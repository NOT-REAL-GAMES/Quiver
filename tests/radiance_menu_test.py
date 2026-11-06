#!/usr/bin/env python3
"""Exercise the real graphics menu control logic in an isolated C harness.

The small extraction keeps renderer/window dependencies out of the test and
never reads or writes the user's config. It verifies behavior, not menu pixels.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
menu = (ROOT / 'Quake/menu.c').read_text()
runtime = (ROOT / 'Quake/radiance_runtime.c').read_text()


def function(source, name):
    match = re.search(r'(?:static\s+)?(?:void|int|bool|const char\s*\*)\s*' + re.escape(name) + r'\s*\([^;]*?\)\s*\{', source)
    if not match:
        raise AssertionError(f'missing function: {name}')
    depth = 1
    pos = match.end()
    while depth:
        if source[pos] == '{': depth += 1
        elif source[pos] == '}': depth -= 1
        pos += 1
    return source[match.start():pos]


start = menu.index('enum\n{\n\tGRAPHICS_OPT_GAMMA')
end = menu.index('};', start) + 2
enum_source = menu[start:end]
radiance_decl = re.search(r'^cvar_t r_radiance = \{[^\n]+;', runtime, re.M)
assert radiance_decl, 'r_radiance declaration missing'

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef int qboolean;
#define CVAR_ARCHIVE 1
#define CLAMP(minimum, x, maximum) ((x)<(minimum)?(minimum):((x)>(maximum)?(maximum):(x)))
#define MENU_SLIDER_START 0
#define MENU_SLIDER_END 100
#define MIN_FPS_MENU_VALUE 30
#define MAX_FPS_MENU_VALUE 1000
#define FPS_MENU_VALUE_STEP 10
#define PREVIEW_UNDERWATER 1
#define PREVIEW_WORLD 2
typedef struct {const char *name, *string; int flags; float value;} cvar_t;
''' + radiance_decl.group() + r'''
static cvar_t vid_palettize, vid_filter, scr_guifilter, host_maxfps, vid_fsaamode, vid_anisotropic;
static cvar_t r_waterwarp, r_oit, r_enhancedmodels, r_lerpmodels, r_lerpmove, r_lerpturn;
static cvar_t r_softparticles, r_rtshadows, r_ssao;
static struct {float value;} vid_gamma, vid_contrast, scr_fov;
static struct {bool ray_query, screen_effects_sops, ssao_broken; struct {bool sampleRateShading;} device_features;} vulkan_globals;
static struct {bool active;} radiance_stats;
static int graphics_options_cursor, m_mouse_x, slider_grab;
static cvar_t *last_changed;
static void Cvar_SetValueQuick(cvar_t *v, float x) { v->value=x; last_changed=v; }
static void Cvar_SetValue(const char *name, float value) {(void)name; (void)value;}
static void S_LocalSound(const char *name) {(void)name;}
static void Cbuf_AddText(const char *text) {(void)text;}
static void M_BeginPreview(int cursor, int preview) {(void)cursor; (void)preview;}
static void M_GraphicsOptions_ChooseNextAASamples(int dir) {(void)dir;}
static void M_GraphicsOptions_ChooseNextParticles(int dir) {(void)dir;}
static float M_GetSliderPos(float a,float b,float c,int d,int e,float f,int g,float h,float i)
{(void)a;(void)b;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;return c;}
''' + enum_source + '\n' + function(menu, 'M_GraphicsOptions_NumItems') + '\n' + function(menu, 'M_GraphicsOptions_AdjustSliders') + '\n' + function(runtime, 'R_RadianceActive') + '\n' + function(runtime, 'R_RadianceMenuStatus') + r'''
int main(void)
{
    assert(r_radiance.flags & CVAR_ARCHIVE);
    assert(strcmp(r_radiance.name,"r_radiance")==0 && strcmp(r_radiance.string,"0")==0);
    for (int shadows = 0; shadows != 2; ++shadows)
    for (int ao = 0; ao != 3; ++ao) {
        vulkan_globals.ray_query = shadows;
        vulkan_globals.screen_effects_sops = ao != 0;
        vulkan_globals.ssao_broken = ao == 2;
        bool has_ao = ao == 1;
        assert(M_GraphicsOptions_NumItems() == GRAPHICS_OPTIONS_ITEMS - !shadows - !has_ao);
        graphics_options_cursor=GRAPHICS_OPT_RADIANCE;
        assert(graphics_options_cursor < M_GraphicsOptions_NumItems());
        r_radiance.value=1; radiance_stats.active=false; last_changed=NULL;
        assert(strcmp(R_RadianceMenuStatus(), "Unavailable")==0);
        M_GraphicsOptions_AdjustSliders(1,0);
        assert(last_changed==&r_radiance && r_radiance.value==0 && strcmp(R_RadianceMenuStatus(),"Off")==0);
        M_GraphicsOptions_AdjustSliders(-1,0);
        assert(last_changed==&r_radiance && r_radiance.value==1 && strcmp(R_RadianceMenuStatus(),"Unavailable")==0);
        radiance_stats.active=true;
        assert(R_RadianceActive() && strcmp(R_RadianceMenuStatus(),"On")==0);
        r_radiance.value=0;
        assert(!R_RadianceActive() && strcmp(R_RadianceMenuStatus(),"Off")==0);
        graphics_options_cursor=GRAPHICS_OPT_RADIANCE-1; last_changed=NULL;
        M_GraphicsOptions_AdjustSliders(1,0); assert(last_changed==&r_softparticles);
        if (shadows) {
            graphics_options_cursor=GRAPHICS_OPT_SHADOWS; last_changed=NULL;
            M_GraphicsOptions_AdjustSliders(1,0); assert(last_changed==&r_rtshadows);
        }
        if (has_ao) {
            graphics_options_cursor=GRAPHICS_OPT_AMBIENT_OCCLUSION-!shadows; last_changed=NULL;
            assert(graphics_options_cursor < M_GraphicsOptions_NumItems());
            M_GraphicsOptions_AdjustSliders(1,0); assert(last_changed==&r_ssao);
        }
    }
    puts("PASS: radiance menu toggle, archived default, status and optional-row indexing");
}
'''
with tempfile.TemporaryDirectory(prefix='quiver-radiance-menu-') as tmp:
    source = Path(tmp) / 'menu_test.c'
    binary = Path(tmp) / 'menu_test'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', str(source), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
