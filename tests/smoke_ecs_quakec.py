#!/usr/bin/env python3
"""Run an isolated stock-ABI QuakeC mutation probe on a dedicated server.

Requires fteqcc, a vanilla-compatible id1 QuakeC source tree, and Quake PAKs.
For example the cleaned GPL source from https://github.com/shpuld/id1-quakec-cleaned
can be passed via --quakec-source. Nothing in the source tree or game data is
modified; all compiler, map and server output lives in a temporary directory.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from qbsp_smoke import brush


def run(cmd, cwd, timeout=60):
    result = subprocess.run([str(arg) for arg in cmd], cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{cmd[0]} exited {result.returncode}:\n{result.stdout}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--quakec-source', type=Path, required=True)
    args = parser.parse_args()
    binary, compiler, source = args.binary.resolve(), args.compiler.resolve(), args.quakec_source.resolve()
    pakdir = args.basedir.resolve()
    if (pakdir / 'id1').is_dir():
        pakdir /= 'id1'
    paks = sorted(p for p in pakdir.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs found in {pakdir}')
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='quiver-ecs-qc-') as tmp:
        work = Path(tmp)
        id1 = work / 'id1'
        id1.mkdir()
        game = work / 'qcprobe'
        (game / 'maps').mkdir(parents=True)
        (work / 'user').mkdir()
        for pak in paks:
            (id1 / pak.name.lower()).symlink_to(pak)
        qc = game / 'qc'
        shutil.copytree(source, qc, ignore=shutil.ignore_patterns('.git', '*.dat'))
        defs, world, src = qc / 'defs.qc', qc / 'world.qc', qc / 'progs.src'
        # The test fixture uses the unmodified stock ABI, adding only a custom
        # field after the engine's system definitions.
        with defs.open('a') as stream:
            stream.write('\nvoid() ecs_probe_frame;\n')
        text = world.read_text()
        pattern = r'(void\s*\(\s*\)\s*StartFrame\s*=\s*\{)'
        text, count = re.subn(pattern, r'\1\n    ecs_probe_frame();', text, count=1)
        if count != 1:
            raise RuntimeError('could not find the QuakeC StartFrame function')
        world.write_text(text)
        text = src.read_text()
        lines = text.splitlines()
        lines[0] = '../progs.dat'
        src.write_text('\n'.join(lines) + '\necs_probe.qc\n')
        shutil.copyfile(root / 'tests/fixtures/ecs/physics_probe.qc', qc / 'ecs_probe.qc')
        run([compiler], qc)
        wad = root / 'Misc/editor_defaults.wad'
        walls = [(-144,-144,-16,144,144,0), (-144,-144,128,144,144,144),
                 (-144,-144,0,-128,144,128), (128,-144,0,144,144,128),
                 (-128,-144,0,128,-128,128), (-128,128,0,128,144,128)]
        text = '{\n"classname" "worldspawn"\n"wad" "' + str(wad) + '"\n'
        text += ''.join(brush(*wall) for wall in walls) + '}\n'
        text += '{\n"classname" "info_player_start"\n"origin" "-80 0 32"\n}\n'
        mapfile = game / 'maps/ecs_probe.map'
        mapfile.write_text(text)
        run([binary, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest',
             mapfile, mapfile.with_suffix('.bsp')], work)
        output = run([binary, '-dedicated', '1', '-basedir', '.', '-userdir', 'user',
                      '-nosound', '-nojoy', '-game', 'qcprobe', '+developer', '1', '+map', 'ecs_probe', '+quit'], work)
        if 'ECS_PROBE_FAIL' in output:
            raise RuntimeError('QuakeC probe reported failure:\n' + output)
        for marker in ('ECS_PROBE_MUTATION_SAME_FRAME', 'ECS_PROBE_REUSED_SAME_FRAME', 'ECS_PROBE_NEW_NEXT_FRAME'):
            if marker not in output:
                raise RuntimeError(f'missing {marker}:\n{output}')
        # The alternate physics_mode 1 omits StartFrame and scans through a
        # dynamically growing edict count, including new slots in the same tick.
        defs.write_text(defs.read_text().replace('void() ecs_probe_frame;', 'void() ecs_mode1_setup;'))
        text = world.read_text().replace('\n    ecs_probe_frame();', '')
        pattern = r'(void\s*\(\s*\)\s*worldspawn\s*=\s*\{)'
        text, count = re.subn(pattern, r'\1\n    ecs_mode1_setup();', text, count=1)
        if count != 1:
            raise RuntimeError('could not find the QuakeC worldspawn function')
        world.write_text(text)
        shutil.copyfile(root / 'tests/fixtures/ecs/physics_mode1_probe.qc', qc / 'ecs_probe.qc')
        run([compiler], qc)
        output = run([binary, '-dedicated', '1', '-basedir', '.', '-userdir', 'user',
                      '-nosound', '-nojoy', '-game', 'qcprobe', '+developer', '1',
                      '+map', 'ecs_probe', '+quit'], work)
        if 'ECS_PROBE_FAIL' in output or 'ECS_PROBE_MODE1_SAME_FRAME' not in output:
            raise RuntimeError('QuakeC think-only physics probe failed:\n' + output)
        print('PASS: QuakeC fields, callback mutation, retained slots, ordered spawn/free and physics_mode 1')


if __name__ == '__main__':
    main()
