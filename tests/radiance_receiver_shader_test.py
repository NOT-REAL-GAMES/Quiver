#!/usr/bin/env python3
"""Compile and check cascade receiver shader variants, including classic paths.

This checks interfaces and shader availability; it does not substitute for a
rendered parity, performance, or Vulkan synchronization test.
"""
from pathlib import Path
import subprocess
import tempfile
import re

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'Shaders'


def run(args):
    result = subprocess.run(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' + result.stdout)
    return result.stdout


def main():
    with tempfile.TemporaryDirectory(prefix='quiver-radiance-receivers-') as directory:
        temp = Path(directory)
        counter = 0

        def compile_shader(source, stage, flags=(), mesh=False):
            nonlocal counter
            counter += 1
            output = temp / f'{counter}.spv'
            command = ['glslangValidator', '-V', '--target-env', 'vulkan1.1']
            if mesh: command += ['--target-env', 'spirv1.4']
            command += [f'-I{SHADERS}', '-S', stage, *flags, str(SHADERS / source), '-o', str(output)]
            run(command)
            run(['spirv-val', '--target-env', 'vulkan1.1spv1.4' if mesh else 'vulkan1.1', str(output)])
            return output

        def location(shader, name):
            text = run(['spirv-dis', str(shader)])
            named = re.search(r'OpName\s+(%\S+)\s+"' + re.escape(name) + r'"', text)
            if not named:
                raise RuntimeError(f'{shader} lacks varying {name}')
            decorated = re.search(r'OpDecorate\s+' + re.escape(named.group(1)) + r'\s+Location\s+(\d+)', text)
            if not decorated:
                raise RuntimeError(f'{shader} lacks location for {name}')
            return int(decorated.group(1))

        # Existing non-radiance shader paths must continue to compile.
        for source, stage in [('world.vert', 'vert'), ('world.frag', 'frag'), ('alias.vert', 'vert'),
                              ('alias_instanced.vert', 'vert'), ('md5.vert', 'vert'), ('alias.frag', 'frag')]:
            compile_shader(source, stage)

        # Compile both traversal backends and every distinct transparency path.
        world_modes = [[], ['-DWBOIT=1'], ['-DMBOIT=1'], ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1'],
                       ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']]
        alias_modes = [[], ['-DALIAS_ALPHA_TEST=1'], ['-DWBOIT=1'], ['-DALIAS_ALPHA_TEST=1', '-DWBOIT=1'],
                       ['-DMBOIT=1'], ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1'],
                       ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1'], ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1'],
                       ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1'],
                       ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1', '-DMBOIT_INPUT_SET=4']]
        for backend in ([], ['-DRC_HARDWARE_RAY_QUERY=1']):
            for mode in world_modes: compile_shader('world.frag', 'frag', ['-DRADIANCE_LIGHTING=1', *backend, *mode])
            for mode in alias_modes: compile_shader('alias.frag', 'frag', ['-DRADIANCE_LIGHTING=1', *backend, *mode])

        for backend in ([], ['-DRC_HARDWARE_RAY_QUERY=1']):
            compile_shader('world.frag', 'frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', *backend])
            compile_shader('radiance_receiver_direct.comp', 'comp', backend)
        compile_shader('radiance_receiver_half.comp', 'comp')
        compile_shader('radiance_receiver_half.comp', 'comp', ['-DRECEIVER_MSAA=1'])
        compile_shader('radiance_receiver_reconstruct.comp', 'comp')

        for source in ('world.vert', 'alias.vert', 'alias_instanced.vert', 'md5.vert'):
            compile_shader(source, 'vert', ['-DRADIANCE_LIGHTING=1'])
        compile_shader('md5.vert', 'vert', ['-DRADIANCE_LIGHTING=1', '-DEIGHT_WEIGHT_SKINNING'])
        for source in ('world_mesh.mesh', 'world_task_mesh.mesh'):
            compile_shader(source, 'mesh', ['-DRADIANCE_LIGHTING=1'], mesh=True)
        world_frag = compile_shader('world.frag', 'frag', ['-DRADIANCE_LIGHTING=1'])
        alias_frag = compile_shader('alias.frag', 'frag', ['-DRADIANCE_LIGHTING=1'])
        # Fragment storage buffers must be declared readonly, not merely left
        # unwritten, or Vulkan requires fragmentStoresAndAtomics at runtime.
        for fragment in (world_frag, alias_frag):
            disassembly = run(['spirv-dis', str(fragment)])
            for name in ('RCRawIntervals', 'RCMergedIntervals', 'RCRawDistances',
                         'RCMergedDistances', 'RCRawValidity', 'RCMergedValidity'):
                symbol = re.search(r'OpName\s+(%\S+)\s+"' + name + r'"', disassembly)
                if not symbol or not re.search(r'OpMemberDecorate\s+' + re.escape(symbol.group(1)) +
                                               r'\s+0\s+NonWritable\b', disassembly):
                    raise RuntimeError(f'{fragment}: {name} is not readonly')
        # Trace writes raw intervals while sampling only a stable merged field.
        # The qualifiers make accidental same-dispatch raw reads fail compile.
        trace = compile_shader('radiance_trace_sw.comp', 'comp')
        trace_disassembly = run(['spirv-dis', str(trace)])
        for qualifier, names in [('NonReadable', ('RCRawIntervals', 'RCRawDistances', 'RCRawValidity')),
                                 ('NonWritable', ('RCMergedIntervals', 'RCMergedDistances', 'RCMergedValidity'))]:
            for name in names:
                symbol = re.search(r'OpName\s+(%\S+)\s+"' + name + r'"', trace_disassembly)
                if not symbol or not re.search(r'OpMemberDecorate\s+' + re.escape(symbol.group(1)) +
                                               r'\s+0\s+' + qualifier + r'\b', trace_disassembly):
                    raise RuntimeError(f'{trace}: {name} lacks {qualifier}')
        for vert, frag in [('world.vert', world_frag), ('alias.vert', alias_frag),
                           ('alias_instanced.vert', alias_frag), ('md5.vert', alias_frag)]:
            vertex = compile_shader(vert, 'vert', ['-DRADIANCE_LIGHTING=1'])
            if location(vertex, 'out_world_position') != location(frag, 'in_world_position'):
                raise RuntimeError(f'{vert} position varying does not match fragment shader')
            if vert != 'world.vert' and location(vertex, 'out_world_normal') != location(frag, 'in_world_normal'):
                raise RuntimeError(f'{vert} normal varying does not match fragment shader')

        world = compile_shader('world.frag', 'frag', ['-DRADIANCE_LIGHTING=1'])
        optimized = temp / 'world-optimized.spv'
        run(['spirv-opt', '-Os', str(world), '-o', str(optimized)])
        disassembly = run(['spirv-dis', str(optimized)])
        if 'lightmap_tex' in disassembly:
            raise RuntimeError('radiance world shader unexpectedly retains the baked lightmap')
        if 'DescriptorSet 5' not in disassembly or 'in_world_position' not in disassembly:
            raise RuntimeError('radiance world shader lacks its receiver descriptor or position')
        print(f'PASS: {counter} compiled receiver shader variants and varying checks; radiance world excludes baked lightmaps')


if __name__ == '__main__':
    main()
