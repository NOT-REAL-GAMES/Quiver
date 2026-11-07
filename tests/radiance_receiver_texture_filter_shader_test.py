#!/usr/bin/env python3
"""Check producer fixed-eight lookup, Gaussian-aware fallback and texture-only
world-scaled filter storage/workgroup invariants."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'Shaders'


def run(arguments):
    result = subprocess.run(arguments, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(' '.join(map(str, arguments)) + '\n' + result.stdout)
    return result.stdout


def shared_bytes(disassembly):
    """Measure the emitted Workgroup variables, rather than mirroring GLSL bounds."""
    constants = {name: int(value) for name, value in re.findall(r'(%\S+) = OpConstant %\S+ (\d+)\b', disassembly)}
    types = {}
    for line in disassembly.splitlines():
        match = re.search(r'(%\S+) = OpType(Int|Float|Vector|Array|Pointer) (.*)', line)
        if match:
            name, kind, operands = match.groups()
            types[name] = kind, operands.split()

    def size(name):
        kind, operands = types[name]
        if kind in ('Int', 'Float'):
            return int(operands[0]) // 8
        if kind == 'Vector':
            return size(operands[0]) * int(operands[1])
        if kind == 'Array':
            return size(operands[0]) * constants[operands[1]]
        if kind == 'Pointer':
            return size(operands[1])
        raise RuntimeError(f'unexpected shared type: {name}')

    return sum(size(pointer) for pointer in re.findall(r'OpVariable (%\S+) Workgroup\b', disassembly))


def main():
    with tempfile.TemporaryDirectory(prefix='receiver-texture-shaders-') as directory:
        temp = Path(directory)
        variants = [('radiance_receiver_filter.comp', []),
                    ('radiance_receiver_diagnostics.comp', []),
                    ('radiance_receiver_half.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1']),
                    ('radiance_receiver_direct.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_SOFT_SHADOWS=1']),
                    ('radiance_receiver_direct.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_SOFT_SHADOWS=1', '-DRC_HARDWARE_RAY_QUERY=1']),
                    ('radiance_receiver_direct.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1']),
                    ('radiance_receiver_direct.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_HARDWARE_RAY_QUERY=1']),
                    ('world.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1']),
                    ('world.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1', '-DRC_HARDWARE_RAY_QUERY=1'])]
        for index, (source, flags) in enumerate(variants):
            path = SHADERS / source
            output = temp / f'{index}.spv'
            base = ['glslangValidator', f'-I{SHADERS}', *flags, str(path)]
            run([*base, '-V', '--target-env', 'vulkan1.1', '-o', str(output)])
            run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])
            preprocessed = run([*base, '-E'])
            if 'RC_ReceiverVisibilityCompatible' in preprocessed:
                raise RuntimeError(f'{source}: fast path includes a probe visibility compatibility scan')
            soft = '-DRC_RECEIVER_SOFT_SHADOWS=1' in flags
            if soft and 'RC_ReceiverSoftVisibility' not in preprocessed:
                raise RuntimeError('soft shadow pipeline lacks its outer rays')
            if source in ('radiance_receiver_direct.comp', 'world.frag'):
                if not soft and 'RC_ReceiverSoftVisibility' in preprocessed:
                    raise RuntimeError(f'{source}: outer-ray post helper leaked into default shader')
                if source == 'world.frag' and 'RC_EvaluateUnshadowedLight' in preprocessed:
                    raise RuntimeError('forward fallback contains optional shadow selection')
                if source == 'radiance_receiver_direct.comp' and soft != ('RC_ReceiverShadowSelect' in preprocessed):
                    raise RuntimeError('only the soft shadow pipeline selects a dominant light')
                abi = run(['spirv-dis', str(output)])
                block = re.search(r'OpName\s+(%\S+)\s+"RCLightingParameters"', abi)[1]
                if not re.search(r'OpMemberDecorate\s+' + re.escape(block) + r'\s+9\s+Offset\s+144', abi):
                    raise RuntimeError('receiver shadow parameters do not match CPU offset144')
                lookup = re.search(r'bool\s+RC_SampleDiffuseMode\s*\(.*?(?=bool\s+RC_SampleDiffuse\s*\()', preprocessed, re.S)[0]
                if source == 'world.frag':
                    if not all(token in lookup for token in ('125u', '27u', 'RC_FilterKernel')):
                        raise RuntimeError('fast fragment fallback does not honor Gaussian strength')
                else:
                    if not re.search(r'corner\s*<\s*8u\s*;', lookup) or any(token in lookup for token in ('125u', '27u', 'RC_FilterKernel', 'bool gaussian', 'bool wide')):
                        raise RuntimeError('fast producer does not compile to eight corners')
                if source == 'radiance_receiver_direct.comp':
                    if not re.search(r'if\s*\(\s*RC_BounceGain\s*\(\s*\)\s*>\s*0\.0', preprocessed):
                        raise RuntimeError('fast producer does not skip zero-gain GI')
            elif source in ('radiance_receiver_filter.comp', 'radiance_receiver_diagnostics.comp'):
                optimized = temp / 'filter-opt.spv'
                freeze = ['--freeze-spec-const', '--fold-spec-const-op-composite', '-Os']
                run(['spirv-opt', *freeze, str(output), '-o', str(optimized)])
                disassembly = run(['spirv-dis', str(optimized)])
                if re.search(r'OpName\s+%\S+\s+"(?:rcRaw|rcMerged|rcBrick)', disassembly):
                    raise RuntimeError(f'{source}: texture-only pass retains probe interval storage')
                if source == 'radiance_receiver_filter.comp':
                    if re.search(r'OpRayQuery|RC_(?:SceneTrace|SceneOccluded|LightVisible)|"rc(?:Scene|Material)', disassembly):
                        raise RuntimeError('selected-source filter includes a scene/material/visibility graph')
                    if not re.search(r'OpExecutionMode\s+%\S+\s+LocalSize\s+416\s+1\s+1', disassembly):
                        raise RuntimeError('filter workgroup does not match the 416x1 dispatch')
                    if 'OpAtomicOr' not in disassembly or 'OpControlBarrier' not in disassembly:
                        raise RuntimeError('filter lost its shared continuity cache')
                    if not 0 < shared_bytes(disassembly) <= 32768:
                        raise RuntimeError('filter shared cache exceeds Vulkan minimum')
                    wide = temp / 'filter-wide.spv'
                    run(['spirv-opt', '--set-spec-const-default-value', '1:512', *freeze, str(output), '-o', str(wide)])
                    wide_bytes = shared_bytes(run(['spirv-dis', str(wide)]))
                    if wide_bytes != 34328:
                        raise RuntimeError(f'512-lane filter needs {wide_bytes} bytes, not RC_RECEIVER_FILTER_WIDE_SHARED')
                if source == 'radiance_receiver_diagnostics.comp':
                    block = re.search(r'OpName\s+(%\S+)\s+"DiagnosticParams"', disassembly)[1]
                    if not re.search(r'OpMemberDecorate\s+' + re.escape(block) + r'\s+3\s+Offset\s+96', disassembly):
                        raise RuntimeError('diagnostic push constants do not match the 112-byte CPU layout')
        transport = run(['glslangValidator', f'-I{SHADERS}', '-E', str(SHADERS / 'radiance_trace_sw.comp')])
        if 'RC_EvaluateUnshadowedLight' in transport or 'RC_ReceiverSoftVisibility' in transport:
            raise RuntimeError('probe transport includes optional receiver shadows')
        alias = run(['glslangValidator', f'-I{SHADERS}', '-DRADIANCE_LIGHTING=1', '-E', str(SHADERS / 'alias.frag')])
        if 'RC_EvaluateUnshadowedLight' in alias or 'RC_ReceiverSoftVisibility' in alias:
            raise RuntimeError('default alias shader includes optional receiver shadows')
        pure = (SHADERS / 'radiance_light_unshadowed.glsl').read_text()
        if re.search(r'RC_(?:LightVisible|SceneTrace|SceneOccluded|EvaluateOneLightMode)\s*\(', pure):
            raise RuntimeError('unshadowed scoring helper contains a visibility query')
        pure = (SHADERS / 'radiance_light_query_free.glsl').read_text()
        if re.search(r'RC_(?:LightVisible|SceneTrace|SceneOccluded|EvaluateOneLight)\s*\(', pure):
            raise RuntimeError('shared radiometric math contains a visibility query')
        print('PASS: nine receiver variants, query-free metadata, isolated post, default/probe exclusion, packed filter')


if __name__ == '__main__':
    main()
