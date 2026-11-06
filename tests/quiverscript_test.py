#!/usr/bin/env python3
"""Exercise script compilation through the actual bundled QuakeC compiler."""
import pathlib
import struct
import subprocess
import sys
import tempfile

compiler = pathlib.Path(sys.argv[1]).resolve()
qcc = pathlib.Path(sys.argv[2]).resolve()
sources = pathlib.Path(sys.argv[3]).resolve()
fixture = pathlib.Path(sys.argv[4]).resolve()

with tempfile.TemporaryDirectory(prefix='quiverscript-test-') as tmp:
    root = pathlib.Path(tmp)
    game = root / 'game'
    def build(script):
        return subprocess.run([compiler, 'build', '--script', script, '--game-dir', game,
                               '--qcc', qcc, '--qc-source', sources],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    result = build(fixture)
    assert result.returncode == 0, result.stdout
    progs = (game / 'progs.dat').read_bytes()
    assert struct.unpack_from('<ii', progs) == (6, 5927)
    assert b'quiver_ripple_tick\0' in progs
    generated = (game / 'quiverscript/generated.qc').read_text()
    assert 'quiver_ripple_tick = {' in generated
    assert 'setorigin' in generated and 'sqrt' in generated and 'sin' in generated
    features = root / 'features.qs'
    features.write_text('''
float helper(float value) { return value * 2; }
void quiver_ripple_tick(entity target, float tick_time) {
    vector position = target.origin;
    float count = 0;
    bool enabled = true;
    string label = "ripple";
    while (count < 2) { count = count + 1; }
    for (count = 0; count < 2; count = count + 1) { position_z = helper(count); }
    if (enabled && label != "") { position_z = helper(tick_time); }
    setorigin(target, position);
}
''')
    result = build(features)
    assert result.returncode == 0, result.stdout
    progs = (game / 'progs.dat').read_bytes()
    # A semantic failure must not replace the last usable package.
    broken = root / 'bad.qs'
    broken.write_text('void broken() {\n  float value = "bad";\n}\n')
    result = build(broken)
    assert result.returncode != 0, result.stdout
    assert str(broken) in result.stdout, result.stdout
    assert (game / 'progs.dat').read_bytes() == progs
    broken.write_text('void broken() {\n  float x = 2;\n')
    result = build(broken)
    assert result.returncode != 0 and 'unterminated function' in result.stdout, result.stdout
    assert (game / 'progs.dat').read_bytes() == progs
    broken.write_text('void broken() {\n  switch (1) {}\n}\n')
    result = build(broken)
    assert result.returncode != 0 and 'unsupported QuiverScript construct' in result.stdout, result.stdout
    assert (game / 'progs.dat').read_bytes() == progs
print('PASS: QuiverScript transpilation, QuakeC compilation, diagnostics and atomic package')
