"""Generate inert SPIR-V symbols for the renderer shader fault test."""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
names = re.findall(r'^DECLARE_SHADER_SPV\s*\(\s*(\w+)\s*\)', source, re.MULTILINE)
if not names or len(names) != len(set(names)):
    raise SystemExit('shader declarations missing or duplicated')
with pathlib.Path(sys.argv[2]).open('w') as output:
    for name in names:
        output.write(f'const unsigned char {name}_spv[4] = {{0}};\n')
        output.write(f'const int {name}_spv_size = 4;\n')
