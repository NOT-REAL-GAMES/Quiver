#!/usr/bin/env python3
"""The point of the light pass is a tiny forward shader: keep lookups out of it."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'Shaders'
COMMON = ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_FULL_LIGHT=1']
LIMIT = 32 * 1024


def run(args):
    result = subprocess.run(args, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' + result.stdout)
    return result.stdout


def main():
    with tempfile.TemporaryDirectory(prefix='receiver-light-shaders-') as directory:
        for source, extra in (('world.frag', []), ('alias.frag', ['-DRC_RECEIVER_MODEL_CULL=1']),
                              ('alias.frag', ['-DRC_RECEIVER_MODEL_CULL=1', '-DALIAS_ALPHA_TEST=1'])):
            output = Path(directory) / 'forward.spv'
            run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', f'-I{SHADERS}', *COMMON, *extra, str(SHADERS / source), '-o', str(output)])
            run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])
            size = output.stat().st_size
            assert size < LIMIT, f'{source} forward grew to {size} bytes: a lookup or fallback leaked back in'
            names = run(['spirv-dis', str(output)])
            for name in ('RC_ReceiverLighting', 'RC_SampleDiffuse', 'RC_EvaluateDirectComponents'):
                assert f'"{name}' not in names, f'{source} forward still contains {name}'
    print('PASS: full-light forward shaders hold no lookup or fallback')


if __name__ == '__main__':
    main()
