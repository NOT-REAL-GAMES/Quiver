#!/usr/bin/env python3
"""Check player-slot identification without pointer ordering across objects."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

text = (Path(__file__).resolve().parents[1] / 'Quake/r_alias.c').read_text()
match = re.search(r'static qboolean R_IsClientPlayerEntity\s*\([^;]*?\)\s*\{', text)
assert match
pos, depth = match.end(), 1
while depth:
    if text[pos] == '{': depth += 1
    elif text[pos] == '}': depth -= 1
    pos += 1
function = text[match.start():pos]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int qboolean;
typedef struct { int field[16]; } entity_t;
static struct { entity_t *entities; int maxclients; } cl;
''' + function + r'''
int main(void) {
    entity_t slots[6]={0}, other={0};
    cl.entities=slots; cl.maxclients=3;
    assert(!R_IsClientPlayerEntity(NULL));
    assert(!R_IsClientPlayerEntity(&other));
    assert(!R_IsClientPlayerEntity(&slots[0]));
    for(int i=1;i<=3;i++) assert(R_IsClientPlayerEntity(&slots[i]));
    assert(!R_IsClientPlayerEntity(&slots[4]));
    assert(!R_IsClientPlayerEntity((const entity_t *)((const char *)&slots[1]+1)));
    cl.maxclients=0;
    assert(!R_IsClientPlayerEntity(&slots[1]));
    cl.entities=NULL; cl.maxclients=3;
    assert(!R_IsClientPlayerEntity(&slots[1]));
    puts("PASS: player slot identity and unrelated entities");
}
'''
with tempfile.TemporaryDirectory(prefix='quiver-radiance-player-') as tmp:
    src=Path(tmp)/'test.c'; exe=Path(tmp)/'test'; src.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
