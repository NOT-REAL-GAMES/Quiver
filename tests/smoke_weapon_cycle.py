#!/usr/bin/env python3
"""Exercise both wheel impulses in the bundled QuakeC on an isolated server."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
void() weapon_cycle_probe = {
    local entity saved;
    saved = self;
    self = spawn();
    self.items = IT_AXE | IT_SHOTGUN | IT_SUPER_SHOTGUN | IT_NAILGUN | IT_LIGHTNING;
    self.ammo_shells = 2; self.ammo_nails = 1; self.ammo_cells = 1;
    self.weapon = IT_AXE; self.impulse = 12; ImpulseCommands();
    if (self.weapon != IT_LIGHTNING || self.impulse != 0) dprint("WEAPON_CYCLE_FAIL reverse wrap\n");
    self.impulse = 12; ImpulseCommands();
    if (self.weapon != IT_NAILGUN) dprint("WEAPON_CYCLE_FAIL unowned\n");
    self.impulse = 12; ImpulseCommands();
    if (self.weapon != IT_SUPER_SHOTGUN) dprint("WEAPON_CYCLE_FAIL reverse\n");
    self.ammo_shells = 1; self.impulse = 12; ImpulseCommands();
    if (self.weapon != IT_SHOTGUN) dprint("WEAPON_CYCLE_FAIL ammo\n");
    self.impulse = 10; ImpulseCommands();
    if (self.weapon != IT_NAILGUN) dprint("WEAPON_CYCLE_FAIL forward ammo\n");
    self.items = IT_AXE; self.weapon = IT_AXE; self.impulse = 12; ImpulseCommands();
    if (self.weapon != IT_AXE) dprint("WEAPON_CYCLE_FAIL only axe\n");
    remove(self); self = saved;
    dprint("WEAPON_CYCLE_PASS\n");
};
'''


def run(args, cwd):
    result = subprocess.run([str(x) for x in args], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--basedir', required=True, type=Path)
    a = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='quiver-weapon-cycle-') as temp:
        work = Path(temp)
        (work / 'id1').mkdir()
        (work / 'user').mkdir()
        (work / 'probe').mkdir()
        for pak in (a.basedir.resolve() / 'id1').glob('pak*.pak'):
            (work / 'id1' / pak.name).symlink_to(pak)
        qc = work / 'qc'
        shutil.copytree(ROOT / 'ThirdParty/id1-quakec', qc)
        with (qc / 'defs.qc').open('a') as f: f.write('\nvoid() weapon_cycle_probe;\n')
        world = qc / 'world.qc'
        source, count = re.subn(r'(void\s*\(\s*\)\s*worldspawn\s*=\s*\{)',
                                r'\1\n    weapon_cycle_probe();', world.read_text(), count=1)
        if count != 1: raise RuntimeError('worldspawn not found')
        world.write_text(source)
        src = qc / 'progs.src'; lines = src.read_text().splitlines(); lines[0] = '../probe/progs.dat'
        src.write_text('\n'.join(lines) + '\nweapon_probe.qc\n')
        (qc / 'weapon_probe.qc').write_text(PROBE)
        run([a.compiler.resolve()], qc)
        output = run([a.binary.resolve(), '-dedicated', '1', '-basedir', work,
                      '-userdir', work / 'user', '-game', 'probe', '+developer', '1',
                      '+map', 'e1m1', '+quit'], work)
        if 'WEAPON_CYCLE_FAIL' in output or 'WEAPON_CYCLE_PASS' not in output:
            raise RuntimeError(output)
        print('PASS: weapon impulses wrap, skip unowned and empty weapons, and retain axe')


if __name__ == '__main__': main()
