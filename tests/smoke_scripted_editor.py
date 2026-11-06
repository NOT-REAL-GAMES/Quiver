#!/usr/bin/env python3
"""Exercise the editor's isolated script compilation and child playtest.

Requires a built engine, its staged quiverscript/fteqcc tools and game sources,
local Quake PAKs, and a Vulkan display. All files and processes are isolated.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import bench_meshlets as bench
import smoke_sector_props_editor  # Installs the editor-console input shim.


def check_game_arguments():
    """Compile the production bounded helper; this mode never launches Vulkan."""
    source = (Path(__file__).resolve().parents[1] / 'Quake/editor.c').read_text()
    start = source.index('static const char *Editor_AppendGameDirectories (')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    function = source[start:end]
    fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
FUNCTION
int main(void) {
    const char *args[51] = {"engine", "-basedir", "base", "-userdir", "user"};
    char storage[1024]; size_t argc=5;const size_t capacity=42; /* MAX_NUM_ARGVS50 minus final8flags+NULL */
    assert(!Editor_AppendGameDirectories("",storage,sizeof(storage),args,capacity,&argc) && argc==5);
    assert(!Editor_AppendGameDirectories("hipnotic;mod with spaces;quiver_ripple",storage,sizeof(storage),args,capacity,&argc));
    assert(argc==11 && !strcmp(args[5],"-game") && !strcmp(args[6],"hipnotic"));
    assert(!strcmp(args[7],"-game") && !strcmp(args[8],"mod with spaces"));
    assert(!strcmp(args[9],"-game") && !strcmp(args[10],"quiver_ripple"));
    args[argc++]="-game";args[argc++]="__quiver_editor_package";
    assert(!strcmp(args[12],"__quiver_editor_package"));
    size_t before=argc;const char *sentinel="unchanged";args[before]=sentinel;
    assert(Editor_AppendGameDirectories("a;b",storage,sizeof(storage),args,before+3,&argc));
    assert(argc==before && args[before]==sentinel);
    assert(Editor_AppendGameDirectories("long_name",storage,4,args,capacity,&argc) && argc==before);
    assert(Editor_AppendGameDirectories("a;;b",storage,sizeof(storage),args,capacity,&argc) && argc==before);
    assert(Editor_AppendGameDirectories(";a",storage,sizeof(storage),args,capacity,&argc) && argc==before);
    assert(Editor_AppendGameDirectories("a;",storage,sizeof(storage),args,capacity,&argc) && argc==before);
    assert(!Editor_AppendGameDirectories("single_mod",storage,sizeof(storage),args,before+2,&argc));
    assert(argc==before+2 && !strcmp(args[before+1],"single_mod"));
    argc=5;
    assert(!Editor_AppendGameDirectories("a;b;c;d;e;f;g;h;i;j;k;l;m;n;o;p;q;r",storage,sizeof(storage),args,capacity,&argc));
    assert(argc==41);argc=5;
    assert(Editor_AppendGameDirectories("a;b;c;d;e;f;g;h;i;j;k;l;m;n;o;p;q;r;s",storage,sizeof(storage),args,capacity,&argc) && argc==5);
    puts("PASS: inherited game argument order, spaces, capacity bounds and failure rollback");
    return 0;
}
'''.replace('FUNCTION', function)
    with tempfile.TemporaryDirectory(prefix='editor-game-args-') as work:
        work = Path(work)
        (work / 'test.c').write_text(fixture)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(work / 'test.c'), '-o', str(work / 'test')], check=True)
        subprocess.run([str(work / 'test')], check=True)


def main():
    if sys.argv[1:] == ['--check-arguments']:
        check_game_arguments()
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--basedir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--stress-map', action='store_true', help='open the 32768-entity map and verify its scripted workload')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.game = 'editor_assets'
    args.width, args.height = 1280, 720
    source = args.basedir.resolve()
    if (source / 'id1').is_dir():
        source /= 'id1'
    paks = sorted(p for p in source.iterdir() if re.fullmatch(r'pak\d+\.pak', p.name.lower()))
    if not paks:
        parser.error(f'no Quake PAKs found in {source}')
    work = Path(tempfile.mkdtemp(prefix='quiver-scripted-editor-'))
    print(f'smoke directory: {work}', flush=True)
    (work / 'id1' / 'scripts').mkdir(parents=True)
    (work / 'id1' / 'maps').mkdir()
    (work / args.game / 'scripts').mkdir(parents=True)
    (work / args.game / 'maps').mkdir()
    (work / 'user').mkdir()
    for pak in paks:
        (work / 'id1' / pak.name.lower()).symlink_to(pak)
    shutil.copy2(Path(__file__).resolve().parents[1] / 'Misc/scripts/ecs_ripple.qs',
                 work / args.game / 'scripts/ripple.qs')
    (work / args.game / 'scripts/bad.qs').write_text('void bad( {\n')
    if args.stress_map:
        root = Path(__file__).resolve().parents[1]
        shutil.copy2(root / 'Misc/maps/ecs32768.map', work / args.game / 'maps/ecs32768.map')
        shutil.copy2(root / 'Misc/editor_defaults.wad', work / 'editor_defaults.wad')
    (work / 'id1/boot.cfg').write_text(bench.BOOT.replace('map start', 'max_edicts 32800\nmap start'))
    os.environ['SMOKE_WIDTH'] = str(args.width)
    os.environ['SMOKE_HEIGHT'] = str(args.height)
    engine = bench.Engine(args, work)
    child = None
    try:
        def command(body):
            engine.serial += 1
            marker = f'SCRIPT_EDITOR_{engine.serial}'
            start = len(engine.text())
            (work / args.game / 'control.tmp').write_text(body + '\necho ' + marker + '\n')
            (work / args.game / 'control.tmp').replace(work / args.game / 'control.cfg')
            os.write(engine.key, b'c')
            return engine.wait(lambda text: text if marker in text else None, start)

        engine.wait(lambda text: 'CL_SignonReply: 4' in text)
        initial = engine.cmd('disconnect\n' + ('editor_open ecs32768' if args.stress_map else 'editor_new'))
        if ('Source map opened' if args.stress_map else 'New 3D sector map') not in initial:
            raise RuntimeError('could not start sector editor')
        if 'World script updated' not in command('editor_script scripts/ripple.qs'):
            raise RuntimeError('could not set world script')
        start = len(engine.text())
        command('editor_play')
        engine.wait(lambda text: 'Scripted playtest started' in text, start)
        packages = list(work.glob('__quiver_editor_*'))
        if len(packages) != 1 or not (packages[0] / 'progs.dat').is_file():
            raise RuntimeError(f'isolated game package missing: {packages}')
        if len(list((packages[0] / 'maps').glob('*.bsp'))) != 1:
            raise RuntimeError('compiled map was not copied into the game package')
        children_file = Path(f'/proc/{engine.process.pid}/task/{engine.process.pid}/children')
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            children = children_file.read_text().split()
            if children:
                child = int(children[0])
                break
            time.sleep(.05)
        if child is None:
            raise RuntimeError('playtest child did not remain running')
        child_args = Path(f'/proc/{child}/cmdline').read_bytes().rstrip(b'\0').split(b'\0')
        child_games = [os.fsdecode(child_args[i + 1]) for i, value in enumerate(child_args[:-1]) if value == b'-game']
        if child_games != [args.game, packages[0].name]:
            raise RuntimeError(f'child lost original mod search paths or changed priority: {child_games}')
        engine.wait(lambda text: 'player entered the game' in text, start)
        if args.stress_map:
            engine.wait(lambda text: 'ECS stress: scripted local visuals active (32768 markers)' in text, start)
        # The child belongs to the isolated harness; stop it before closing the parent.
        os.kill(child, signal.SIGTERM)
        engine.wait(lambda text: 'Scripted playtest ended' in text or 'Scripted playtest failed' in text, start)
        child = None
        if 'World script updated' not in command('editor_script scripts/bad.qs'):
            raise RuntimeError('could not switch to invalid script')
        start = len(engine.text())
        command('editor_play')
        engine.wait(lambda text: 'Script compilation failed' in text, start)
        if 'Scripted playtest started' in engine.text()[start:]:
            raise RuntimeError('invalid script launched a stale playtest')
        print('PASS: world script, inherited mod assets, isolated child playtest, and compile failure', flush=True)
    finally:
        if child is not None:
            try:
                os.kill(child, signal.SIGTERM)
            except ProcessLookupError:
                pass
        if engine.process.poll() is None:
            engine.process.terminate()
            engine.process.wait(timeout=10)
        engine.close()


if __name__ == '__main__':
    main()
