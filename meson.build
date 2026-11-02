project('vkquake', 'c', 'cpp', meson_version : '>=1.3.0', default_options : ['c_std=gnu11', 'cpp_std=gnu++26', 'buildtype=release', 'b_ndebug=if-release'])

cc = meson.get_compiler('cpp')
cc_c = meson.get_compiler('c')
fs = import('fs')

if cc.has_header_symbol('sched.h', 'CPU_ZERO', prefix: '#define _GNU_SOURCE')
    message('Info: Task affinity support enabled.')
    add_project_arguments('-D_GNU_SOURCE', language: 'cpp')
else
    message('Warning: sched.h not found, disabling Task affinity support.')
    add_project_arguments('-DTASK_AFFINITY_NOT_AVAILABLE', language: 'cpp')
endif

shaders = [
    'Shaders/alias.frag',
    'Shaders/alias.vert',
    'Shaders/alias_alphatest.frag',
    'Shaders/md5.vert',
    'Shaders/basic.frag',
    'Shaders/basic.vert',
    'Shaders/basic_sky.vert',
    'Shaders/basic_alphatest.frag',
    'Shaders/basic_notex.frag',
    'Shaders/cs_tex_warp.comp',
    'Shaders/indirect.comp',
    'Shaders/indirect_clear.comp',
    'Shaders/postprocess.frag',
    'Shaders/postprocess.vert',
    'Shaders/screen_effects_10bit.comp',
    'Shaders/screen_effects_10bit_scale.comp',
    'Shaders/screen_effects_10bit_scale_sops.comp',
    'Shaders/screen_effects_8bit.comp',
    'Shaders/screen_effects_8bit_scale.comp',
    'Shaders/screen_effects_8bit_scale_sops.comp',
    'Shaders/showtris.frag',
    'Shaders/showtris.vert',
    'Shaders/sky_box.frag',
    'Shaders/sky_cube.frag',
    'Shaders/sky_cube.vert',
    'Shaders/sky_layer.frag',
    'Shaders/sky_layer.vert',
    'Shaders/sky_atmosphere.frag',
    'Shaders/sky_atmosphere.vert',
    'Shaders/sky_clouds.frag',
    'Shaders/sky_clouds_msaa.frag',
    'Shaders/sky_clouds.vert',
    'Shaders/sky_atmosphere_lut.comp',
    'Shaders/sky_clouds_reduced.vert',
    'Shaders/sky_clouds_reduced.frag',
    'Shaders/sky_clouds_reduced_msaa.frag',
    'Shaders/sky_clouds_composite.frag',
    'Shaders/sky_clouds_composite_msaa.frag',
    'Shaders/update_lightmap_10bit.comp',
    'Shaders/update_lightmap_10bit_rt.comp',
    'Shaders/update_lightmap_8bit.comp',
    'Shaders/update_lightmap_8bit_rt.comp',
    'Shaders/world.frag',
    'Shaders/world.vert',
    'Shaders/terrain.frag',
    'Shaders/terrain.vert',
    'Shaders/ray_debug.comp',
]

glslang = find_program('glslangValidator')
spirv_opt = find_program('spirv-opt')
spirv_remap_command = []
spirv_opt_command  = []
run_test_canonicalize_ids_spirv_opt = run_command(spirv_opt, '-h', check : false)
has_canonicalize_ids_spirv_opt = (run_test_canonicalize_ids_spirv_opt.stdout().strip().contains('--canonicalize-ids'))
bintoc = executable('bintoc', ['Shaders/bintoc.c'], native: true)
mkpak = executable('mkpak', ['Misc/vq_pak/mkpak.c'], native: true)
if (build_machine.system() == 'darwin') or get_option('buildtype').startswith('debug')
    spirv_opt_command = [] # MoltenVK spirv-cross has a bug that breaks optimized shaders
elif has_canonicalize_ids_spirv_opt
	message('Info: spirv-opt has --canonicalize-ids (glslang >= 16.0).')
    spirv_opt_command = [spirv_opt, '-Os', '--canonicalize-ids', '--strip-debug', '@PLAINNAME@.spv', '-o', '@PLAINNAME@.spv', '&&']   
else 
    spirv_opt_command = [spirv_opt, '-Os', '@PLAINNAME@.spv', '-o', '@PLAINNAME@.spv', '&&']
	message('Info: spirv-opt has NOT --canonicalize-ids (glslang < 16.0) so use spirv-remap instead.')
	spirv_remap = find_program('spirv-remap')
	spirv_remap_command = [spirv_remap, '-s', '-o', '.', '-i', '@PLAINNAME@.spv', '&&']
endif

bintoc_command = [bintoc, '@PLAINNAME@.spv', '@PLAINNAME@_spv', '@OUTPUT@']

vq_pak_path = join_paths(meson.project_source_root(), 'Misc', 'vq_pak')
mkpak_toc_file = join_paths(vq_pak_path, 'vq_pak_contents.txt')
pak_files = []
foreach entry : fs.read(mkpak_toc_file).strip().split('\n')
    pak_files += files(join_paths('Misc', 'vq_pak', entry.strip()))
endforeach

mkpak_command = [mkpak, '@OUTPUT@', vq_pak_path, '@INPUT@']

run_mkpak = custom_target('run-mkpak',
  input : mkpak_toc_file,
  depend_files : pak_files,
  output : 'vkquake.pak',
  command: mkpak_command)

# Generate the embedded source from the pak in this build directory.
bintoc_compress_command = [bintoc, '-c', '@INPUT@', 'vkquake.pak', '@OUTPUT@']

run_generate_embedded_pak = custom_target('run-embed_vkquake_pak',
							input : run_mkpak,
							output : 'embedded_pak.cpp',
							command : bintoc_compress_command)
shaders_cpp = []
foreach shader : shaders
    target_env = shader.contains('sops') ? ['--target-env', 'vulkan1.1'] : []
    debug_flag = get_option('buildtype').startswith('debug') ? ['-g'] : []
    glslang_command = [glslang, '-V'] + debug_flag + target_env + ['-o', '@PLAINNAME@.spv', '@INPUT@', '--depfile', '@PLAINNAME@.deps', '&&']
    shaders_cpp += custom_target(shader.underscorify(),
                 input: shader,
                 output: '@PLAINNAME@.cpp',
                 depfile: '@PLAINNAME@.deps',
				 depends : [run_generate_embedded_pak],
                 command: glslang_command
                          + spirv_opt_command
                          + spirv_remap_command
                          + bintoc_command)
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
    'Quake/gl_screen.c',
    'Quake/gl_sky.c',
    'Quake/gl_texmgr.c',
    'Quake/gl_vidsdl.c',
    'Quake/gl_warp.c',
    'Quake/host.c',
    'Quake/host_cmd.c',
    'Quake/image.c',
    'Quake/in_sdl.c',
    'Quake/keys.c',
    'Quake/main_sdl.c',
    'Quake/mathlib.c',
    'Quake/mdfour.c',
    'Quake/mem.c',
    'Quake/menu.c',
    'Quake/net_bsd.c',
    'Quake/net_dgrm.c',
    'Quake/net_loop.c',
    'Quake/net_main.c',
    'Quake/net_udp.c',
    'Quake/palette.c',
    'Quake/pl_linux.c',
    'Quake/pr_cmds.c',
    'Quake/pr_edict.c',
    'Quake/pr_exec.c',
    'Quake/pr_ext.c',
    'Quake/r_alias.c',
    'Quake/r_brush.c',
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
    'Quake/snd_umx.c',
    'Quake/snd_wave.c',
    'Quake/strlcat.c',
    'Quake/strlcpy.c',
    'Quake/sv_main.c',
    'Quake/sv_move.c',
    'Quake/sv_phys.c',
    'Quake/sv_user.c',
	'Quake/sys_sdl.c',
    'Quake/sys_sdl_unix.c',
    'Quake/tasks.c',
    'Quake/view.c',
    'Quake/wad.c',
    'Quake/world.c',
	'Quake/hash_map.c',
    'Quake/kd_model.c',
    'Quake/kd_render.c',
    'Quake/kd_world.c',
    'Quake/kd_compile.c',
    'Quake/kd_svo.c',
    'Quake/noise_gen.c',
    'Quake/jolt_physics.cpp',
    'Quake/jolt_stream_world.cpp',
    'Quake/jolt_stream_character.cpp',
    'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp',
    'Quake/terrain_lod.cpp',
    'Quake/terrain_mesher.cpp',
    'Quake/terrain_textures.cpp',
    'Quake/terrain_density.cpp',
    'Quake/terrain_collision.cpp',
    'Quake/terrain_lod_cache.cpp',
    'Quake/octree_engine.cpp',
    'Quake/octree_checkpoint.cpp',
    'Quake/octree_paging.cpp',
    'Quake/octree_qc.cpp',
    'Quake/octree_render.cpp',
    'Quake/world_protocol.cpp',
    'Quake/world_peer.cpp',
    'Quake/world_peer_gns.cpp',
    'Quake/sky_atmosphere.cpp',
    'Quake/sky_profile.cpp',
]

# The prebuilt Jolt library uses Distribution mode in every engine build.
cflags = ['-Wall', '-Wno-trigraphs', '-Werror', '-DJPH_NO_DEBUG']

deps = [
    cc.find_library('m', required : false),
    cc.find_library('dl', required : false),
    dependency('threads'),
    dependency('sqlite3'),
    dependency('sdl2'),
    cc.find_library('Jolt', dirs : [join_paths(meson.project_source_root(), 'Jolt', 'Build', 'Linux_Distribution')], static : true),
]

# Optional native ICE transport. The bootstrap installs only into this checkout.
gns_prefix = get_option('gns_prefix')
gns_cmake_args = []
if gns_prefix != ''
    if not fs.is_absolute(gns_prefix)
        gns_prefix = join_paths(meson.project_source_root(), gns_prefix)
    endif
    gns_cmake_args = ['-DCMAKE_PREFIX_PATH=' + gns_prefix]
endif
gns_dep = dependency('GameNetworkingSockets', method: 'cmake',
    modules: ['GameNetworkingSockets::GameNetworkingSockets'],
    cmake_args: gns_cmake_args, required: get_option('use_gns'))
gns_enabled = gns_dep.found()
if gns_enabled
    # Upstream exports no package version file. Check the public API we use so
    # auto discovery of an older package selects the fallback instead of failing.
    gns_enabled = cc.links('''
        #include <steam/steamnetworkingsockets.h>
        #include <steam/isteamnetworkingutils.h>
        #include <steam/steamnetworkingcustomsignaling.h>
        int main() {
            SteamNetworkingIdentity identity;
            identity.Clear();
            identity.SetGenericBytes("probe", 5);
            SteamNetworkingErrMsg error;
            if (!GameNetworkingSockets_Init(&identity, error)) return 1;
            auto *utils = SteamNetworkingUtils();
            bool ok = utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation, 1);
            ok &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 64);
            ok &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, 16396);
            GameNetworkingSockets_Kill();
            return ok ? 0 : 1;
        }
    ''', dependencies: gns_dep, name: 'GameNetworkingSockets native ICE API')
    if not gns_enabled and get_option('use_gns').enabled()
        error('GameNetworkingSockets lacks the required native ICE API. Run python3 Misc/build_gns.py to build the pinned dependency.')
    endif
endif
gns_args = []
if gns_enabled
    gns_args = ['-DQUIVER_HAVE_GNS=1']
    deps += gns_dep
    cflags += gns_args
    message('GameNetworkingSockets enabled; native ICE requires ENABLE_ICE=ON in that dependency.')
else
    message('GameNetworkingSockets unavailable/disabled: peer TCP and authoritative server fallback remain available.')
endif

if build_machine.system() == 'darwin'
    molten_dirs = []
    if fs.exists('/opt/homebrew/lib/libMoltenVK.dylib')
        molten_dirs += '/opt/homebrew/lib'
    endif
    deps += cc.find_library('MoltenVK', required : true, dirs : molten_dirs)
else
    deps += dependency('vulkan')
endif

if get_option('use_codec_wave').enabled()
    cflags += '-DUSE_CODEC_WAVE'
endif

if get_option('mp3_lib') == 'mad'
    mp3_dep = dependency('mad', required : get_option('use_codec_mp3'))
    if mp3_dep.found()
        srcs += [ 'Quake/snd_mp3.c', 'Quake/snd_mp3tag.c' ]
        deps += mp3_dep
        cflags += '-DUSE_CODEC_MP3'
    endif
else
    mp3_dep = dependency('libmpg123', required : get_option('use_codec_mp3'))
    if mp3_dep.found()
        srcs += [ 'Quake/snd_mpg123.c', 'Quake/snd_mp3tag.c' ]
        deps += mp3_dep
        cflags += '-DUSE_CODEC_MP3'
    endif
endif

flac_dep = dependency('flac', required : get_option('use_codec_flac'))
if flac_dep.found()
    srcs += 'Quake/snd_flac.c'
    deps += flac_dep
    cflags += '-DUSE_CODEC_FLAC'
endif

ogg_dep = dependency('ogg', required : get_option('use_codec_vorbis'))
if get_option('vorbis_lib') == 'vorbis'
    vorbisfile_dep = dependency('vorbisfile', required : get_option('use_codec_vorbis'))
    vorbis_dep = dependency('vorbis', required : get_option('use_codec_vorbis'))
    if ogg_dep.found() and vorbisfile_dep.found() and vorbis_dep.found()
        srcs += 'Quake/snd_vorbis.c'
        deps += [ ogg_dep, vorbisfile_dep, vorbis_dep ]
        cflags += '-DUSE_CODEC_VORBIS'
    endif
else
    vorbis_dep = dependency('vorbisidec', required : get_option('use_codec_vorbis'))
    if ogg_dep.found() and vorbis_dep.found()
        srcs += 'Quake/snd_vorbis.c'
        deps += [ ogg_dep, vorbis_dep ]
        cflags += [ '-DUSE_CODEC_VORBIS', '-DVORBIS_USE_TREMOR' ]
    endif
endif

opus_dep = dependency('opus', required : get_option('use_codec_opus'))
opusfile_dep = dependency('opusfile', required : get_option('use_codec_opus'))
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
    'Quake',
    'Quake/mimalloc',
    include_directories('Jolt', is_system : true),
]

# Meson selects the compiler by file extension. Keep the engine's .c sources
# in place and copy them to .cpp files for the C++ compiler at build time.
cpp_srcs = []
foreach src : srcs
    if src.endswith('.c')
        cpp_srcs += fs.copyfile(src, fs.stem(src) + '.cpp')
    else
        cpp_srcs += src
    endif
endforeach

executable('vkquake', [cpp_srcs, shaders_cpp, run_generate_embedded_pak], dependencies : deps, include_directories : incdirs, cpp_args : cflags, override_options : ['cpp_std=gnu++26'])

world_core_test = executable('octree_world_test', ['tests/octree_world_test.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')],
    override_options : ['b_ndebug=false'])
test('octree world', world_core_test, timeout : 120)
terrain_lod_test = executable('terrain_lod_test', ['tests/terrain_lod_test.cpp', 'Quake/terrain_lod.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')],
    override_options : ['b_ndebug=false'])
test('terrain LOD', terrain_lod_test, timeout : 120)
terrain_lod_cache_test = executable('terrain_lod_cache_test', ['tests/terrain_lod_cache_test.cpp', 'Quake/terrain_lod_cache.cpp', 'Quake/terrain_lod.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')],
    override_options : ['b_ndebug=false'])
test('terrain LOD cache', terrain_lod_cache_test, timeout : 120)
terrain_mesher_test = executable('terrain_mesher_test', ['tests/terrain_mesher_test.cpp', 'Quake/terrain_mesher.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')],
    override_options : ['b_ndebug=false'])
test('terrain marching tetrahedra', terrain_mesher_test, timeout : 120)
terrain_collision_test = executable('terrain_collision_test', ['tests/terrain_collision_test.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_mesher.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')],
    override_options : ['b_ndebug=false'])
test('terrain collision primitives', terrain_collision_test, timeout : 120)
terrain_world_collision_test = executable('terrain_world_collision_test', ['tests/terrain_world_collision_test.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_mesher.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')],
    override_options : ['b_ndebug=false'])
test('terrain collision world', terrain_world_collision_test, timeout : 120)
world_protocol_test = executable('world_protocol_test', ['tests/world_protocol_test.cpp', 'Quake/world_protocol.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')], override_options : ['b_ndebug=false'])
test('world protocol', world_protocol_test, timeout : 120)

world_peer_deps = [dependency('threads')]
if gns_enabled
    world_peer_deps += gns_dep
endif
if host_machine.system() == 'windows'
    world_peer_deps += cc.find_library('ws2_32')
endif
world_peer_test = executable('world_peer_test', ['tests/world_peer_test.cpp', 'Quake/world_peer.cpp', 'Quake/world_peer_gns.cpp'],
    include_directories : ['Quake'], dependencies : world_peer_deps, cpp_args: gns_args, override_options : ['b_ndebug=false'])
test('world peer transport', world_peer_test, timeout : 30)
# Invoked by run_world_nat_smoke.py inside isolated Linux network namespaces.
if host_machine.system() == 'linux'
    executable('world_peer_nat_fixture', ['tests/world_peer_nat_fixture.cpp', 'Quake/world_peer.cpp', 'Quake/world_peer_gns.cpp'],
        include_directories: ['Quake'], dependencies: world_peer_deps, cpp_args: gns_args,
        override_options: ['b_ndebug=false'])
endif


world_handles_test = executable('world_position_handles_test', 'tests/world_position_handles_test.cpp',
    include_directories : ['Quake'], override_options : ['b_ndebug=false'])
test('world position handles', world_handles_test)
world_strings_test = executable('world_string_pool_test', 'tests/world_string_pool_test.cpp',
    include_directories : ['Quake'], override_options : ['b_ndebug=false'])
test('world restored strings', world_strings_test)

terrain_textures_test = executable('terrain_textures_test', ['tests/terrain_textures_test.cpp', 'Quake/terrain_textures.cpp'],
    include_directories : ['Quake'], override_options : ['b_ndebug=false'])
test('terrain material textures', terrain_textures_test)

cloud_noise_test = executable('cloud_noise_test', 'tests/noise_gen_test.cpp',
    include_directories : ['Quake'], dependencies : [dependency('sdl2'), dependency('threads')],
    override_options : ['b_ndebug=false'])
test('cloud noise periodicity', cloud_noise_test, timeout : 120)

planet_generation_test = executable('planet_generation_test', ['tests/planet_generation_test.cpp', 'Quake/planet_generation.cpp'], include_directories : ['Quake'], override_options : ['b_ndebug=false'], dependencies : dependency('threads'))
test('planet generation', planet_generation_test, timeout : 120)
planet_curvature_test = executable('planet_curvature_test', 'tests/planet_curvature_test.cpp', include_directories : ['Quake'], override_options : ['b_ndebug=false'])
test('planet curvature', planet_curvature_test)

planet_world_test = executable('planet_world_test', ['tests/planet_world_test.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'], include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')], override_options : ['b_ndebug=false'])
test('planet world', planet_world_test, timeout : 180)
world_collision_async_test = executable('world_collision_async_test', ['tests/world_collision_async_test.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'], include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')], override_options : ['b_ndebug=false'])
test('asynchronous world collision', world_collision_async_test, timeout : 120)
world_solids_test = executable('world_solids_test', ['tests/world_solids_test.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp'], include_directories : ['Quake'], override_options : ['b_ndebug=false'])
test('world solids', world_solids_test, timeout : 120)

world_solids_persistence_test = executable('world_solids_persistence_test', ['tests/world_solids_persistence_test.cpp', 'Quake/world_protocol.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'],
    include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')], override_options : ['b_ndebug=false'])
test('world solids persistence', world_solids_persistence_test, timeout : 180)

planet_surface_test = executable('planet_surface_test', ['tests/planet_surface_test.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'], include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')], override_options : ['b_ndebug=false'])
test('planet surface', planet_surface_test, timeout : 120)

feature_collision_test = executable('feature_collision_test', ['tests/feature_collision_test.cpp', 'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp', 'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'], include_directories : ['Quake'], dependencies : [dependency('sqlite3'), dependency('threads')], override_options : ['b_ndebug=false'])
test('feature collision', feature_collision_test, timeout : 120)

jolt_stream_test = executable('jolt_stream_test', [
    'tests/jolt_stream_test.cpp', 'Quake/jolt_stream_world.cpp', 'Quake/jolt_stream_character.cpp',
    'Quake/octree_world.cpp', 'Quake/planet_generation.cpp', 'Quake/world_solids.cpp', 'Quake/terrain_features.cpp',
    'Quake/planet_surface.cpp', 'Quake/terrain_density.cpp', 'Quake/terrain_collision.cpp', 'Quake/terrain_mesher.cpp'],
    include_directories : [include_directories('Quake'), include_directories('Jolt', is_system : true)],
    cpp_args : ['-DJPH_NO_DEBUG'],
    dependencies : [dependency('sqlite3'), dependency('threads'), cc.find_library('Jolt', dirs : [join_paths(meson.project_source_root(), 'Jolt', 'Build', 'Linux_Distribution')], static : true)],
    override_options : ['b_ndebug=false'])
test('Jolt streamed world and controller', jolt_stream_test, timeout : 180)
