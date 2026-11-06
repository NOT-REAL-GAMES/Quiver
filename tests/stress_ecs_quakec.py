#!/usr/bin/env python3
"""Stress real server ECS dispatch with per-edict QuakeC boolean toggles.

Runs separate generated maps on a dedicated server without clients. The normal
fixture flag is an edict-backed QuakeC float. With --native-components and an
instrumented build it also flips and verifies an ECS-owned bool per edict.
Requires local Quake PAKs, a compatible id1 QuakeC source tree and fteqcc.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile
import time

from qbsp_smoke import brush

TIMING = re.compile(r'sv_speeds:\s*(\d+) ticks \| clients ([\d.]+) \| physics ([\d.]+).*?\| stats ([\d.]+) \| send ([\d.]+)')
NATIVE = re.compile(r'ECS_NATIVE_STRESS_PASS entities=(\d+) warmup=(\d+) ticks=(\d+) mean_ms=([\d.]+) median_ms=([\d.]+) p95_ms=([\d.]+) component_bytes=(\d+) setup_ms=([\d.]+) verified_ticks=(\d+)')


def command(argv, cwd, timeout):
    start = time.monotonic()
    result = subprocess.run([str(x) for x in argv], cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    return result, time.monotonic() - start


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--maptool-binary', type=Path,
                   help='engine binary with embedded QBSP, if different from --binary')
    p.add_argument('--basedir', type=Path, required=True)
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--quakec-source', type=Path, required=True)
    p.add_argument('--counts', nargs='+', type=int, default=[1024, 8192, 31998, 32768])
    p.add_argument('--max-edicts', type=int, default=32770)
    p.add_argument('--native-components', action='store_true',
                   help='require the opt-in native ECS component stress build')
    p.add_argument('--timeout', type=float, default=120)
    a = p.parse_args()
    if any(n not in (1024, 8192, 31998, 32768) for n in a.counts) or a.max_edicts < 256:
        p.error('unsupported entity count or maximum')
    binary, compiler, source = a.binary.resolve(), a.compiler.resolve(), a.quakec_source.resolve()
    maptool_binary = a.maptool_binary.resolve() if a.maptool_binary else binary
    pakdir = a.basedir.resolve()
    if (pakdir / 'id1').is_dir(): pakdir /= 'id1'
    paks = sorted(x for x in pakdir.iterdir() if re.fullmatch(r'pak\d+\.pak', x.name.lower()))
    if not paks: p.error(f'no Quake PAKs found in {pakdir}')
    root = Path(__file__).resolve().parents[1]
    work = Path(tempfile.mkdtemp(prefix='quiver-ecs-quakec-stress-'))
    print(f'results: {work}', flush=True)
    (work/'id1').mkdir(); (work/'user').mkdir(); (work/'stress/maps').mkdir(parents=True)
    for pak in paks: (work/'id1'/pak.name.lower()).symlink_to(pak)
    qc = work/'stress/qc'
    shutil.copytree(source, qc, ignore=shutil.ignore_patterns('.git', '*.dat'))
    defs, world, src = qc/'defs.qc', qc/'world.qc', qc/'progs.src'
    with defs.open('a') as f: f.write('\nvoid() ecs_stress_frame;\n')
    text, count = re.subn(r'(void\s*\(\s*\)\s*StartFrame\s*=\s*\{)',
                          r'\1\n    ecs_stress_frame();', world.read_text(), count=1)
    if count != 1: raise RuntimeError('could not find StartFrame')
    # Avoid four unrelated body queue edicts; the fixture runs without clients.
    text, count = re.subn(r'(?m)^\s*InitBodyQue\(\);', '', text, count=1)
    if count != 1: raise RuntimeError('could not remove body queue from test copy')
    world.write_text(text)
    lines = src.read_text().splitlines(); lines[0] = '../progs.dat'
    src.write_text('\n'.join(lines) + '\necs_stress.qc\n')
    shutil.copyfile(root/'tests/fixtures/ecs/quakec_tick_stress.qc', qc/'ecs_stress.qc')
    compiled, _ = command([compiler], qc, a.timeout)
    if compiled.returncode: raise RuntimeError('QuakeC compile failed:\n'+compiled.stdout)
    wad = root/'Misc/editor_defaults.wad'
    walls = [(-144,-144,-16,144,144,0), (-144,-144,128,144,144,144),
             (-144,-144,0,-128,144,128), (128,-144,0,144,144,128),
             (-128,-144,0,128,-128,128), (-128,128,0,128,144,128)]
    text = '{\n"classname" "worldspawn"\n"wad" "' + str(wad) + '"\n'
    text += ''.join(brush(*wall) for wall in walls) + '}\n'
    mapfile = work/'stress/maps/ecs1024.map'; mapfile.write_text(text)
    built, _ = command([maptool_binary, '--quiver-map-tool', 'qbsp', '-bsp2', '-nopercent', '-leaktest',
                        mapfile, mapfile.with_suffix('.bsp')], work, a.timeout)
    if built.returncode: raise RuntimeError('map compile failed:\n'+built.stdout)
    for n in a.counts:
        if n != 1024: shutil.copyfile(mapfile.with_suffix('.bsp'), work/f'stress/maps/ecs{n}.bsp')
    report = {'binary': str(binary), 'description': 'QuakeC edict boolean field; ECS-dispatched thinks',
              'native_components': a.native_components,
              'max_edicts_requested': a.max_edicts,
              'timing_note': 'sv_speeds instruments per-entity think callbacks; physics timing includes this instrumentation',
              'results': {}}
    for n in a.counts:
        print(f'Running ecs{n}...', flush=True)
        result, elapsed = command([binary, '-dedicated', '1', '-no-ecs-playable-stress', *(['-ecs-level-stress'] if a.native_components else []), '-basedir', '.', '-userdir', 'user', '-nosound', '-nojoy',
                                   '-game', 'stress', '+developer', '1', '+max_edicts', a.max_edicts,
                                   '+sys_ticrate', '0.02', '+sv_autosave', '0', '+sv_speeds', '1', '+map', f'ecs{n}'], work, a.timeout)
        (work/f'ecs{n}.log').write_text(result.stdout)
        passed = result.returncode == 0 and 'ECS_STRESS_PASS' in result.stdout and 'ECS_STRESS_FAIL' not in result.stdout
        native = NATIVE.search(result.stdout)
        if a.native_components:
            passed = passed and native is not None and int(native[1]) == n and 'ECS_NATIVE_STRESS_FAIL' not in result.stdout
        after = result.stdout.partition('ECS_STRESS_MEASURE')[2].partition('ECS_STRESS_PASS')[0]
        samples = [(int(t), float(physics)) for t, _clients, physics, _stats, _send in TIMING.findall(after)]
        if len(samples)>1: samples=samples[1:] # discard a potentially partial warmup interval
        maxed = re.search(r'ED_Alloc: no free edicts \(max_edicts is (\d+)\)', result.stdout)
        entry = dict(passed=passed, exit_code=result.returncode, elapsed_seconds=elapsed,
                     physics_ms_per_tick=statistics.median(x[1] for x in samples) if samples else None,
                     physics_samples=samples, error=maxed.group(0) if maxed else None)
        if native:
            entry['native_component'] = dict(entities=int(native[1]), warmup=int(native[2]), ticks=int(native[3]),
                                             mean_ms=float(native[4]), median_ms=float(native[5]), p95_ms=float(native[6]),
                                             component_bytes=int(native[7]), setup_ms=float(native[8]), verified_ticks=int(native[9]))
        report['results'][str(n)] = entry
        print(f'  {entry}', flush=True)
        (work/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    if not all(x['passed'] for x in report['results'].values()):
        raise SystemExit('one or more levels could not complete; inspect results and logs')

if __name__ == '__main__': main()
