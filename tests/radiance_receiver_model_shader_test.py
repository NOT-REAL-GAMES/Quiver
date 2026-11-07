#!/usr/bin/env python3
"""Compile/link model receiver interfaces and assert the reuse shader contract."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'Shaders'


def run(args):
    result = subprocess.run(args, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' + result.stdout)
    return result.stdout


def main():
    with tempfile.TemporaryDirectory(prefix='receiver-model-shaders-') as directory:
        temp = Path(directory)
        for cutout in (False, True):
            alpha = ['-DALIAS_ALPHA_TEST=1'] if cutout else []
            source = str(SHADERS / 'radiance_receiver_model.frag')
            output = temp / 'prepass.spv'
            run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', f'-I{SHADERS}', *alpha, source, '-o', str(output)])
            run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])
            dis = run(['spirv-dis', str(output)])
            block = re.search(r'OpName\s+(%\S+)\s+"ModelReceiver"', dis)[1]
            assert re.search(r'OpMemberDecorate\s+' + re.escape(block) + r'\s+0\s+Offset\s+80', dis), 'prepass token ABI'
            assert 'RayQuery' not in dis and 'RC_SampleDiffuse' not in dis, 'prepass performs lighting'
            assert ('OpKill' in dis) == cutout, 'prepass alpha coverage variant'
            # Link against the actual vertex variants, including eight-weight
            # skinning, so smooth-normal/UV/location drift fails this check.
            for vertex, flags in [('alias.vert', []), ('md5.vert', []), ('md5.vert', ['-DEIGHT_WEIGHT_SKINNING'])]:
                run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-l', f'-I{SHADERS}', '-DRADIANCE_LIGHTING=1',
                     *alpha, *flags, str(SHADERS / vertex), source, '-o', str(output)])
            for backend in ('sw', 'rt'):
                flags = ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_MODEL_CULL=1', '-DRC_RECEIVER_TEXTURE_FILTER=1',
                         '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1', *alpha]
                if backend == 'rt':
                    flags.append('-DRC_HARDWARE_RAY_QUERY=1')
                base = ['glslangValidator', f'-I{SHADERS}', *flags, str(SHADERS / 'alias.frag')]
                run([*base, '-V', '--target-env', 'vulkan1.1', '-o', str(output)])
                run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])
                pre = run([*base, '-E'])
                assert 'RC_ReceiverVisibilityCompatible' not in pre, 'upsampler rechecks probe visibility'
                assert 'RC_ReceiverSoftVisibility' not in pre and 'RC_ReceiverShadowSelect' not in pre, 'forward cloned optional shadow graph'
                assert 'RC_ReceiverLighting' in pre, 'model lacks receiver reuse'
                assert all(token in pre for token in ('125u', '27u', 'RC_FilterKernel')), 'fallback loses Gaussian'
                dis = run(['spirv-dis', str(output)])
                assert re.search(r'OpDecorate\s+%\S+\s+DescriptorSet\s+6\b', dis), 'receiver set ABI'
                assert 'OpKill' in dis, 'deferred model fragment lacks known-depth occlusion rejection'
                block = re.search(r'OpName\s+(%\S+)\s+"PushConsts"', dis)[1]
                assert re.search(r'OpMemberDecorate\s+' + re.escape(block) + r'\s+3\s+Offset\s+80', dis), 'forward token ABI'
        default = run(['glslangValidator', f'-I{SHADERS}', '-DRADIANCE_LIGHTING=1', '-E', str(SHADERS / 'alias.frag')])
        assert 'RC_ReceiverLighting' not in default and 'receiver_token' not in default, 'legacy model path changed interfaces'
        assert 'RC_ReceiverOccluded' not in default, 'opaque-only culling leaked into native model shader'
        output = temp / 'native.spv'
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', f'-I{SHADERS}', '-DRADIANCE_LIGHTING=1',
             str(SHADERS / 'alias.frag'), '-o', str(output)])
        assert 'OpKill' not in run(['spirv-dis', str(output)]), 'zero-alpha return added discard to native opaque shader'

    print('PASS: model prepass links all three vertex layouts; four reuse variants retain guarded fallback and query-free upsampling')


if __name__ == '__main__':
    main()
