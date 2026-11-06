#!/usr/bin/env python3
"""Build the packaged ripple demo without changing a user's game directory."""
import pathlib
import subprocess
import sys

compiler, qcc, script, qc_sources, game, stamp = map(pathlib.Path, sys.argv[1:])
game.mkdir(parents=True, exist_ok=True)
command = [str(compiler.resolve()), 'build', '--script', str(script.resolve()),
           '--game-dir', str(game.resolve()), '--qcc', str(qcc.resolve()),
           '--qc-source', str(qc_sources.resolve())]
result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
if result.returncode:
    print(result.stdout, file=sys.stderr)
    sys.exit(result.returncode)
stamp.write_text('QuiverScript ripple package built\n')
