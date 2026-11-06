#!/usr/bin/env python3
"""Regenerate the namespace wrappers used by legacy Make and Visual Studio.

Meson generates equivalent translation units in its build directory. Keep the
tool source lists here in sync with embedded/meson.build.
"""
from pathlib import Path

COMMON = 'threads cmdlib log qvec mathlib polylib'.split()
QBSP = 'brush bspfile csg4 file globals map merge outside parser portals qbsp solidbsp surfaces tjunc util wad winding writebsp exportobj'.split()
VIS = 'flow soundpvs state vis'.split()
LIGHT = 'entities litfile ltface trace light phong bounce settings'.split()

root = Path(__file__).resolve().parent
out = root / 'wrappers'
out.mkdir(exist_ok=True)
expected = set()
for tool, items in (('qbsp', QBSP), ('vis', VIS), ('light', LIGHT)):
    sources = ['common/' + name for name in COMMON]
    if tool != 'qbsp':
        sources += ['common/bsputils', 'common/bspfile']
    if tool == 'light':
        sources += ['common/mesh']
    sources += [tool + '/' + name for name in items]
    for source in sources:
        name = tool + '_' + source.replace('/', '_') + '.cc'
        expected.add(name)
        prefix = '#define DOUBLEVEC_T\n' if tool == 'qbsp' else ''
        (out / name).write_text(prefix + '#include "../preinclude.hh"\nnamespace quiver_' + tool +
                                ' {\n#include "../../' + source + '.cc"\n}\n')
for stale in out.glob('*.cc'):
    if stale.name not in expected:
        stale.unlink()
