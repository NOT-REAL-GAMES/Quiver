project(
    'vkquake',
    'c',
    meson_version : '>= 1.3',
    default_options : ['c_std=gnu23,c23', 'buildtype=release', 'strip=false', 'debug=true', 'b_ndebug=if-release', 'warning_level=1', 'werror=true'])

cc = meson.get_compiler('c')

# Always build keeping frame pointers to get better backtraces
add_project_arguments(cc.get_supported_arguments('-fno-omit-frame-pointer', '/Oy-'), language: 'c')

# Ask linker not to locate uninitialized variables into COMMON (will use .bss then) to avoid macOS link issue
add_project_arguments('-fno-common', language: 'c')

if host_machine.system() == 'windows'
    foreach native : [true, false]
        add_project_arguments('-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-D_WINSOCK_DEPRECATED_NO_WARNINGS', language : 'c', native : native)
    endforeach
endif

if host_machine.system() != 'windows'
    if cc.has_header_symbol('sched.h', 'CPU_ZERO', prefix: '#define _GNU_SOURCE')
        message('Info: Task affinity support enabled.')
        add_project_arguments('-D_GNU_SOURCE', language: 'c')
    else
        message('Warning: sched.h not found, disabling Task affinity support.')
        add_project_arguments('-DTASK_AFFINITY_NOT_AVAILABLE', language: 'c')
    endif
endif

shaders = [
    'Shaders/alias.vert',
    'Shaders/alias_instanced.vert',
    'Shaders/alias_ecs_cull.comp',
    'Shaders/radiance_trace_sw.comp',
    'Shaders/radiance_trace_rt.comp',
    'Shaders/radiance_merge.comp',
    'Shaders/radiance_receiver_world.frag',
    'Shaders/radiance_receiver_half.comp',
    'Shaders/radiance_receiver_reconstruct.comp',
    'Shaders/radiance_receiver_filter.comp',
    'Shaders/radiance_receiver_diagnostics.comp',
    'Shaders/md5.vert',
    'Shaders/basic.vert',
    'Shaders/fte_particles.frag',
    'Shaders/fte_particles_msaa.frag',
    'Shaders/basic_alphatest.frag',
    'Shaders/basic_notex.frag',
    'Shaders/draw_pic.frag',
    'Shaders/draw_pic_alphatest.frag',
    'Shaders/draw_pic_xbr.frag',
    'Shaders/draw_pic_xbr_alphatest.frag',
    'Shaders/draw_pic_xbr.vert',
    'Shaders/cs_tex_warp.comp',
    'Shaders/indirect.comp',
    'Shaders/indirect_clear.comp',
    'Shaders/meshlet_cull.comp',
    'Shaders/meshlet_hiz.comp',
    'Shaders/meshlet_hiz_raster.comp',
    'Shaders/meshlet_hiz_reduce.comp',
    'Shaders/postprocess.frag',
    'Shaders/scene_upscale.frag',
    'Shaders/ssao_composite.frag',
    'Shaders/ssao_composite_debug.frag',
    'Shaders/ssao_composite_msaa_debug.frag',
    'Shaders/ssao_prepare.comp',
    'Shaders/ssao_prepare_msaa.comp',
    'Shaders/ssao_evaluate.comp',
    'Shaders/ssao_evaluate_fp16.comp',
    'Shaders/ssao_mip.comp',
    'Shaders/ssao_mip_fp16.comp',
    'Shaders/ssao_mip_msaa.comp',
    'Shaders/ssao_mip_msaa_fp16.comp',
    'Shaders/ssao_filter.comp',
    'Shaders/ssao_filter_fp16.comp',
    'Shaders/ssao_composite_msaa.frag',
    'Shaders/postprocess.vert',
    'Shaders/showtris.frag',
    'Shaders/showtris.vert',
    'Shaders/md5_debug.vert',
    'Shaders/sky_box.frag',
    'Shaders/sky_cube.frag',
    'Shaders/sky_cube.vert',
    'Shaders/sky_layer.frag',
    'Shaders/sky_layer.vert',
    'Shaders/world.vert',
    'Shaders/ray_debug.comp',
    'Shaders/ray_debug_hdr.comp',
    'Shaders/screen_effects_hdr.comp',
    'Shaders/mesh_interpolate.comp',
    'Shaders/skinning.comp',
]

# Prefer SDL3 over SDL2; -Duse_sdl3=enabled requires SDL3, disabled forces SDL2
use_sdl3_opt = get_option('use_sdl3')
use_sdl3 = false
if host_machine.system() == 'windows'
    if use_sdl3_opt.disabled()
        error('SDL2 is not supported on Windows')
    endif
    # use the bundled SDL3 like the other Windows builds do
    sdl_dep = declare_dependency(
        include_directories : include_directories('Windows/SDL3/include'),
        dependencies : cc.find_library('SDL3', dirs : meson.project_source_root() / 'Windows/SDL3/lib64'))
    use_sdl3 = true
else
    if not use_sdl3_opt.disabled()
        sdl_dep = dependency('sdl3', required : use_sdl3_opt.enabled())
        use_sdl3 = sdl_dep.found()
    endif
endif
if use_sdl3
    add_project_arguments('-DUSE_SDL3', language: 'c')
    message('Info: Using SDL3.')
else
    sdl_dep = dependency('sdl2')
    message('Info: Using SDL2.')
endif

if host_machine.system() == 'darwin' and use_sdl3
    add_languages('objc', required : true, native : false)
    add_project_arguments('-fblocks', language : 'objc')
    add_project_arguments('-DUSE_SDL3', language : 'objc')
endif

glslang = find_program('glslangValidator')
spirv_opt = find_program('spirv-opt')
python = import('python').find_installation()
mesh_shader_probe = '''
import os, subprocess, sys, tempfile
source = ('#version 460\n#extension GL_EXT_mesh_shader : require\n'
          'layout(local_size_x=1) in;\n'
          'layout(triangles, max_vertices=3, max_primitives=1) out;\n'
          'void main() { SetMeshOutputsEXT(0, 0); }\n')
with tempfile.TemporaryDirectory() as directory:
    output = os.path.join(directory, 'mesh.spv')
    result = subprocess.run([sys.argv[1], '--stdin', '-S', 'mesh', '-V',
                             '--target-env', 'vulkan1.1', '--target-env', 'spirv1.4',
                             '-o', output], input=source, text=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if result.returncode == 0:
        result = subprocess.run([sys.argv[2], '-Os', output, '-o', output],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sys.exit(result.returncode)
'''
mesh_shader_supported = run_command(python, '-c', mesh_shader_probe, glslang, spirv_opt, check : false).returncode() == 0
if mesh_shader_supported
    shaders += ['Shaders/world_mesh.mesh']
    add_project_arguments('-DHAVE_WORLD_MESH_SHADER=1', language : 'c')
    task_shader_probe = '''
import os, subprocess, sys, tempfile
source = ('#version 460\n#extension GL_EXT_mesh_shader : require\n'
          'layout(local_size_x=1) in;\n'
          'void main() { EmitMeshTasksEXT(0, 1, 1); }\n')
with tempfile.TemporaryDirectory() as directory:
    output = os.path.join(directory, 'task.spv')
    result = subprocess.run([sys.argv[1], '--stdin', '-S', 'task', '-V',
                             '--target-env', 'vulkan1.1', '--target-env', 'spirv1.4',
                             '-o', output], input=source, text=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if result.returncode == 0:
        result = subprocess.run([sys.argv[2], '-Os', output, '-o', output],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sys.exit(result.returncode)
'''
    task_shader_supported = run_command(python, '-c', task_shader_probe, glslang, spirv_opt, check : false).returncode() == 0
    if task_shader_supported
        shaders += ['Shaders/world_task.task', 'Shaders/world_task_mesh.mesh']
        add_project_arguments('-DHAVE_WORLD_TASK_SHADER=1', language : 'c')
    else
        message('Task shaders disabled because the shader toolchain does not support them')
    endif
else
    message('Mesh shaders disabled because the shader toolchain does not support SPIR-V mesh shaders')
endif
run_test_canonicalize_ids_spirv_opt = run_command(spirv_opt, '-h', check : false)
has_canonicalize_ids_spirv_opt = (run_test_canonicalize_ids_spirv_opt.stdout().strip().contains('--canonicalize-ids'))
bintoc = executable('bintoc', ['Shaders/bintoc.c'], native: true)
mkpak = executable('mkpak', ['Misc/vq_pak/mkpak.c'], native: true)

if (build_machine.system() == 'darwin') or get_option('buildtype').startswith('debug')
    spirv_opt_args = [] # MoltenVK spirv-cross has a bug that breaks optimized shaders
else
    if not has_canonicalize_ids_spirv_opt
        error('spirv-opt does not support --canonicalize-ids, a newer Vulkan SDK is required')
    endif
    spirv_opt_args = ['-Os', '--canonicalize-ids', '--strip-debug']
endif

glslang_args = ['-V', '--quiet']
if get_option('buildtype').startswith('debug')
    glslang_args += '-g'
endif

vq_pak_path = join_paths(meson.project_source_root(), 'Misc', 'vq_pak')
mkpak_toc_file = join_paths(vq_pak_path, 'vq_pak_contents.txt')

mkpak_command = [mkpak, '@OUTPUT@', vq_pak_path, mkpak_toc_file, '@DEPFILE@']

run_mkpak = custom_target('run-mkpak',
  output : 'vkquake.pak',
  depfile : 'vkquake.pak.d',
  command: mkpak_command)

# Write the embedded pak into the build dir so Meson can track it.
bintoc_compress_command = [bintoc, '-c', '@INPUT@', 'vkquake.pak', '@OUTPUT@']

run_generate_embedded_pak = custom_target('run-embed_vkquake_pak',
							output : 'embedded_pak.c',
							input : run_mkpak,
							command : bintoc_compress_command)

# shader sources compiled multiple times with different preprocessor defines
shader_variants = [
    ['Shaders/basic.frag', 'basic.frag', []],
    ['Shaders/basic.frag', 'basic_oit.frag', ['-DWBOIT=1']],
    ['Shaders/basic.frag', 'basic_mboit_moment.frag', ['-DMBOIT=1']],
    ['Shaders/basic.frag', 'basic_mboit_composite.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1']],
    ['Shaders/basic.frag', 'basic_mboit_composite_msaa.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']],
    ['Shaders/world.frag', 'world.frag', []],
    ['Shaders/world.frag', 'world_oit.frag', ['-DWBOIT=1']],
    ['Shaders/world.frag', 'world_mboit_moment.frag', ['-DMBOIT=1']],
    ['Shaders/world.frag', 'world_mboit_composite.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1']],
    ['Shaders/world.frag', 'world_mboit_composite_msaa.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']],
    ['Shaders/alias.frag', 'alias.frag', []],
    ['Shaders/alias.frag', 'alias_alphatest.frag', ['-DALIAS_ALPHA_TEST=1']],
    ['Shaders/alias.frag', 'alias_oit.frag', ['-DWBOIT=1']],
    ['Shaders/alias.frag', 'alias_alphatest_oit.frag', ['-DALIAS_ALPHA_TEST=1', '-DWBOIT=1']],
    ['Shaders/alias.frag', 'alias_mboit_moment.frag', ['-DMBOIT=1']],
    ['Shaders/alias.frag', 'alias_alphatest_mboit_moment.frag', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1']],
    ['Shaders/alias.frag', 'alias_mboit_composite.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1']],
    ['Shaders/alias.frag', 'alias_alphatest_mboit_composite.frag', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1']],
    ['Shaders/alias.frag', 'alias_mboit_composite_msaa.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']],
    ['Shaders/alias.frag', 'alias_alphatest_mboit_composite_msaa.frag', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']],
    ['Shaders/alias.frag', 'md5_mboit_composite.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMBOIT_INPUT_SET=4']],
    ['Shaders/alias.frag', 'md5_alphatest_mboit_composite.frag', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMBOIT_INPUT_SET=4']],
    ['Shaders/alias.frag', 'md5_mboit_composite_msaa.frag', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1', '-DMBOIT_INPUT_SET=4']],
    ['Shaders/alias.frag', 'md5_alphatest_mboit_composite_msaa.frag', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1', '-DMBOIT_INPUT_SET=4']],
    ['Shaders/md5.vert', 'md5_8.vert', ['-DEIGHT_WEIGHT_SKINNING']],
    ['Shaders/wboit_resolve.frag', 'wboit_resolve.frag', []],
    ['Shaders/wboit_resolve.frag', 'wboit_resolve_msaa.frag', ['-DMSAA=1']],
    ['Shaders/mboit_resolve.frag', 'mboit_resolve.frag', []],
    ['Shaders/mboit_resolve.frag', 'mboit_resolve_msaa.frag', ['-DMSAA=1']],
    ['Shaders/screen_effects.comp', 'screen_effects_8bit.comp', []],
    ['Shaders/screen_effects.comp', 'screen_effects_10bit.comp', ['-DUSE_10BIT=1']],
    ['Shaders/update_lightmap.comp', 'update_lightmap_8bit.comp', []],
    ['Shaders/update_lightmap.comp', 'update_lightmap_8bit_rt.comp', ['-DRAY_QUERIES=1']],
    ['Shaders/update_lightmap.comp', 'update_lightmap_10bit.comp', ['-DUSE_10BIT=1']],
    ['Shaders/update_lightmap.comp', 'update_lightmap_10bit_rt.comp', ['-DUSE_10BIT=1', '-DRAY_QUERIES=1']],
    ['Shaders/skinning.comp', 'skinning_8.comp', ['-DEIGHT_WEIGHT_SKINNING']],
]

# Real-time radiance receivers share the existing geometry and transparency
# paths; compile independent SW-BVH and ray-query fragment variants.
shader_variants += [
    ['Shaders/world.vert', 'world_radiance.vert', ['-DRADIANCE_LIGHTING=1']],
    ['Shaders/world.frag', 'world_receiver_forward_sw.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1']],
    ['Shaders/world.frag', 'world_receiver_forward_rt.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_HARDWARE_RAY_QUERY=1']],
    ['Shaders/radiance_receiver_direct.comp', 'radiance_receiver_direct_sw.comp', []],
    ['Shaders/radiance_receiver_direct.comp', 'radiance_receiver_direct_rt.comp', ['-DRC_HARDWARE_RAY_QUERY=1']],
    ['Shaders/radiance_receiver_half.comp', 'radiance_receiver_half_fast.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1']],
    ['Shaders/world.frag', 'world_receiver_forward_fast_sw.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1']],
    ['Shaders/world.frag', 'world_receiver_forward_fast_rt.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1', '-DRC_HARDWARE_RAY_QUERY=1']],
    ['Shaders/radiance_receiver_direct.comp', 'radiance_receiver_direct_fast_sw.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1']],
    ['Shaders/radiance_receiver_direct.comp', 'radiance_receiver_direct_fast_rt.comp', ['-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_HARDWARE_RAY_QUERY=1']],
    ['Shaders/radiance_receiver_soft.comp', 'radiance_receiver_soft_sw.comp', []],
    ['Shaders/radiance_receiver_soft.comp', 'radiance_receiver_soft_rt.comp', ['-DRC_HARDWARE_RAY_QUERY=1']],
    ['Shaders/radiance_receiver_model.frag', 'radiance_receiver_model.frag', []],
    ['Shaders/radiance_receiver_model.frag', 'radiance_receiver_model_alphatest.frag', ['-DALIAS_ALPHA_TEST=1']],
    ['Shaders/alias.frag', 'alias_receiver_forward_fast_sw.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_MODEL_CULL=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1']],
    ['Shaders/alias.frag', 'alias_receiver_forward_fast_sw_alphatest.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_MODEL_CULL=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1', '-DALIAS_ALPHA_TEST=1']],
    ['Shaders/alias.frag', 'alias_receiver_forward_fast_rt.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_MODEL_CULL=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1', '-DRC_HARDWARE_RAY_QUERY=1']],
    ['Shaders/alias.frag', 'alias_receiver_forward_fast_rt_alphatest.frag', ['-DRADIANCE_LIGHTING=1', '-DRC_RECEIVER_REUSE=1', '-DRC_RECEIVER_MODEL_CULL=1', '-DRC_RECEIVER_TEXTURE_FILTER=1', '-DRC_RECEIVER_GAUSSIAN_FALLBACK=1', '-DRC_HARDWARE_RAY_QUERY=1', '-DALIAS_ALPHA_TEST=1']],
    ['Shaders/alias.vert', 'alias_radiance.vert', ['-DRADIANCE_LIGHTING=1']],
    ['Shaders/alias_instanced.vert', 'alias_instanced_radiance.vert', ['-DRADIANCE_LIGHTING=1']],
    ['Shaders/md5.vert', 'md5_radiance.vert', ['-DRADIANCE_LIGHTING=1']],
    ['Shaders/md5.vert', 'md5_8_radiance.vert', ['-DRADIANCE_LIGHTING=1', '-DEIGHT_WEIGHT_SKINNING']],
]
world_radiance_modes = [
    ['', []], ['_oit', ['-DWBOIT=1']], ['_mboit_moment', ['-DMBOIT=1']],
    ['_mboit_composite', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1']],
    ['_mboit_composite_msaa', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']],
]
alias_radiance_modes = [
    ['', []], ['_alphatest', ['-DALIAS_ALPHA_TEST=1']],
    ['_oit', ['-DWBOIT=1']], ['_alphatest_oit', ['-DALIAS_ALPHA_TEST=1', '-DWBOIT=1']],
    ['_mboit_moment', ['-DMBOIT=1']], ['_alphatest_mboit_moment', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1']],
    ['_mboit_composite', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1']],
    ['_alphatest_mboit_composite', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1']],
    ['_mboit_composite_msaa', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']],
    ['_alphatest_mboit_composite_msaa', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1']],
]
md5_radiance_modes = [
    ['_mboit_composite', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMBOIT_INPUT_SET=4']],
    ['_alphatest_mboit_composite', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMBOIT_INPUT_SET=4']],
    ['_mboit_composite_msaa', ['-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1', '-DMBOIT_INPUT_SET=4']],
    ['_alphatest_mboit_composite_msaa', ['-DALIAS_ALPHA_TEST=1', '-DMBOIT=1', '-DMBOIT_COMPOSITE=1', '-DMSAA=1', '-DMBOIT_INPUT_SET=4']],
]
foreach backend : ['sw', 'rt']
    backend_flags = backend == 'rt' ? ['-DRC_HARDWARE_RAY_QUERY=1'] : []
    foreach mode : world_radiance_modes
        shader_variants += [['Shaders/world.frag', 'world_radiance_' + backend + mode[0] + '.frag', ['-DRADIANCE_LIGHTING=1'] + backend_flags + mode[1]]]
    endforeach
    foreach mode : alias_radiance_modes
        shader_variants += [['Shaders/alias.frag', 'alias_radiance_' + backend + mode[0] + '.frag', ['-DRADIANCE_LIGHTING=1'] + backend_flags + mode[1]]]
    endforeach
    foreach mode : md5_radiance_modes
        shader_variants += [['Shaders/alias.frag', 'md5_radiance_' + backend + mode[0] + '.frag', ['-DRADIANCE_LIGHTING=1'] + backend_flags + mode[1]]]
    endforeach
endforeach
if mesh_shader_supported
    shader_variants += [['Shaders/world_mesh.mesh', 'world_mesh_radiance.mesh', ['-DRADIANCE_LIGHTING=1', '--target-env', 'vulkan1.1', '--target-env', 'spirv1.4']]]
    if task_shader_supported
        shader_variants += [['Shaders/world_task_mesh.mesh', 'world_task_mesh_radiance.mesh', ['-DRADIANCE_LIGHTING=1', '--target-env', 'vulkan1.1', '--target-env', 'spirv1.4']]]
    endif
endif

fs = import('fs')

shader_jobs = []
foreach shader : shaders
    target_env = (shader.contains('sops') or shader.contains('ssao_mip') or shader.contains('_fp16')) ? ['--target-env', 'vulkan1.1'] : []
    if shader.endswith('.mesh') or shader.endswith('.task')
        target_env = ['--target-env', 'vulkan1.1', '--target-env', 'spirv1.4']
    endif
    shader_jobs += [[shader, fs.name(shader), target_env]]
endforeach
foreach variant : shader_variants
    shader_jobs += [[variant[0], variant[1], variant[2]]]
endforeach

shaders_c = []
foreach job : shader_jobs
    source = job[0]
    name = job[1]
    args = glslang_args + job[2]
    # Keep the large shared-memory filter helper explicit as well as in the
    # compiler depfile, so header-only edits also rebuild an existing target.
    filter_dependencies = name == 'radiance_receiver_filter.comp' ? files('Shaders/radiance_receiver_filter_axis.glsl') : []
    if source.contains('radiance') or name.contains('radiance') or name.contains('receiver')
        filter_dependencies += files('Shaders/radiance_light_query_free.glsl')
    endif
    if source == 'Shaders/radiance_receiver_filter.comp'
        filter_dependencies += files('Shaders/radiance_receiver_shadow_filter.glsl', 'Shaders/radiance_light_unshadowed.glsl')
    endif
    if source in ['Shaders/radiance_receiver_half.comp', 'Shaders/radiance_receiver_reconstruct.comp',
                  'Shaders/radiance_receiver_filter.comp', 'Shaders/radiance_receiver_diagnostics.comp'] or name.startswith('world_receiver_forward') or name.startswith('alias_receiver_forward')
        filter_dependencies += files('Shaders/radiance_receiver_scale.glsl')
    endif
    if source in ['Shaders/radiance_receiver_model.frag', 'Shaders/alias.frag']
        filter_dependencies += files('Shaders/radiance_model_normal.glsl', 'Shaders/alias_common.inc')
    endif
    if source in ['Shaders/radiance_receiver_direct.comp', 'Shaders/radiance_receiver_soft.comp']
        filter_dependencies += files('Shaders/radiance_receiver_shadows.glsl', 'Shaders/radiance_receiver_shadow_metadata.glsl',
            'Shaders/radiance_receiver_shadow_source.glsl', 'Shaders/radiance_light_unshadowed.glsl', 'Shaders/radiance_parameters.glsl')
    endif
    if spirv_opt_args.length() > 0
        spv = custom_target(name.underscorify() + '_raw_spv',
            input : source,
            output : name + '.raw.spv',
            depfile : name + '.deps',
            depend_files : filter_dependencies,
            command : [glslang] + args + ['-o', '@OUTPUT@', '@INPUT@', '--depfile', '@DEPFILE@'])
        spv = custom_target(name.underscorify() + '_spv',
            input : spv,
            output : name + '.spv',
            command : [spirv_opt] + spirv_opt_args + ['@INPUT@', '-o', '@OUTPUT@'])
    else
        spv = custom_target(name.underscorify() + '_spv',
            input : source,
            output : name + '.spv',
            depfile : name + '.deps',
            depend_files : filter_dependencies,
            command : [glslang] + args + ['-o', '@OUTPUT@', '@INPUT@', '--depfile', '@DEPFILE@'])
    endif
    if name == 'radiance_trace_sw.comp'
        radiance_test_trace_spv = spv
    elif name == 'radiance_merge.comp'
        radiance_test_merge_spv = spv
    endif
    shaders_c += custom_target(name.underscorify() + '_c',
        input : spv,
        output : name + '.c',
        command : [bintoc, '@INPUT@', name + '_spv', '@OUTPUT@'])
endforeach

srcs = [
    'Quake/bgmusic.c',
    'Quake/cd_null.c',
    'Quake/cfgfile.c',
    'Quake/chase.c',
    'Quake/cl_demo.c',
    'Quake/cl_input.c',
    'Quake/cl_main.c',
    'Quake/cl_parse.c',
    'Quake/cl_tent.c',
    'Quake/cmd.c',
    'Quake/common.c',
    'Quake/console.c',
    'Quake/crc.c',
    'Quake/cvar.c',
    'Quake/gl_draw.c',
    'Quake/gl_fog.c',
    'Quake/gl_heap.c',
    'Quake/gl_mesh.c',
    'Quake/gl_model.c',
    'Quake/gl_refrag.c',
    'Quake/gl_rlight.c',
    'Quake/gl_rmain.c',
    'Quake/gl_rmisc.c',
    'Quake/r_passes.c',
    'Quake/r_ecs_gpu.c',
    'Quake/radiance_cascades.c',
    'Quake/radiance_emissive.c',
    'Quake/radiance_lights.c',
    'Quake/radiance_gpu_inputs.c',
    'Quake/radiance_material_atlas.c',
    'Quake/radiance_materials_quake.c',
    'Quake/radiance_coordinator.c',
    'Quake/radiance_matrix.c',
    'Quake/radiance_receiver_vulkan.c',
    'Quake/radiance_receiver_targets.c',
    'Quake/radiance_receiver_pipelines.c',
    'Quake/radiance_receiver_record.c',
    'Quake/radiance_receiver_diagnostics.c',
    'Quake/radiance_receiver_world.c',
    'Quake/radiance_receiver_model.c',
    'Quake/radiance_receiver_frame.c',
    'Quake/radiance_receiver_snapshot.c',
    'Quake/radiance_receiver_forward.c',
    'Quake/radiance_receiver_capture.c',
    'Quake/radiance_receiver_select.c',
    'Quake/radiance_receiver_descriptors.c',
    'Quake/radiance_receiver_layouts.c',
    'Quake/radiance_receiver_resources.c',
    'Quake/radiance_receiver_stage.c',
    'Quake/radiance_receiver_alternates.c',
    'Quake/radiance_receiver_shaders.c',
    'Quake/radiance_receiver_slot.c',
    'Quake/radiance_receiver_live.c',
    'Quake/radiance_scene.c',
    'Quake/radiance_alias.c',
    'Quake/radiance_scene_alias.c',
    'Quake/radiance_scene_bsp.c',
    'Quake/radiance_scene_runtime.c',
    'Quake/radiance_scene_vulkan.c',
    'Quake/radiance_scene_rt.c',
    'Quake/radiance_scene_collect.c',
    'Quake/radiance_sparse.c',
    'Quake/radiance_vulkan.c',
    'Quake/radiance_engine.c',
    'Quake/radiance_runtime.c',
    'Quake/r_ssao.c',
    'Quake/gl_screen.c',
    'Quake/gl_sky.c',
    'Quake/gl_texmgr.c',
    'Quake/gl_vidsdl.c',
    'Quake/gl_warp.c',
    'Quake/host.c',
    'Quake/host_cmd.c',
    'Quake/image.c',
    'Quake/json.c',
    'Quake/in_sdl.c',
    'Quake/in_sdl2.c',
    'Quake/in_sdl3.c',
    'Quake/keys.c',
    'Quake/main_sdl.c',
    'Quake/mathlib.c',
    'Quake/mdfour.c',
    'Quake/mem.c',
    'Quake/menu.c',
    'Quake/net_dgrm.c',
    'Quake/net_loop.c',
    'Quake/net_main.c',
    'Quake/palette.c',
    'Quake/pr_cmds.c',
    'Quake/pr_edict.c',
    'Quake/pr_exec.c',
    'Quake/pr_ext.c',
    'Quake/r_alias.c',
    'Quake/r_brush.c',
    'Quake/bsp_meshlet.c',
    'Quake/editor.c',
    'Quake/editor_map.c',
    'Quake/editor_sector.c',
    'Quake/editor_sector_portal.c',
    'Quake/editor_process.c',
    'Quake/editor_tools.c',
    'Quake/editor_ui.c',
    'Quake/editor_model_preview.c',
    'Quake/ui_control.c',
    'Quake/ecs.c',
    'Quake/editor_controls.c',
    'Quake/static_models.c',
    'Quake/static_models_collision.c',
    'Quake/static_models_engine.c',
    'Quake/meshlet_hiz.c',
    'Quake/r_part.c',
    'Quake/r_part_fte.c',
    'Quake/r_sprite.c',
    'Quake/r_world.c',
    'Quake/sbar.c',
    'Quake/snd_codec.c',
    'Quake/snd_dma.c',
    'Quake/snd_mem.c',
    'Quake/snd_mix.c',
    'Quake/snd_sdl.c',
    'Quake/snd_sdl3.c',
    'Quake/snd_umx.c',
    'Quake/snd_wave.c',
    'Quake/steam.c',
    'Quake/strlcat.c',
    'Quake/strlcpy.c',
    'Quake/sv_main.c',
    'Quake/sv_move.c',
    'Quake/sv_phys.c',
    'Quake/sv_user.c',
	'Quake/sys_sdl.c',
    'Quake/tasks.c',
    'Quake/view.c',
    'Quake/wad.c',
    'Quake/world.c',
	'Quake/hash_map.c',
	 run_generate_embedded_pak,
]

if get_option('ecs_level_stress')
    srcs += files('tests/ecs_level_stress.c')
endif

if host_machine.system() == 'windows'
    srcs += [
        'Quake/net_win.c',
        'Quake/net_wins.c',
        'Quake/net_wipx.c',
        'Quake/pl_win.c',
        'Quake/sys_sdl_win.c',
    ]
    srcs += import('windows').compile_resources(
        'Windows/vkQuake.rc',
        # the \xA9 copyright escape is Windows-1252; llvm-rc refuses 8 bit
        # codepoints without an explicit codepage. gcc builds use windres,
        # which takes no /C option
        args : cc.get_id() == 'gcc' ? [] : ['/C', '1252'],
        include_directories : include_directories('Windows', 'Quake'),
        depend_files : ['Windows/vkQuake.ico'])
else
    srcs += [
        'Quake/net_bsd.c',
        'Quake/net_udp.c',
        'Quake/pl_linux.c',
        'Quake/sys_sdl_unix.c',
    ]
endif

cflags = cc.get_supported_arguments('-Wno-trigraphs', '-Wno-microsoft-enum-forward-reference') + ['-D_FILE_OFFSET_BITS=64']
if get_option('ecs_level_stress')
    cflags += '-DECS_LEVEL_STRESS'
endif
if cc.get_id() == 'clang'
    # the stb single header libraries define their whole API static
    cflags += '-Wno-unused-function'
endif

deps = [
    cc.find_library('m', required : false),
    cc.find_library('dl', required : false),
    dependency('threads'),
    sdl_dep,
]

if host_machine.system() == 'darwin' and use_sdl3
    srcs += 'Quake/in_mac.m'
    deps += dependency('appleframeworks', modules : ['Foundation', 'GameController'])
endif

if host_machine.system() == 'windows'
    foreach syslib : ['ws2_32', 'winmm', 'dbghelp', 'ole32', 'uuid']
        deps += cc.find_library(syslib)
    endforeach
    import('fs').copyfile('Windows/SDL3/lib64/SDL3.dll')
endif

if build_machine.system() == 'darwin'
    # MoltenVK is linked directly by default. -Duse_vulkan_loader=enabled links the
    # Vulkan loader instead, so the driver can be chosen with VK_DRIVER_FILES.
    vulkan_dep = dependency('vulkan', version: '>=1.4.341', required : get_option('use_vulkan_loader'))
else
    vulkan_dep = dependency('vulkan', version: '>=1.4.341')
endif
if vulkan_dep.found()
    deps += vulkan_dep
else
    molten_dirs = []
    if import('fs').exists('/opt/homebrew/lib/libMoltenVK.dylib')
        molten_dirs += '/opt/homebrew/lib'
    endif
    deps += cc.find_library('MoltenVK', required : true, dirs : molten_dirs)
endif

if get_option('use_codec_wave').enabled()
    cflags += '-DUSE_CODEC_WAVE'
endif

if host_machine.system() == 'windows'
    # the bundled codec libraries, like the other Windows builds use
    codec_inc = include_directories('Windows/codecs/include')
    codec_lib_dir = meson.project_source_root() / 'Windows/codecs/x64'
    codec_deps = {}
    foreach name, option : {
        'libmad' : 'use_codec_mp3',
        'libmpg123' : 'use_codec_mp3',
        'libFLAC' : 'use_codec_flac',
        'libogg' : 'use_codec_vorbis',
        'libvorbisfile' : 'use_codec_vorbis',
        'libvorbis' : 'use_codec_vorbis',
        'libopus' : 'use_codec_opus',
        'libopusfile' : 'use_codec_opus',
    }
        lib = cc.find_library(name, dirs : codec_lib_dir, required : get_option(option))
        codec_deps += {name : lib.found() ? declare_dependency(include_directories : codec_inc, dependencies : lib) : lib}
    endforeach
    foreach dll : ['libFLAC-8.dll', 'libmad-0.dll', 'libmpg123-0.dll', 'libogg-0.dll', 'libopus-0.dll', 'libopusfile-0.dll', 'libvorbis-0.dll', 'libvorbisfile-3.dll']
        import('fs').copyfile('Windows/codecs/x64' / dll)
    endforeach
    mad_dep = codec_deps['libmad']
    mpg123_dep = codec_deps['libmpg123']
    flac_dep = codec_deps['libFLAC']
    ogg_dep = codec_deps['libogg']
    vorbisfile_dep = codec_deps['libvorbisfile']
    vorbis_dep = codec_deps['libvorbis']
    tremor_dep = dependency('', required : false)
    opus_dep = codec_deps['libopus']
    opusfile_dep = codec_deps['libopusfile']
else
    if get_option('mp3_lib') == 'mad'
        mad_dep = dependency('mad', required : get_option('use_codec_mp3'))
    else
        mpg123_dep = dependency('libmpg123', required : get_option('use_codec_mp3'))
    endif
    flac_dep = dependency('flac', required : get_option('use_codec_flac'))
    ogg_dep = dependency('ogg', required : get_option('use_codec_vorbis'))
    if get_option('vorbis_lib') == 'vorbis'
        vorbisfile_dep = dependency('vorbisfile', required : get_option('use_codec_vorbis'))
        vorbis_dep = dependency('vorbis', required : get_option('use_codec_vorbis'))
    else
        tremor_dep = dependency('vorbisidec', required : get_option('use_codec_vorbis'))
    endif
    opus_dep = dependency('opus', required : get_option('use_codec_opus'))
    opusfile_dep = dependency('opusfile', required : get_option('use_codec_opus'))
endif

if get_option('mp3_lib') == 'mad'
    if mad_dep.found()
        srcs += [ 'Quake/snd_mp3.c', 'Quake/snd_mp3tag.c' ]
        deps += mad_dep
        cflags += '-DUSE_CODEC_MP3'
    endif
else
    if mpg123_dep.found()
        srcs += [ 'Quake/snd_mpg123.c', 'Quake/snd_mp3tag.c' ]
        deps += mpg123_dep
        cflags += '-DUSE_CODEC_MP3'
    endif
endif

if flac_dep.found()
    srcs += 'Quake/snd_flac.c'
    deps += flac_dep
    cflags += '-DUSE_CODEC_FLAC'
endif

if get_option('vorbis_lib') == 'vorbis'
    if ogg_dep.found() and vorbisfile_dep.found() and vorbis_dep.found()
        srcs += 'Quake/snd_vorbis.c'
        deps += [ ogg_dep, vorbisfile_dep, vorbis_dep ]
        cflags += '-DUSE_CODEC_VORBIS'
    endif
else
    if ogg_dep.found() and tremor_dep.found()
        srcs += 'Quake/snd_vorbis.c'
        deps += [ ogg_dep, tremor_dep ]
        cflags += [ '-DUSE_CODEC_VORBIS', '-DVORBIS_USE_TREMOR' ]
    endif
endif

if opus_dep.found() and opusfile_dep.found()
    srcs += 'Quake/snd_opus.c'
    deps += [ opus_dep, opusfile_dep ]
    cflags += '-DUSE_CODEC_OPUS'
endif

if get_option('buildtype').startswith('debug')
    cflags += '-D_DEBUG'
endif

if get_option('do_userdirs').enabled()
    cflags += '-DDO_USERDIRS=1'
endif

incdirs = [
    'Quake/mimalloc',
]

maptool_libs = []
maptools_option = get_option('embedded_maptools')
maptools_available = not maptools_option.disabled() and add_languages('cpp', required : maptools_option, native : false)
# The compiler must have mutually compatible C++ and threading headers as
# well as a linkable runtime. A <vector>-only check misses broken toolchains
# whose stale pthread headers cannot compile the actual map-tool sources.
cpp_stdlib_probe = '''
#include <type_traits>
#include <vector>
#include <mutex>
#include <condition_variable>
static_assert(std::is_same<std::vector<int>::value_type, int>::value, "C++ standard library");
int main() {
    std::mutex mutex;
    std::condition_variable condition;
    std::unique_lock<std::mutex> lock(mutex);
    condition.notify_one();
    return 0;
}
'''
if maptools_available
    maptools_cpp = meson.get_compiler('cpp')
    maptools_available = maptools_cpp.links(cpp_stdlib_probe, dependencies : dependency('threads'), name : 'working C++ standard library and threading headers')
endif
if maptools_available
    subdir('ThirdParty/ericw-qbsp/embedded')
    maptool_libs += maptool_lib
    cflags += '-DHAVE_EMBEDDED_MAPTOOLS'
else
    if maptools_option.enabled()
        error('Embedded map tools require compatible C++ standard library and threading headers. Install a complete C++ compiler and configure a fresh build with CXX set to it (for example CXX=/usr/bin/g++ meson setup build), or explicitly use -Dembedded_maptools=disabled for an engine-only build.')
    elif not maptools_option.disabled()
        warning('Embedded editor tools require a working C++ compiler and standard library')
    endif
endif

vkquake_exe = executable(
    'vkquake',
    [srcs, shaders_c],
    dependencies : deps,
    include_directories : incdirs,
    c_args : cflags,
    link_with : maptool_libs,
    c_pch : 'Quake/quakedef.h',
    win_subsystem : 'windows')

if maptools_available
    test('Embedded map compiler pipeline', python,
        args : [files('tests/editor_embedded_pipeline_test.py'), vkquake_exe, files('Misc/editor_defaults.wad')])
endif

meshlet_test = executable('bsp_meshlet_test',
    ['tests/bsp_meshlet_test.c', 'Quake/bsp_meshlet.c'],
    include_directories : 'Quake',
    dependencies : cc.find_library('m', required : false))
test('BSP meshlet bake', meshlet_test)

weld_test = executable('bsp_vertex_weld_test',
    'tests/bsp_vertex_weld_test.c',
    include_directories : 'Quake',
    dependencies : cc.find_library('m', required : false))
test('BSP vertex weld', weld_test)

# Bundle the GPL-licensed ericw QBSP compiler for editor playtesting. A C++
# compiler with a working standard library is optional for engine-only builds.
# When available, the executable is built beside the engine.
configure_file(input : 'Misc/editor_defaults.wad', output : 'editor_defaults.wad', copy : true)
qbsp_cpp_available = add_languages('cpp', required : false, native : false)
if qbsp_cpp_available
    qbsp_cpp = meson.get_compiler('cpp')
    qbsp_cpp_available = qbsp_cpp.links(cpp_stdlib_probe, dependencies : dependency('threads'), name : 'working C++ standard library and threading headers')
endif
if qbsp_cpp_available
    qbsp_args = ['-DDOUBLEVEC_T', '-DERICWTOOLS_VERSION=v0.18.1']
    if host_machine.system() == 'windows'
        qbsp_args += ['-DUSE_WIN32THREADS', '-DWIN32', '-D_CRT_SECURE_NO_WARNINGS']
        if meson.get_compiler('cpp').get_id() == 'gcc'
            qbsp_args += '-D__USE_MINGW_ANSI_STDIO=1'
        endif
    else
        qbsp_args += ['-DUSE_PTHREADS', '-DLINUX']
    endif
    qbsp_sources = files(
        'ThirdParty/ericw-qbsp/common/threads.cc',
        'ThirdParty/ericw-qbsp/common/cmdlib.cc',
        'ThirdParty/ericw-qbsp/common/log.cc',
        'ThirdParty/ericw-qbsp/common/qvec.cc',
        'ThirdParty/ericw-qbsp/common/mathlib.cc',
        'ThirdParty/ericw-qbsp/common/polylib.cc',
        'ThirdParty/ericw-qbsp/qbsp/brush.cc',
        'ThirdParty/ericw-qbsp/qbsp/bspfile.cc',
        'ThirdParty/ericw-qbsp/qbsp/csg4.cc',
        'ThirdParty/ericw-qbsp/qbsp/file.cc',
        'ThirdParty/ericw-qbsp/qbsp/globals.cc',
        'ThirdParty/ericw-qbsp/qbsp/map.cc',
        'ThirdParty/ericw-qbsp/qbsp/merge.cc',
        'ThirdParty/ericw-qbsp/qbsp/outside.cc',
        'ThirdParty/ericw-qbsp/qbsp/parser.cc',
        'ThirdParty/ericw-qbsp/qbsp/portals.cc',
        'ThirdParty/ericw-qbsp/qbsp/qbsp.cc',
        'ThirdParty/ericw-qbsp/qbsp/solidbsp.cc',
        'ThirdParty/ericw-qbsp/qbsp/surfaces.cc',
        'ThirdParty/ericw-qbsp/qbsp/tjunc.cc',
        'ThirdParty/ericw-qbsp/qbsp/util.cc',
        'ThirdParty/ericw-qbsp/qbsp/wad.cc',
        'ThirdParty/ericw-qbsp/qbsp/winding.cc',
        'ThirdParty/ericw-qbsp/qbsp/writebsp.cc',
        'ThirdParty/ericw-qbsp/qbsp/exportobj.cc',
        'ThirdParty/ericw-qbsp/qbsp/main.cc')
    qbsp_exe = executable('quiver-qbsp', qbsp_sources,
        include_directories : 'ThirdParty/ericw-qbsp/include',
        cpp_args : qbsp_args,
        dependencies : dependency('threads'),
        override_options : ['cpp_std=c++11', 'warning_level=0', 'werror=false'])
    test('QBSP generated room', python, args : [files('tests/qbsp_smoke.py'), qbsp_exe, files('Misc/editor_defaults.wad')])
else
    warning('C++ compiler or standard library unavailable; bundled editor QBSP will not be built')
endif

# Standalone editor regressions; keep assertions enabled in release builds.
editor_map_test = executable('editor_map_test',
    ['tests/editor_map_test.c', 'Quake/editor_map.c'],
    include_directories : 'Quake',
    dependencies : cc.find_library('m', required : false),
    override_options : ['b_ndebug=false'])
test('Editor map document', editor_map_test)
editor_light_color_pipeline_test = executable('editor_light_color_pipeline_test',
    ['tests/editor_light_color_pipeline_test.c', 'Quake/editor_map.c', 'Quake/editor_sector.c', 'Quake/radiance_lights.c'],
    include_directories : 'Quake', dependencies : cc.find_library('m', required : false),
    override_options : ['b_ndebug=false'])
test('Editor saved light color and Radiance integration', editor_light_color_pipeline_test)
static_models_test = executable('static_models_test',
    ['tests/static_models_test.c', 'Quake/static_models.c'],
    include_directories : include_directories('Quake'), override_options : ['b_ndebug=false'])
test('Compiled static model format and BSPX', static_models_test)

static_models_collision_test = executable('static_models_collision_test',
    ['tests/static_models_collision_test.c', 'Quake/static_models_collision.c', 'Quake/radiance_scene.c'],
    include_directories : 'Quake', dependencies : cc.find_library('m', required : false),
    override_options : ['b_ndebug=false'])
test('Compiled static model continuous triangle collision', static_models_collision_test)
test('Compiled static model world trace integration', python,
    args : files('tests/static_models_world_test.py'))
test('Compiled static model client lifecycle', python,
    args : files('tests/static_models_client_test.py'))
test('Compiled static model compiler preparation', python,
    args : files('tests/static_models_compiler_test.py'))

editor_sector_test = executable('editor_sector_test',
    ['tests/editor_sector_test.c', 'Quake/editor_sector.c', 'Quake/editor_map.c'],
    include_directories : 'Quake',
    dependencies : cc.find_library('m', required : false),
    override_options : ['b_ndebug=false'])
test('Editor 3D sector document', editor_sector_test)
if maptools_available
    test('Editor sloped stacked sector compilation', python,
        args : [files('tests/editor_sector_compile_test.py'), editor_sector_test, vkquake_exe, files('Misc/editor_defaults.wad')])
endif

editor_sector_portal_test = executable('editor_sector_portal_test',
    ['tests/editor_sector_portal_test.c', 'Quake/editor_sector_portal.c', 'Quake/editor_sector.c', 'Quake/editor_map.c'],
    include_directories : 'Quake',
    dependencies : cc.find_library('m', required : false),
    override_options : ['b_ndebug=false'])
test('Editor 3D sector portals', editor_sector_portal_test)
if maptools_available
    test('Editor sector portal compilation', python,
        args : [files('tests/editor_sector_portal_compile_test.py'), vkquake_exe, editor_sector_portal_test, files('Misc/editor_defaults.wad')])
    editor_sector_intrusion_fixture = executable('editor_sector_intrusion_fixture',
        ['tests/editor_sector_intrusion_fixture.c', 'Quake/editor_sector_portal.c', 'Quake/editor_sector.c', 'Quake/editor_map.c'],
        include_directories : 'Quake',
        dependencies : cc.find_library('m', required : false),
        override_options : ['b_ndebug=false'])
    test('Editor slanted portal shell and collision', python,
        args : [files('tests/editor_sector_intrusion_test.py'), vkquake_exe, editor_sector_intrusion_fixture, files('Misc/editor_defaults.wad')])
    editor_sector_prop_fixture = executable('editor_sector_prop_fixture',
        ['tests/editor_sector_prop_fixture.c', 'Quake/editor_sector.c', 'Quake/editor_map.c'],
        include_directories : 'Quake',
        dependencies : cc.find_library('m', required : false),
        override_options : ['b_ndebug=false'])
    test('Editor solid sector props', python,
        args : [files('tests/editor_sector_prop_compile_test.py'), vkquake_exe, editor_sector_prop_fixture, files('Misc/editor_defaults.wad')])
endif

editor_process_test = executable('editor_process_test',
    ['tests/editor_process_test.c', 'Quake/editor_process.c'],
    override_options : ['b_ndebug=false'])
test('Editor compiler process', editor_process_test)

editor_tools_test = executable('editor_tools_test',
    ['tests/editor_tools_test.c', 'Quake/editor_tools.c'],
    include_directories : 'Quake',
    c_args : ['-DHAVE_EMBEDDED_MAPTOOLS'],
    override_options : ['b_ndebug=false'])
test('Editor embedded tool dispatch', editor_tools_test)

editor_ui_test = executable('editor_ui_test',
    ['tests/editor_ui_test.c', 'Quake/editor_ui.c', 'Quake/ui_control.c', 'Quake/editor_controls.c'],
    include_directories : 'Quake',
    dependencies : deps,
    c_args : cflags,
    override_options : ['b_ndebug=false'])
test('Editor viewport interaction', editor_ui_test)
test('Editor textured model preview projection and lifecycle', python,
    args : files('tests/editor_model_preview_test.py'))
test('Editor scripted playtest inherited game arguments', python,
    args : [files('tests/smoke_scripted_editor.py'), '--check-arguments'])

editor_orthographic_ui_test = executable('editor_orthographic_ui_test',
    ['tests/editor_orthographic_ui_test.c', 'Quake/editor_ui.c', 'Quake/ui_control.c', 'Quake/editor_controls.c'],
    include_directories : 'Quake',
    dependencies : deps,
    c_args : cflags,
    override_options : ['b_ndebug=false'])
test('Editor orthographic view interaction', editor_orthographic_ui_test)

ui_control_test = executable('ui_control_test',
    ['tests/ui_control_test.c', 'Quake/ui_control.c'],
    include_directories : 'Quake',
    dependencies : cc.find_library('m', required : false),
    override_options : ['b_ndebug=false'])
test('Retained UI controls', ui_control_test)

ecs_test = executable('ecs_test', ['tests/ecs_test.c', 'Quake/ecs.c'],
    include_directories : 'Quake', override_options : ['b_ndebug=false'])
test('ECS lifecycle and ordered iteration', ecs_test)
ecs_gpu_lighting_bounds_test = executable('ecs_gpu_lighting_bounds_test', 'tests/ecs_gpu_lighting_bounds_test.c',
    dependencies : cc.find_library('m', required : false), override_options : ['b_ndebug=false'])
test('ECS GPU lighting bounds', ecs_gpu_lighting_bounds_test)
test('ECS playable map asset', python, args : files('tests/ecs_playable_map_test.py'))

radiance_math_dep = cc.find_library('m', required : false)
if host_machine.system() == 'linux' and cc.has_multi_arguments('-ffunction-sections', '-fdata-sections') and cc.has_link_argument('-Wl,--gc-sections')
    # The fault-injection fixtures compile the real static renderer helpers
    # with a mocked Vulkan interface; unused engine code is discarded.
    radiance_clear_barrier_test = executable('radiance_clear_barrier_test',
        ['tests/radiance_clear_barrier_test.c', 'Quake/radiance_vulkan.c'],
        include_directories : 'Quake', dependencies : [vulkan_dep],
        c_args : ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Radiance cascade clear barriers', radiance_clear_barrier_test)
    color_target_fault_test = executable('vulkan_color_target_test', 'tests/vulkan_color_target_test.c',
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan color target failure cleanup', color_target_fault_test)
    color_target_debug_fault_test = executable('vulkan_color_target_debug_test', 'tests/vulkan_color_target_test.c',
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-D_DEBUG', '-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan debug ray-query descriptor failure cleanup', color_target_debug_fault_test)
    memory_fault_test = executable('vulkan_memory_transaction_test', 'tests/vulkan_memory_transaction_test.c',
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan memory failure accounting', memory_fault_test)
    framebuffer_fault_test = executable('vulkan_framebuffer_transaction_test', 'tests/vulkan_framebuffer_transaction_test.c',
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan framebuffer failure cleanup', framebuffer_fault_test)
    renderpass_fault_test = executable('vulkan_renderpass_transaction_test', 'tests/vulkan_renderpass_transaction_test.c',
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan render-pass failure cleanup', renderpass_fault_test)
    shader_transaction_test = executable('vulkan_shader_transaction_test', 'tests/vulkan_shader_transaction_test.c',
        dependencies : vulkan_dep, override_options : ['b_ndebug=false'])
    test('Vulkan shader module failure cleanup', shader_transaction_test)
    shader_stubs = custom_target('vulkan_shader_test_stubs', input : 'Shaders/shaders.h',
        output : 'vulkan_shader_test_stubs.c',
        command : [python, files('tests/generate_shader_stubs.py'), '@INPUT@', '@OUTPUT@'])
    renderer_shader_transaction_test = executable('vulkan_renderer_shader_transaction_test',
        ['tests/vulkan_renderer_shader_transaction_test.c', shader_stubs],
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan renderer shader module failure cleanup', renderer_shader_transaction_test)
    renderer_debug_shader_transaction_test = executable('vulkan_renderer_debug_shader_transaction_test',
        ['tests/vulkan_renderer_shader_transaction_test.c', shader_stubs],
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-D_DEBUG', '-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan debug renderer shader module failure cleanup', renderer_debug_shader_transaction_test)
    renderer_pipeline_transaction_test = executable('vulkan_renderer_pipeline_transaction_test',
        ['tests/vulkan_renderer_pipeline_transaction_test.c', shader_stubs],
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan renderer pipeline failure cleanup', renderer_pipeline_transaction_test)
    renderer_debug_pipeline_transaction_test = executable('vulkan_renderer_debug_pipeline_transaction_test',
        ['tests/vulkan_renderer_pipeline_transaction_test.c', shader_stubs],
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-D_DEBUG', '-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan debug renderer pipeline failure cleanup', renderer_debug_pipeline_transaction_test)
    ssao_transaction_test = executable('vulkan_ssao_transaction_test', 'tests/vulkan_ssao_transaction_test.c',
        include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
        c_args : cflags + ['-ffunction-sections', '-fdata-sections'], link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Vulkan SSAO resource failure cleanup', ssao_transaction_test)
endif
foreach receiver_test : [
    ['receiver_targets', 'radiance_receiver_targets'],
    ['receiver_pipelines', 'radiance_receiver_pipelines'],
    ['receiver_record', 'radiance_receiver_record'],
    ['receiver_world', 'radiance_receiver_world'],
]
    candidate = executable(receiver_test[0] + '_test',
        ['tests/' + receiver_test[1] + '_test.c', 'Quake/' + receiver_test[1] + '.c'],
        include_directories : 'Quake', dependencies : radiance_math_dep,
        override_options : ['b_ndebug=false'])
    test('Prototype ' + receiver_test[0], candidate)
endforeach
foreach receiver_test : ['descriptors', 'layouts', 'select', 'resources', 'stage', 'shaders', 'slot', 'live']
    candidate = executable('receiver_' + receiver_test + '_test',
        ['tests/radiance_receiver_' + receiver_test + '_test.c', 'Quake/radiance_receiver_' + receiver_test + '.c'],
        include_directories : 'Quake', override_options : ['b_ndebug=false'])
    test('Prototype receiver ' + receiver_test, candidate)
endforeach
receiver_diagnostics_test = executable('receiver_diagnostics_test',
    ['tests/radiance_receiver_diagnostics_test.c', 'Quake/radiance_receiver_diagnostics.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance receiver diagnostic lifecycle', receiver_diagnostics_test)
receiver_alternates_test = executable('receiver_alternates_test',
    ['tests/radiance_receiver_alternates_test.c', 'Quake/radiance_receiver_alternates.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Prototype receiver alternates', receiver_alternates_test)
receiver_capture_test = executable('receiver_capture_test',
    ['tests/radiance_receiver_capture_test.c', 'Quake/radiance_receiver_capture.c'],
    include_directories : 'Quake', override_options : ['b_ndebug=false'])
test('Prototype receiver capture', receiver_capture_test)
receiver_model_packet_test = executable('receiver_model_packet_test',
    ['tests/radiance_receiver_model_test.c', 'Quake/radiance_receiver_model.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance model receiver packets and lifecycle', receiver_model_packet_test)
test('Radiance native alias zero-alpha flags', python,
    args : files('tests/radiance_alias_alpha_flags_test.py'))
receiver_integration_test = executable('receiver_integration_test',
    ['tests/radiance_receiver_integration_test.c', 'Quake/radiance_receiver_snapshot.c',
     'Quake/radiance_receiver_frame.c', 'Quake/radiance_receiver_record.c',
     'Quake/radiance_receiver_world.c', 'Quake/radiance_receiver_forward.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Prototype receiver integration', receiver_integration_test)
receiver_snapshot_test = executable('receiver_snapshot_test',
    ['tests/radiance_receiver_snapshot_test.c', 'Quake/radiance_receiver_snapshot.c'],
    include_directories : 'Quake', override_options : ['b_ndebug=false'])
test('Prototype receiver snapshot', receiver_snapshot_test)
receiver_forward_test = executable('receiver_forward_test',
    ['tests/radiance_receiver_forward_test.c', 'Quake/radiance_receiver_forward.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Prototype receiver forward', receiver_forward_test)
receiver_frame_test = executable('receiver_frame_test',
    ['tests/radiance_receiver_frame_test.c', 'Quake/radiance_receiver_frame.c'],
    include_directories : 'Quake', override_options : ['b_ndebug=false'])
test('Prototype receiver frame gate', receiver_frame_test)
receiver_reconstruct_equivalence_spv = custom_target('receiver_reconstruct_equivalence_spv',
    input : 'tests/radiance_receiver_reconstruct_equivalence_test.comp', output : 'receiver_reconstruct_equivalence.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_receiver_scale.glsl', 'Shaders/radiance_receiver_resolve.glsl', 'Shaders/radiance_receiver_checkerboard.glsl', 'Shaders/radiance_receiver_checkerboard_batched.glsl'),
    build_by_default : true)
receiver_reconstruct_equivalence_test = executable('receiver_reconstruct_equivalence_test',
    'tests/radiance_receiver_resolve_test.c', dependencies : vulkan_dep, override_options : ['b_ndebug=false'])
test('Radiance checkerboard reconstruction equivalence GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_reconstruct_equivalence_spv, '--gated'], timeout : 240)
receiver_scale_spv = custom_target('receiver_scale_spv',
    input : 'tests/radiance_receiver_scale_test.comp', output : 'receiver_scale.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_receiver_scale.glsl', 'Shaders/radiance_receiver_classify.glsl', 'Shaders/radiance_receiver_resolve.glsl', 'Shaders/radiance_receiver_checkerboard.glsl'),
    build_by_default : true)
test('Radiance receiver scales GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_scale_spv, '--gated'], timeout : 240)
foreach rational_mode : ['legacy', 'texture']
    rational_flags = rational_mode == 'texture' ? ['-DRC_RECEIVER_TEXTURE_FILTER=1'] : []
    rational_spv = custom_target('receiver_rational_' + rational_mode + '_spv',
        input : 'tests/radiance_receiver_rational_scale_test.comp', output : 'receiver_rational_' + rational_mode + '.spv',
        command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders'] + rational_flags + ['-o', '@OUTPUT@', '@INPUT@'],
        depend_files : files('Shaders/radiance_receiver_scale.glsl', 'Shaders/radiance_receiver_classify.glsl',
            'Shaders/radiance_receiver_resolve.glsl', 'Shaders/radiance_receiver_guides.glsl', 'Shaders/radiance_receiver_checkerboard.glsl'),
        build_by_default : true)
    test('Radiance receiver rational scale ' + rational_mode + ' GPU', receiver_reconstruct_equivalence_test,
        args : [rational_spv, '--gated'], timeout : 240)
endforeach
receiver_model_spv = custom_target('receiver_model_spv',
    input : 'tests/radiance_receiver_model_test.comp', output : 'receiver_model.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/alias_common.inc', 'Shaders/radiance_model_normal.glsl', 'Shaders/radiance_highlight.glsl',
        'Shaders/radiance_receiver_classify.glsl',
        'Shaders/radiance_receiver_scale.glsl', 'Shaders/radiance_receiver_resolve.glsl',
        'Shaders/radiance_receiver_guides.glsl', 'Shaders/radiance_receiver_checkerboard.glsl'),
    build_by_default : true)
test('Radiance model receiver composition GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_model_spv, '--gated'], timeout : 240)
test('Radiance model receiver shader interfaces', python,
    args : [files('tests/radiance_receiver_model_shader_test.py')], timeout : 120)
receiver_texture_filter_spv = custom_target('receiver_texture_filter_spv',
    input : 'tests/radiance_receiver_texture_filter_test.comp', output : 'receiver_texture_filter.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_receiver_scale.glsl', 'Shaders/radiance_receiver_resolve.glsl', 'Shaders/radiance_receiver_guides.glsl',
        'Shaders/radiance_receiver_texture_filter.glsl', 'Shaders/radiance_receiver_checkerboard.glsl'),
    build_by_default : true)
test('Radiance receiver texture filter GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_texture_filter_spv, '--gated'], timeout : 240)
receiver_filter_axis_spv = custom_target('receiver_filter_axis_spv',
    input : 'tests/radiance_receiver_filter_axis_test.comp', output : 'receiver_filter_axis.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_receiver_scale.glsl', 'Shaders/radiance_receiver_guides.glsl', 'Shaders/radiance_receiver_classify.glsl',
        'Shaders/radiance_receiver_checkerboard.glsl', 'Shaders/radiance_receiver_texture_filter.glsl',
        'Shaders/radiance_receiver_filter_axis.glsl', 'Shaders/radiance_filter.glsl'),
    build_by_default : true)
test('Radiance receiver separable Gaussian axis GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_filter_axis_spv, '--gated'], timeout : 240)
receiver_shadows_spv = custom_target('receiver_shadows_spv',
    input : 'tests/radiance_receiver_shadows_test.comp', output : 'receiver_shadows.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_receiver_shadows.glsl', 'Shaders/radiance_receiver_shadow_metadata.glsl',
        'Shaders/radiance_receiver_shadow_source.glsl', 'Shaders/radiance_light_unshadowed.glsl', 'Shaders/radiance_parameters.glsl'),
    build_by_default : true)
test('Radiance receiver contact-hardening shadows GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_shadows_spv, '--gated'], timeout : 240)
receiver_shadow_filter_spv = custom_target('receiver_shadow_filter_spv',
    input : 'tests/radiance_receiver_shadow_filter_test.comp', output : 'receiver_shadow_filter.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_receiver_shadow_filter.glsl', 'Shaders/radiance_light_query_free.glsl',
        'Shaders/radiance_light_unshadowed.glsl', 'Shaders/radiance_receiver_guides.glsl',
        'Shaders/radiance_receiver_scale.glsl', 'Shaders/radiance_receiver_checkerboard.glsl', 'Shaders/radiance_filter.glsl'),
    build_by_default : true)
test('Radiance receiver selected-source shadow filter GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_shadow_filter_spv, '--gated'], timeout : 240)
receiver_texture_lookup_spv = custom_target('receiver_texture_lookup_spv',
    input : 'tests/radiance_receiver_texture_lookup_test.comp', output : 'receiver_texture_lookup.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_lookup.glsl', 'Shaders/radiance_filter.glsl', 'Shaders/radiance_cascades.glsl'),
    build_by_default : true)
test('Radiance receiver texture lookup GPU', receiver_reconstruct_equivalence_test,
    args : [receiver_texture_lookup_spv, '--gated'], timeout : 240)
test('Radiance receiver texture filter shaders', python, args : files('tests/radiance_receiver_texture_filter_shader_test.py'))
# Grouped receiver visibility must match the original independent probes.
receiver_visibility_batch_test = executable('receiver_visibility_batch_test',
    'tests/radiance_receiver_visibility_test.c', dependencies : vulkan_dep, override_options : ['b_ndebug=false'])
foreach visibility_mode : ['merged', 'raw']
    visibility_args = visibility_mode == 'raw' ? ['-DRC_INDIRECT_BOUNCE=0'] : []
    visibility_spv = custom_target('receiver_visibility_' + visibility_mode + '_spv',
        input : 'tests/radiance_receiver_visibility_test.comp', output : 'receiver_visibility_' + visibility_mode + '.spv',
        command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders'] + visibility_args + ['-o', '@OUTPUT@', '@INPUT@'],
        depend_files : files('Shaders/radiance_receiver_visibility.glsl'), build_by_default : true)
    test('Radiance receiver visibility batch ' + visibility_mode + ' GPU', receiver_visibility_batch_test,
        args : [visibility_spv, '--gated'], timeout : 240)
endforeach
direction_bin_spv = custom_target('radiance_direction_bin_test_spv',
    input : 'tests/radiance_direction_bin_test.comp', output : 'radiance_direction_bin_test.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_lookup.glsl', 'Shaders/radiance_filter.glsl'), build_by_default : true)
test('Radiance direction bin GPU', receiver_visibility_batch_test,
    args : [direction_bin_spv, '--gated'], timeout : 240)
radiance_core_test = executable('radiance_cascades_test', ['tests/radiance_cascades_test.c', 'Quake/radiance_cascades.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance cascade layout and scheduling', radiance_core_test)
radiance_sparse_test = executable('radiance_sparse_test',
    ['tests/radiance_sparse_test.c', 'Quake/radiance_sparse.c', 'Quake/radiance_cascades.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Sparse radiance brick coverage', radiance_sparse_test)
radiance_coverage_test = executable('radiance_coverage_test',
    ['tests/radiance_coverage_test.c', 'Quake/radiance_sparse.c', 'Quake/radiance_cascades.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance receiver spacing', radiance_coverage_test)
radiance_sparse_refresh_test = executable('radiance_sparse_refresh_test',
    ['tests/radiance_sparse_refresh_test.c', 'Quake/radiance_sparse.c', 'Quake/radiance_cascades.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Sparse radiance temporal refresh', radiance_sparse_refresh_test)
radiance_lights_test = executable('radiance_lights_test', ['tests/radiance_lights_test.c', 'Quake/radiance_lights.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance runtime light sources', radiance_lights_test)
radiance_material_atlas_test = executable('radiance_material_atlas_test',
    ['tests/radiance_material_atlas_test.c', 'Quake/radiance_material_atlas.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance material atlas', radiance_material_atlas_test)
radiance_materials_quake_test = executable('radiance_materials_quake_test',
    ['tests/radiance_materials_quake_test.c', 'Quake/radiance_materials_quake.c', 'Quake/radiance_material_atlas.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    override_options : ['b_ndebug=false'])
test('Radiance Quake texture sources', radiance_materials_quake_test)
radiance_materials_inventory_test = executable('radiance_materials_inventory_test',
    ['tests/radiance_materials_inventory_test.c', 'Quake/radiance_materials_quake.c', 'Quake/radiance_material_atlas.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    override_options : ['b_ndebug=false'])
test('Radiance immutable animation atlas inventory', radiance_materials_inventory_test)
radiance_coordinator_test = executable('radiance_coordinator_test',
    ['tests/radiance_coordinator_test.c', 'Quake/radiance_coordinator.c', 'Quake/radiance_lights.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    override_options : ['b_ndebug=false'])
test('Radiance input coordinator', radiance_coordinator_test)
radiance_matrix_test = executable('radiance_matrix_test',
    ['tests/radiance_matrix_test.c', 'Quake/radiance_matrix.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance matrix transforms', radiance_matrix_test)
radiance_emissive_test = executable('radiance_emissive_test',
    ['tests/radiance_emissive_test.c', 'Quake/radiance_emissive.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance emissive surface sources', radiance_emissive_test)
test('Radiance menu', python, args : files('tests/radiance_menu_test.py'))
test('Radiance frame selection gate', python, args : files('tests/radiance_graphics_gate_test.py'))
test('Radiance receiver task publication order', python, args : files('tests/radiance_receiver_task_order_test.py'))
test('Radiance receiver camera capture lifetime', python, args : files('tests/radiance_receiver_capture_view_test.py'))
test('Radiance live probe spacing transaction', python, args : files('tests/radiance_probe_spacing_live_test.py'))
test('Radiance frame submission', python, args : files('tests/radiance_frame_submission_test.py'))
test('Radiance resolution scale', python, args : files('tests/radiance_resolution_scale_test.py'))
test('Radiance shadow controls and ABI', python, args : files('tests/radiance_shadow_parameters_test.py'))
test('Radiance completed receiver diagnostics', python, args : files('tests/radiance_receiver_diagnostics_runtime_test.py'))
test('Radiance screen effects constants', python, args : files('tests/radiance_screen_effect_constants_test.py'))
test('Radiance lazy backend lifecycle', python, args : files('tests/radiance_backend_lifecycle_test.py'))
test('Radiance player entity identity', python, args : files('tests/radiance_player_entity_test.py'))
test('Radiance benchmark interpretation', python, args : files('tests/bench_radiance_test.py'))
test('Radiance soft shadow benchmark guards', python, args : files('tests/bench_radiance_soft_shadows_test.py'))
test('GPU frame time reporting', python, args : files('tests/gpu_frame_time_test.py'))
radiance_scene_test = executable('radiance_scene_test', ['tests/radiance_scene_test.c', 'Quake/radiance_scene.c'],
    include_directories : 'Quake', dependencies : radiance_math_dep, override_options : ['b_ndebug=false'])
test('Radiance software scene BVH', radiance_scene_test)
radiance_alias_test = executable('radiance_alias_test',
    ['tests/radiance_alias_test.c', 'Quake/radiance_alias.c', 'Quake/radiance_scene.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    override_options : ['b_ndebug=false'])
test('Radiance alias geometry retention', radiance_alias_test)
radiance_scene_alias_test = executable('radiance_scene_alias_test',
    ['tests/radiance_scene_alias_test.c', 'Quake/radiance_scene_alias.c', 'Quake/radiance_alias.c', 'Quake/radiance_scene.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    override_options : ['b_ndebug=false'])
test('Radiance alias scene extraction', radiance_scene_alias_test)
radiance_scene_bsp_test = executable('radiance_scene_bsp_test',
    ['tests/radiance_scene_bsp_test.c', 'Quake/radiance_scene_bsp.c', 'Quake/radiance_scene.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    override_options : ['b_ndebug=false'])
test('Radiance BSP scene extraction', radiance_scene_bsp_test)
# This fixture exercises real brush extraction, the cached emissive proxy, and
# receiver/visibility shaders. GPU execution is opt-in via QUIVER_TEST_VULKAN.
brush_emission_shader_dependencies = files('Shaders/radiance_scene_trace.glsl', 'Shaders/radiance_lighting.glsl')
# Exercise the general scene tracer and receiver shader as well. In particular,
# this covers map attenuation and spotlight cones on both traversal backends.
scene_gpu_sw_spv = custom_target('radiance_scene_gpu_sw_spv',
    input : 'tests/radiance_scene_gpu_test.comp', output : 'radiance_scene_gpu_sw.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : brush_emission_shader_dependencies, build_by_default : true)
scene_gpu_rt_spv = custom_target('radiance_scene_gpu_rt_spv',
    input : 'tests/radiance_scene_gpu_test.comp', output : 'radiance_scene_gpu_rt.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.2', '-DRC_HARDWARE_RAY_QUERY=1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : brush_emission_shader_dependencies, build_by_default : true)
scene_gpu_sources = ['tests/radiance_scene_gpu_test.c', 'Quake/radiance_scene_vulkan.c',
    'Quake/radiance_scene.c', 'Quake/radiance_gpu_inputs.c']
scene_gpu_sw_test = executable('radiance_scene_gpu_sw_test', scene_gpu_sources,
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    c_args : cflags, override_options : ['b_ndebug=false'])
scene_gpu_rt_test = executable('radiance_scene_gpu_rt_test', scene_gpu_sources + ['Quake/radiance_scene_rt.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    c_args : cflags + ['-DRC_TEST_HARDWARE'], override_options : ['b_ndebug=false'])
test('Radiance scene software GPU', scene_gpu_sw_test, args : [scene_gpu_sw_spv, '--gated'], timeout : 240)
test('Radiance scene ray-query GPU', scene_gpu_rt_test, args : [scene_gpu_rt_spv, '--gated'], timeout : 240)
radiance_input_shader_dependencies = files('Shaders/radiance_cascades.glsl', 'Shaders/radiance_scene_trace.glsl',
    'Shaders/radiance_lookup.glsl', 'Shaders/radiance_filter.glsl', 'Shaders/radiance_lighting.glsl')
radiance_filter_spv = custom_target('radiance_filter_test_spv',
    input : 'tests/radiance_filter_test.comp', output : 'radiance_filter_test.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_filter.glsl'), build_by_default : true)
radiance_filter_test = executable('radiance_filter_gpu_test', 'tests/radiance_filter_gpu_test.c',
    dependencies : [vulkan_dep, radiance_math_dep], c_args : cflags, override_options : ['b_ndebug=false'])
test('Radiance filter GPU', radiance_filter_test, args : [radiance_filter_spv, '--gated'], timeout : 240)
radiance_test_bounce_spv = custom_target('radiance_test_bounce_spv',
    input : 'tests/radiance_bounce_test.comp', output : 'radiance_test_bounce.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : radiance_input_shader_dependencies, build_by_default : true)
radiance_test_receiver_spv = custom_target('radiance_test_receiver_spv',
    input : 'tests/radiance_bounce_test.comp', output : 'radiance_test_receiver.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-DTEST_RECEIVER=1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : radiance_input_shader_dependencies, build_by_default : true)
radiance_test_topology_spv = custom_target('radiance_test_topology_spv',
    input : 'tests/radiance_topology_trace_test.comp', output : 'radiance_test_topology.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : files('Shaders/radiance_cascades.glsl'), build_by_default : true)
radiance_input_gpu_test = executable('radiance_input_gpu_test',
    ['tests/radiance_gpu_inputs_test.c', 'Quake/radiance_gpu_inputs.c', 'Quake/radiance_vulkan.c',
     'Quake/radiance_sparse.c', 'Quake/radiance_cascades.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    c_args : cflags, override_options : ['b_ndebug=false'])
test('Radiance sparse GPU merge', radiance_input_gpu_test,
    args : [radiance_test_trace_spv, radiance_test_merge_spv, radiance_test_bounce_spv,
            radiance_test_receiver_spv, radiance_test_topology_spv, '--gated'], timeout : 240)
brush_emission_sw_spv = custom_target('radiance_brush_emission_sw_spv',
    input : 'tests/radiance_brush_emission_gpu_test.comp', output : 'radiance_brush_emission_sw.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : brush_emission_shader_dependencies, build_by_default : true)
brush_emission_rt_spv = custom_target('radiance_brush_emission_rt_spv',
    input : 'tests/radiance_brush_emission_gpu_test.comp', output : 'radiance_brush_emission_rt.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.2', '-DRC_HARDWARE_RAY_QUERY=1', '-I' + meson.current_source_dir() / 'Shaders', '-o', '@OUTPUT@', '@INPUT@'],
    depend_files : brush_emission_shader_dependencies, build_by_default : true)
brush_emission_sources = ['tests/radiance_brush_emission_gpu_test.c', 'Quake/radiance_scene_bsp.c',
    'Quake/radiance_emissive.c', 'Quake/radiance_scene_vulkan.c', 'Quake/radiance_scene.c', 'Quake/radiance_gpu_inputs.c']
brush_emission_sw_test = executable('radiance_brush_emission_sw_test', brush_emission_sources,
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    c_args : cflags, override_options : ['b_ndebug=false'])
brush_emission_rt_test = executable('radiance_brush_emission_rt_test', brush_emission_sources + ['Quake/radiance_scene_rt.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    c_args : cflags + ['-DRC_TEST_HARDWARE'], override_options : ['b_ndebug=false'])
test('Radiance brush emissive software GPU', brush_emission_sw_test, args : [brush_emission_sw_spv, '--gated'])
test('Radiance brush emissive ray-query GPU', brush_emission_rt_test, args : [brush_emission_rt_spv, '--gated'])
test('Radiance omitted emission gain zero software GPU', brush_emission_sw_test,
    args : [brush_emission_sw_spv, '--gated'], env : ['QUIVER_TEST_EMISSIVE_GAIN=0'], timeout : 240)
test('Radiance omitted emission gain two software GPU', brush_emission_sw_test,
    args : [brush_emission_sw_spv, '--gated'], env : ['QUIVER_TEST_EMISSIVE_GAIN=2'], timeout : 240)
radiance_scene_runtime_test = executable('radiance_scene_runtime_test',
    ['tests/radiance_scene_runtime_test.c', 'Quake/radiance_scene_runtime.c', 'Quake/radiance_scene_bsp.c', 'Quake/radiance_scene.c'],
    include_directories : ['Quake', 'Quake/mimalloc'], dependencies : [sdl_dep, vulkan_dep, radiance_math_dep],
    override_options : ['b_ndebug=false'])
test('Radiance dynamic occluder selection', radiance_scene_runtime_test)

executable('ecs_stress_bench', ['tests/ecs_stress_bench.c', 'Quake/ecs.c'],
    include_directories : 'Quake', build_by_default : false)

# The focused VM adapter harness links the real edict implementation while
# discarding unrelated engine functions. GNU/LLVM section GC is required.
if host_machine.system() == 'linux' and cc.get_id() in ['gcc', 'clang'] and \
        cc.has_multi_arguments('-ffunction-sections', '-fdata-sections') and cc.has_link_argument('-Wl,--gc-sections')
    map_description_test = executable('map_description_test',
        ['tests/map_description_test.c', 'Quake/gl_model.c'],
        include_directories : 'Quake',
        dependencies : [sdl_dep, vulkan_dep, cc.find_library('m', required : false)],
        c_args : ['-ffunction-sections', '-fdata-sections'],
        link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Embedded and packed map descriptions', map_description_test)
    ecs_qcvm_test = executable('ecs_qcvm_test',
        ['tests/ecs_qcvm_test.c', 'Quake/pr_edict.c', 'Quake/ecs.c'],
        include_directories : 'Quake',
        dependencies : [sdl_dep, vulkan_dep, cc.find_library('m', required : false)],
        c_args : ['-ffunction-sections', '-fdata-sections'],
        link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('ECS isolated QuakeC VM lifecycle', ecs_qcvm_test)

    ecs_client_relink_test = executable('ecs_client_relink_test',
        ['tests/ecs_client_relink_test.c', 'Quake/ecs.c', 'Quake/mathlib.c'],
        include_directories : 'Quake',
        dependencies : [sdl_dep, vulkan_dep, cc.find_library('m', required : false)],
        c_args : ['-ffunction-sections', '-fdata-sections'],
        link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('ECS client relink behavior', ecs_client_relink_test)
    radiance_temp_entity_identity_test = executable('radiance_temp_entity_identity_test',
        ['tests/radiance_temp_entity_identity_test.c', 'Quake/cl_tent.c'],
        include_directories : 'Quake',
        dependencies : [sdl_dep, vulkan_dep, cc.find_library('m', required : false)],
        c_args : ['-ffunction-sections', '-fdata-sections'],
        link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Radiance temporary entity identity', radiance_temp_entity_identity_test)
    radiance_demo_seek_identity_test = executable('radiance_demo_seek_identity_test',
        ['tests/radiance_demo_seek_identity_test.c', 'Quake/cl_demo.c', 'Quake/cl_tent.c'],
        include_directories : 'Quake',
        dependencies : [sdl_dep, vulkan_dep, cc.find_library('m', required : false)],
        c_args : ['-ffunction-sections', '-fdata-sections'],
        link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('Radiance demo seek entity identity', radiance_demo_seek_identity_test)
    net_entity_width_test = executable('net_entity_width_test',
        ['tests/net_entity_width_test.c', 'Quake/common.c', 'Quake/sv_main.c'],
        include_directories : 'Quake',
        dependencies : [sdl_dep, vulkan_dep, cc.find_library('m', required : false)],
        c_args : ['-ffunction-sections', '-fdata-sections'],
        link_args : ['-Wl,--gc-sections'],
        override_options : ['b_ndebug=false'])
    test('PEXT2 wide entity indices', net_entity_width_test)
endif

# Native QuiverScript tools. FTEQCC is built from its pinned upstream sources;
# the compiler is a separate program so the engine remains C-only and does not
# gain a scripting or Python runtime dependency.
quiverscript_tool = executable('quiverscript', 'Tools/quiverscript.c', install : true)
fteqcc_src = [
    'qccmain.c', 'qcc_pr_comp.c', 'qcc_pr_lex.c', 'packager.c', 'decomp.c',
    'comprout.c', 'hash.c', 'qcc_cmdlib.c', 'qcd_main.c', 'qcctui.c',
]
fteqcc_files = []
foreach filename : fteqcc_src
    fteqcc_files += files('ThirdParty/fteqcc/' + filename)
endforeach
fteqcc = executable('fteqcc', fteqcc_files,
    dependencies : [cc.find_library('m', required : false)],
    c_args : ['-DNO_ZLIB'] + cc.get_supported_arguments('-Wno-pointer-sign'),
    override_options : ['werror=false', 'c_std=gnu17'], install : true)
qc_source_files = [
    'ai.qc', 'boss.qc', 'buttons.qc', 'client.qc', 'combat.qc', 'defs.qc',
    'demon.qc', 'dog.qc', 'doors.qc', 'enforcer.qc', 'fight.qc', 'fish.qc',
    'hknight.qc', 'items.qc', 'knight.qc', 'misc.qc', 'monsters.qc', 'ogre.qc',
    'oldone.qc', 'plats.qc', 'player.qc', 'shalrath.qc', 'shambler.qc',
    'soldier.qc', 'subs.qc', 'tarbaby.qc', 'triggers.qc', 'weapons.qc',
    'wizard.qc', 'world.qc', 'zombie.qc', 'progs.src', 'LICENSE',
]
qc_source_deps = []
foreach filename : qc_source_files
    qc_source_deps += files('ThirdParty/id1-quakec/' + filename)
endforeach
custom_target('quiverscript-game-sources', output : 'quiverscript-game-sources.stamp',
    command : [python, files('Tools/stage_quakec.py'),
        meson.project_source_root() / 'ThirdParty/id1-quakec',
        meson.project_build_root() / 'id1-quakec', '@OUTPUT@'],
    depend_files : qc_source_deps, build_by_default : true)
install_subdir('ThirdParty/id1-quakec', install_dir : get_option('bindir'))
test('QuiverScript package compilation', python,
    args : [files('tests/quiverscript_test.py'), quiverscript_tool, fteqcc,
        meson.project_source_root() / 'ThirdParty/id1-quakec', files('Misc/scripts/ecs_ripple.qs')])
ripple_package = custom_target('quiverscript-ripple-package', output : 'quiverscript-ripple-package.stamp',
    command : [python, files('Tools/build_ripple_package.py'), quiverscript_tool, fteqcc,
        files('Misc/scripts/ecs_ripple.qs'), meson.project_source_root() / 'ThirdParty/id1-quakec',
        meson.project_build_root() / 'quiver_ripple', '@OUTPUT@'],
    depend_files : qc_source_deps, build_by_default : true)
test('Ripple marker model', python, args : files('tests/ripple_model_test.py'))
