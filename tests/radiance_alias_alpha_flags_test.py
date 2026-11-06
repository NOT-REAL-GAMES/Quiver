#!/usr/bin/env python3
"""Execute the real alias/MD5 UBO flag blocks with mocked draw inputs."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'Quake/r_alias.c').read_text()
types = []
blocks = []
for name in ('aliasubo_t', 'md5ubo_t'):
    match = re.search(r'typedef struct\s*\{[^}]+\}\s*' + name + r';', source)
    assert match, name
    types.append(match.group())
    start = source.index(name + '\t')
    start = source.index('ubo->flags =', start)
    end = source.index('VkDescriptorSet descriptor_sets[', start)
    blocks.append(source[start:end])

has_alpha = re.search(r'const bool has_alpha =[^;]+;', source).group()
pipeline_start = source.index('if (showtris == 0)', source.index('static void GL_DrawAliasFrame ('))
pipeline_end = source.index('const qboolean oit_pass', pipeline_start)
pipeline = source[pipeline_start:pipeline_end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum { PV_QUAKE1, PV_QUAKE3, PV_MD5, PV_MD5_8 };
enum { TEXPREF_ALPHAPIXELS=1, MODEL_PIPELINE_ALPHA_BLEND_BIT=2,
 MODEL_PIPELINE_ALPHA_TEST_BIT=1, MODEL_PIPELINE_SHOWTRIS=4,
 MODEL_PIPELINE_SHOWTRIS_DEPTH_TEST=5 };
typedef struct { unsigned flags; } gltexture_t;
typedef struct { int poseverttype, numjoints, numframes; } aliashdr_t;
typedef struct { int pose1,pose2; } lerpdata_t;
typedef struct { int player; } entity_t;
static struct { entity_t viewent; } cl;
static bool r_fullbright_cheatsafe,r_lightmap_cheatsafe;
static struct { float value; } r_fullbright;
static bool R_IsClientPlayerEntity(const entity_t *entity) { return entity->player; }
'''
body = '\n'.join(types)
for name, block in zip(('aliasubo_t', 'md5ubo_t'), blocks):
    body += f'''
static uint32_t flags_{name}(aliashdr_t *paliashdr,entity_t *e,bool transparent,
 float entity_alpha,bool editor,int showtris,bool emission,bool lit,bool alphatest) {{
 gltexture_t texture={{transparent?TEXPREF_ALPHAPIXELS:0}},*tx=&texture;
 gltexture_t *fb=emission?&texture:NULL;
 const float *editor_projection=editor?(const float *)&texture:NULL;
 const void *radiance=lit?(const void *)&texture:NULL;
 lerpdata_t lerpdata={{2,3}};
 (void)lerpdata;
 {name} data={{0}},*ubo=&data;
 {has_alpha}
 int pipeline_index;
 {pipeline}
 {block}
 if (!editor && showtris==0)
  assert(((pipeline_index & MODEL_PIPELINE_ALPHA_BLEND_BIT)!=0)==has_alpha);
 if (((ubo->flags&0x20)!=0)) assert(!editor && showtris==0 && has_alpha);
 assert(ubo->entalpha==entity_alpha);
'''
    if name == 'md5ubo_t':
        body += ' assert(ubo->joints_offsets[0]==14 && ubo->joints_offsets[1]==21 && ubo->joints_bind_offset==28);\n'
    body += ' return ubo->flags;\n}\n'

suffix = r'''
int main(void) {
 unsigned cases=0;
 for(int kind=PV_QUAKE1;kind<=PV_MD5_8;++kind)
 for(unsigned texture_alpha=0;texture_alpha<2;++texture_alpha)
 for(unsigned entity_alpha=0;entity_alpha<2;++entity_alpha)
 for(unsigned editor=0;editor<2;++editor)
 for(int showtris=0;showtris<3;++showtris)
 for(unsigned emission=0;emission<2;++emission)
 for(unsigned lit=0;lit<2;++lit)
 for(unsigned player=0;player<2;++player)
 for(unsigned viewmodel=0;viewmodel<2;++viewmodel)
 for(unsigned debug=0;debug<3;++debug)
 for(unsigned cutout=0;cutout<2;++cutout) {
  aliashdr_t header={kind,7,4}; entity_t ordinary={player}; cl.viewent.player=player;
  entity_t *entity=viewmodel?&cl.viewent:&ordinary;
  float alpha=entity_alpha?.5f:1.f;
  r_fullbright_cheatsafe=debug==1; r_lightmap_cheatsafe=debug==2; r_fullbright.value=1;
  uint32_t actual=kind<=PV_QUAKE3?
   flags_aliasubo_t(&header,entity,texture_alpha,alpha,editor,showtris,emission,lit,cutout):
   flags_md5ubo_t(&header,entity,texture_alpha,alpha,editor,showtris,emission,lit,cutout);
  uint32_t expected=emission | ((texture_alpha||entity_alpha)&&!editor&&!showtris?0x20:0)
   | (lit&&viewmodel?8:0) | (lit&&player?16:0) | (!editor&&debug?2:0)
   | (kind==PV_QUAKE3?4:0);
  assert(actual==expected); ++cases;
 }
 assert(cases==9216);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rc-alias-alpha-flags-') as directory:
    directory = Path(directory)
    code, binary = directory / 'flags.c', directory / 'flags'
    code.write_text(prefix + body + suffix)
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    flags = shlex.split(os.environ.get('RC_TEST_CFLAGS', '-O2'))
    subprocess.run([*compiler, '-std=c99', '-Wall', '-Wextra', '-Werror', *flags,
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('alias/MD3/MD5/MD5_8 host alpha flag: 9,216 cases passed')
