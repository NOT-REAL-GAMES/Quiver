HOST_OS =$(shell uname|sed -e s/_.*//|tr '[:upper:]' '[:lower:]')

CC = gcc
# HOST_CC is for bintoc which is run on the host OS, not
#         the target OS: cross-compile friendliness.
HOST_CC = gcc
LINKER = $(CC)
STRIP ?= strip
WINDRES ?= windres
GLSLANG = glslangValidator
DEBUG ?= 0
USE_SDL3 ?= 1
EMBED_MAPTOOLS ?= 1

# The map compilers are isolated into C++ namespaces and linked into the game.
# Derive a matching C++ driver for cross compilers unless explicitly supplied.
ifeq ($(origin CXX),default)
CXX := $(patsubst %gcc,%g++,$(CC))
ifeq ($(CXX),$(CC))
CXX := $(patsubst %clang,%clang++,$(CC))
endif
endif
ifeq ($(EMBED_MAPTOOLS),1)
LINKER = $(CXX)
CFLAGS += -DHAVE_EMBEDDED_MAPTOOLS
MAPTOOLS_ROOT := ../ThirdParty/ericw-qbsp
MAPTOOLS_WRAPPERS := $(wildcard $(MAPTOOLS_ROOT)/embedded/wrappers/*.cc)
MAPTOOLS_OBJS := $(patsubst $(MAPTOOLS_ROOT)/embedded/wrappers/%.cc,.editor-tools/%.o,$(MAPTOOLS_WRAPPERS)) .editor-tools/bridge.o
MAPTOOLS_FLAGS := -std=c++11 -O2 -I$(MAPTOOLS_ROOT)/include -DQUIVER_EMBEDDED_MAPTOOLS -DERICWTOOLS_VERSION=v0.18.1
ifneq ($(findstring sys_sdl_win.o,$(SYSOBJ_SYS)),)
MAPTOOLS_FLAGS += -DUSE_WIN32THREADS -DWIN32 -D_CRT_SECURE_NO_WARNINGS -D__USE_MINGW_ANSI_STDIO=1
else
MAPTOOLS_FLAGS += -DUSE_PTHREADS -DLINUX -pthread
endif
endif

ifeq ($(USE_SDL3),1)
CFLAGS += -DUSE_SDL3
endif

CHECK_GCC = $(shell if echo | $(CC) $(1) -Werror -S -o /dev/null -xc - > /dev/null 2>&1; then echo "$(1)"; else echo "$(2)"; fi;)

CFLAGS += -MMD -Wall -Wno-trigraphs -Werror -std=gnu11 -fno-common
ifneq ($(HOST_OS),darwin)
CFLAGS += -D_FILE_OFFSET_BITS=64
endif
CFLAGS += $(CPUFLAGS)
ifneq ($(DEBUG),0)
DFLAGS += -D_DEBUG
CFLAGS += -g
CFLAGS += -fno-omit-frame-pointer
DO_STRIP=
else
DFLAGS += -DNDEBUG
CFLAGS += -O3
CFLAGS += -g
CFLAGS += -fno-omit-frame-pointer
#CFLAGS += $(call CHECK_GCC,-flto=auto,)
CFLAGS += $(call CHECK_GCC,-fweb,)
CFLAGS += $(call CHECK_GCC,-frename-registers,)
#CFLAGS += $(call CHECK_GCC,-fno-asynchronous-unwind-tables,)
CFLAGS += $(call CHECK_GCC,-fno-ident,)
CMD_STRIP=$(STRIP) $(1)
define DO_STRIP
	$(call CMD_STRIP,$(1));
endef
endif

ifeq ($(DEBUG),0)
GLSLANG_FLAGS = -V
GLSLANG_OUT_FOLDER = Release
else
GLSLANG_FLAGS = -g -V
GLSLANG_OUT_FOLDER = Debug
endif

# ---------------------------
# libraries
# ---------------------------
ifneq ($(VORBISLIB),vorbis)
ifneq ($(VORBISLIB),tremor)
$(error Invalid VORBISLIB setting)
endif
endif
ifneq ($(MP3LIB),mpg123)
ifneq ($(MP3LIB),mad)
$(error Invalid MP3LIB setting)
endif
endif
ifeq ($(MP3LIB),mad)
mp3_obj = snd_mp3
lib_mp3dec = -lmad
endif
ifeq ($(MP3LIB),mpg123)
mp3_obj = snd_mpg123
lib_mp3dec = -lmpg123
endif
ifeq ($(VORBISLIB),vorbis)
CPP_VORBISDEC =
LIB_VORBISDEC = -lvorbisfile -lvorbis -logg
endif
ifeq ($(VORBISLIB),tremor)
CPP_VORBISDEC = -DVORBIS_USE_TREMOR
LIB_VORBISDEC = -lvorbisidec -logg
endif

CODECLIBS  :=
ifeq ($(USE_CODEC_WAVE),1)
CFLAGS += -DUSE_CODEC_WAVE
endif
ifeq ($(USE_CODEC_FLAC),1)
CFLAGS += -DUSE_CODEC_FLAC
CODECLIBS += -lFLAC
endif
ifeq ($(USE_CODEC_OPUS),1)
CFLAGS += -DUSE_CODEC_OPUS
endif
ifeq ($(USE_CODEC_VORBIS),1)
CFLAGS += -DUSE_CODEC_VORBIS $(CPP_VORBISDEC)
CODECLIBS += $(LIB_VORBISDEC)
endif
ifeq ($(USE_CODEC_MP3),1)
CFLAGS += -DUSE_CODEC_MP3
CODECLIBS += $(lib_mp3dec)
endif
ifeq ($(USE_CODEC_MIKMOD),1)
CFLAGS += -DUSE_CODEC_MIKMOD
CODECLIBS += -lmikmod
endif
ifeq ($(USE_CODEC_XMP),1)
CFLAGS += -DUSE_CODEC_XMP
CODECLIBS += -lxmp
endif
ifeq ($(USE_CODEC_MODPLUG),1)
CFLAGS += -DUSE_CODEC_MODPLUG
CODECLIBS += -lmodplug
endif
ifeq ($(USE_CODEC_UMX),1)
CFLAGS += -DUSE_CODEC_UMX
endif
CFLAGS += -Imimalloc

# ---------------------------
# objects
# ---------------------------

MUSIC_OBJS:= bgmusic.o \
	snd_codec.o \
	snd_flac.o \
	snd_wave.o \
	snd_vorbis.o \
	snd_opus.o \
	$(mp3_obj).o \
	snd_mp3tag.o \
	snd_mikmod.o \
	snd_modplug.o \
	snd_xmp.o \
	snd_umx.o
COMOBJ_SND := snd_dma.o snd_mix.o snd_mem.o $(MUSIC_OBJS)
SYSOBJ_SND := snd_sdl.o snd_sdl3.o
SYSOBJ_CDA := cd_sdl.o
SYSOBJ_INPUT := in_sdl.o in_sdl2.o in_sdl3.o
ifeq ($(HOST_OS),darwin)
ifeq ($(USE_SDL3),1)
SYSOBJ_INPUT += in_mac.o
endif
endif
SYSOBJ_GL_VID:= gl_vidsdl.o
SYSOBJ_MAIN:= main_sdl.o

SHADER_OBJS = \
	alias_frag.o \
	alias_oit_frag.o \
	alias_alphatest_frag.o \
	alias_alphatest_oit_frag.o \
	alias_mboit_moment_frag.o \
	alias_alphatest_mboit_moment_frag.o \
	alias_mboit_composite_frag.o \
	alias_alphatest_mboit_composite_frag.o \
	alias_mboit_composite_msaa_frag.o \
	alias_alphatest_mboit_composite_msaa_frag.o \
	md5_mboit_composite_frag.o \
	md5_alphatest_mboit_composite_frag.o \
	md5_mboit_composite_msaa_frag.o \
	md5_alphatest_mboit_composite_msaa_frag.o \
	alias_vert.o \
	alias_instanced_vert.o \
	alias_ecs_cull_comp.o \
	radiance_trace_sw_comp.o \
	radiance_trace_rt_comp.o \
	radiance_merge_comp.o \
	radiance_receiver_world_frag.o \
	radiance_receiver_model_frag.o \
	radiance_receiver_model_alphatest_frag.o \
	alias_receiver_forward_fast_sw_frag.o \
	alias_receiver_forward_fast_sw_alphatest_frag.o \
	alias_receiver_forward_fast_rt_frag.o \
	alias_receiver_forward_fast_rt_alphatest_frag.o \
	radiance_receiver_half_comp.o \
	radiance_receiver_half_fast_comp.o \
	radiance_receiver_half_msaa_comp.o \
	radiance_receiver_half_fast_msaa_comp.o \
	radiance_receiver_depth_resolve_frag.o \
	radiance_receiver_reconstruct_comp.o \
	radiance_receiver_filter_comp.o \
	radiance_receiver_diagnostics_comp.o \
	radiance_receiver_direct_fast_sw_comp.o \
	radiance_receiver_direct_fast_rt_comp.o \
	radiance_receiver_light_comp.o \
	radiance_receiver_light_miss_sw_comp.o \
	radiance_receiver_light_miss_rt_comp.o \
	world_receiver_forward_lit_frag.o \
	alias_receiver_forward_lit_frag.o \
	alias_receiver_forward_lit_alphatest_frag.o \
	radiance_receiver_soft_sw_comp.o \
	radiance_receiver_soft_rt_comp.o \
	world_receiver_forward_fast_sw_frag.o \
	world_receiver_forward_fast_rt_frag.o \
	radiance_receiver_direct_sw_comp.o \
	radiance_receiver_direct_rt_comp.o \
	world_receiver_forward_sw_frag.o \
	world_receiver_forward_rt_frag.o \
	md5_vert.o \
	md5_8_vert.o \
	basic_alphatest_frag.o \
	screen_effects_8bit_comp.o \
	screen_effects_10bit_comp.o \
	screen_effects_hdr_comp.o \
	ray_debug_hdr_comp.o \
	cs_tex_warp_comp.o \
	indirect_comp.o \
	indirect_clear_comp.o \
	meshlet_cull_comp.o \
	meshlet_hiz_comp.o \
	meshlet_hiz_raster_comp.o \
	meshlet_hiz_reduce_comp.o \
	basic_frag.o \
	fte_particles_frag.o \
	fte_particles_msaa_frag.o \
	basic_oit_frag.o \
	basic_mboit_moment_frag.o \
	basic_mboit_composite_frag.o \
	basic_mboit_composite_msaa_frag.o \
	basic_notex_frag.o \
	draw_pic_frag.o \
	draw_pic_alphatest_frag.o \
	draw_pic_xbr_frag.o \
	draw_pic_xbr_alphatest_frag.o \
	draw_pic_xbr_vert.o \
	basic_vert.o \
	sky_layer_frag.o \
	sky_layer_vert.o \
	sky_box_frag.o \
	sky_cube_frag.o \
	sky_cube_vert.o \
	postprocess_frag.o \
	scene_upscale_frag.o \
	ssao_composite_frag.o \
	ssao_composite_debug_frag.o \
	ssao_composite_msaa_debug_frag.o \
	ssao_prepare_comp.o \
	ssao_prepare_msaa_comp.o \
	ssao_evaluate_comp.o \
	ssao_evaluate_fp16_comp.o \
	ssao_mip_comp.o \
	ssao_mip_fp16_comp.o \
	ssao_mip_msaa_comp.o \
	ssao_mip_msaa_fp16_comp.o \
	ssao_filter_comp.o \
	ssao_filter_fp16_comp.o \
	ssao_composite_msaa_frag.o \
	postprocess_vert.o \
	wboit_resolve_frag.o \
	wboit_resolve_msaa_frag.o \
	mboit_resolve_frag.o \
	mboit_resolve_msaa_frag.o \
	world_frag.o \
	world_oit_frag.o \
	world_mboit_moment_frag.o \
	world_mboit_composite_frag.o \
	world_mboit_composite_msaa_frag.o \
	world_vert.o \
	showtris_frag.o \
	showtris_vert.o \
	md5_debug_vert.o \
	update_lightmap_8bit_comp.o \
	update_lightmap_8bit_rt_comp.o \
	update_lightmap_10bit_comp.o \
	update_lightmap_10bit_rt_comp.o \
	ray_debug_comp.o \
	mesh_interpolate_comp.o \
	skinning_comp.o \
	skinning_8_comp.o

SHADER_OBJS += \
	world_radiance_vert.o \
	alias_radiance_vert.o \
	alias_instanced_radiance_vert.o \
	md5_radiance_vert.o \
	md5_8_radiance_vert.o \
	world_radiance_sw_frag.o \
	world_radiance_sw_oit_frag.o \
	world_radiance_sw_mboit_moment_frag.o \
	world_radiance_sw_mboit_composite_frag.o \
	world_radiance_sw_mboit_composite_msaa_frag.o \
	alias_radiance_sw_frag.o \
	alias_radiance_sw_alphatest_frag.o \
	alias_radiance_sw_oit_frag.o \
	alias_radiance_sw_alphatest_oit_frag.o \
	alias_radiance_sw_mboit_moment_frag.o \
	alias_radiance_sw_alphatest_mboit_moment_frag.o \
	alias_radiance_sw_mboit_composite_frag.o \
	alias_radiance_sw_alphatest_mboit_composite_frag.o \
	alias_radiance_sw_mboit_composite_msaa_frag.o \
	alias_radiance_sw_alphatest_mboit_composite_msaa_frag.o \
	md5_radiance_sw_mboit_composite_frag.o \
	md5_radiance_sw_alphatest_mboit_composite_frag.o \
	md5_radiance_sw_mboit_composite_msaa_frag.o \
	md5_radiance_sw_alphatest_mboit_composite_msaa_frag.o \
	world_radiance_rt_frag.o \
	world_radiance_rt_oit_frag.o \
	world_radiance_rt_mboit_moment_frag.o \
	world_radiance_rt_mboit_composite_frag.o \
	world_radiance_rt_mboit_composite_msaa_frag.o \
	alias_radiance_rt_frag.o \
	alias_radiance_rt_alphatest_frag.o \
	alias_radiance_rt_oit_frag.o \
	alias_radiance_rt_alphatest_oit_frag.o \
	alias_radiance_rt_mboit_moment_frag.o \
	alias_radiance_rt_alphatest_mboit_moment_frag.o \
	alias_radiance_rt_mboit_composite_frag.o \
	alias_radiance_rt_alphatest_mboit_composite_frag.o \
	alias_radiance_rt_mboit_composite_msaa_frag.o \
	alias_radiance_rt_alphatest_mboit_composite_msaa_frag.o \
	md5_radiance_rt_mboit_composite_frag.o \
	md5_radiance_rt_alphatest_mboit_composite_frag.o \
	md5_radiance_rt_mboit_composite_msaa_frag.o \
	md5_radiance_rt_alphatest_mboit_composite_msaa_frag.o

# Older shader compilers can still build the indexed renderer.
MESH_SHADER_SUPPORT := $(shell printf '\043version 460\n\043extension GL_EXT_mesh_shader : require\nlayout(local_size_x=1) in;\nlayout(triangles, max_vertices=3, max_primitives=1) out;\nvoid main() { SetMeshOutputsEXT(0,0); }\n' | $(GLSLANG) --stdin -S mesh -V --target-env vulkan1.1 --target-env spirv1.4 -o /dev/null >/dev/null 2>&1 && echo 1)
ifeq ($(MESH_SHADER_SUPPORT),1)
DFLAGS += -DHAVE_WORLD_MESH_SHADER=1
SHADER_OBJS += world_mesh_mesh.o
SHADER_OBJS += world_mesh_radiance_mesh.o
TASK_SHADER_SUPPORT := $(shell printf '\043version 460\n\043extension GL_EXT_mesh_shader : require\nlayout(local_size_x=1) in;\nvoid main() { EmitMeshTasksEXT(0,1,1); }\n' | $(GLSLANG) --stdin -S task -V --target-env vulkan1.1 --target-env spirv1.4 -o /dev/null >/dev/null 2>&1 && echo 1)
ifeq ($(TASK_SHADER_SUPPORT),1)
DFLAGS += -DHAVE_WORLD_TASK_SHADER=1
SHADER_OBJS += world_task_task.o world_task_mesh_mesh.o
SHADER_OBJS += world_task_mesh_radiance_mesh.o
endif
endif

GLOBJS = \
	palette.o \
	gl_refrag.o \
	gl_rlight.o \
	gl_rmain.o \
	gl_fog.o \
	gl_rmisc.o \
	r_passes.o \
	r_ecs_gpu.o \
	radiance_cascades.o \
	radiance_emissive.o \
	radiance_sparse.o \
	radiance_lights.o \
	radiance_gpu_inputs.o \
	radiance_material_atlas.o \
	radiance_materials_quake.o \
	radiance_coordinator.o \
	radiance_matrix.o \
	radiance_receiver_vulkan.o \
	radiance_receiver_targets.o \
	radiance_receiver_pipelines.o \
	radiance_receiver_record.o \
	radiance_receiver_diagnostics.o \
	radiance_receiver_world.o \
	radiance_receiver_model.o \
	radiance_receiver_frame.o \
	radiance_receiver_snapshot.o \
	radiance_receiver_forward.o \
	radiance_receiver_capture.o \
	radiance_receiver_select.o \
	radiance_receiver_descriptors.o \
	radiance_receiver_layouts.o \
	radiance_receiver_resources.o \
	radiance_receiver_stage.o \
	radiance_receiver_alternates.o \
	radiance_receiver_shaders.o \
	radiance_receiver_slot.o \
	radiance_receiver_live.o \
	radiance_scene.o \
	radiance_alias.o \
	radiance_scene_alias.o \
	radiance_scene_bsp.o \
	radiance_scene_runtime.o \
	radiance_scene_vulkan.o \
	radiance_scene_rt.o \
	radiance_scene_collect.o \
	radiance_vulkan.o \
	radiance_engine.o \
	radiance_runtime.o \
	r_ssao.o \
	r_part.o \
	r_part_fte.o \
	r_world.o \
	gl_screen.o \
	gl_sky.o \
	gl_warp.o \
	$(SYSOBJ_GL_VID) \
	gl_draw.o \
	image.o \
	gl_texmgr.o \
	gl_mesh.o \
	gl_heap.o \
	r_sprite.o \
	r_alias.o \
	r_brush.o \
	bsp_meshlet.o \
	editor.o \
	editor_map.o \
	editor_sector.o \
	editor_sector_portal.o \
	editor_process.o \
	editor_tools.o \
	editor_ui.o \
	editor_model_preview.o \
	ui_control.o \
	ecs.o \
	editor_controls.o \
	static_models.o \
	static_models_collision.o \
	static_models_engine.o \
	meshlet_hiz.o \
	gl_model.o

OBJS := strlcat.o \
	strlcpy.o \
	$(GLOBJS) \
	$(SYSOBJ_INPUT) \
	$(COMOBJ_SND) \
	$(SYSOBJ_SND) \
	$(SYSOBJ_CDA) \
	$(SYSOBJ_NET) \
	net_dgrm.o \
	net_loop.o \
	net_main.o \
	chase.o \
	cl_demo.o \
	cl_input.o \
	cl_main.o \
	cl_parse.o \
	cl_tent.o \
	console.o \
	keys.o \
	menu.o \
	sbar.o \
	view.o \
	view_motion.o \
	wad.o \
	cmd.o \
	common.o \
	crc.o \
	cvar.o \
	cfgfile.o \
	json.o \
	steam.o \
	host.o \
	host_cmd.o \
	mathlib.o \
	mdfour.o \
	pr_cmds.o \
	pr_ext.o \
	pr_edict.o \
	pr_exec.o \
	sv_main.o \
	sv_move.o \
	sv_phys.o \
	sv_user.o \
	world.o \
	mem.o \
	tasks.o \
	hash_map.o \
	embedded_pak.o \
	$(SYSOBJ_SYS) $(SYSOBJ_MAIN)

ifeq ($(EMBED_MAPTOOLS),1)
OBJS += $(MAPTOOLS_OBJS)

.editor-tools/%.o: $(MAPTOOLS_ROOT)/embedded/wrappers/%.cc
	@mkdir -p .editor-tools
	$(CXX) $(MAPTOOLS_FLAGS) $(DFLAGS) -MMD -MP -c $< -o $@

.editor-tools/bridge.o: $(MAPTOOLS_ROOT)/embedded/bridge.cc
	@mkdir -p .editor-tools
	$(CXX) $(MAPTOOLS_FLAGS) $(DFLAGS) -MMD -MP -c $< -o $@

sinclude $(MAPTOOLS_OBJS:.o=.d)
endif

$(BINTOC_EXE): ../Shaders/bintoc.c
	make -B -C ../Misc/vq_pak

.SECONDARY:
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_task.spv: ../Shaders/%.task
	$(GLSLANG) $(GLSLANG_FLAGS) --target-env vulkan1.1 --target-env spirv1.4 $< -o $@ --depfile ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_task.d
%_task.o: ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_task.spv $(BINTOC_EXE)
	$(BINTOC_EXE) $< $*_task_spv ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_task.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -o $@ ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_task.c

.SECONDARY:
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_mesh.spv: ../Shaders/%.mesh
	$(GLSLANG) $(GLSLANG_FLAGS) --target-env vulkan1.1 --target-env spirv1.4 $< -o $@ --depfile ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_mesh.d
%_mesh.o: ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_mesh.spv $(BINTOC_EXE)
	$(BINTOC_EXE) $< $*_mesh_spv ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_mesh.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -o $@ ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_mesh.c

.SECONDARY:
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_frag.spv: ../Shaders/%.frag
	$(GLSLANG) $(GLSLANG_FLAGS) $< -o $@ --depfile ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_frag.d
%_frag.o: ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_frag.spv $(BINTOC_EXE)
	$(BINTOC_EXE) $< $*_frag_spv ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_frag.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -o $@ ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_frag.c

.SECONDARY:
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_vert.spv: ../Shaders/%.vert
	$(GLSLANG) $(GLSLANG_FLAGS) $< -o $@ --depfile ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_vert.d
%_vert.o: ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_vert.spv $(BINTOC_EXE)
	$(BINTOC_EXE) $< $*_vert_spv ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_vert.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -o $@ ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_vert.c

.SECONDARY:
%_sops_comp.o: ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_sops_comp.spv $(BINTOC_EXE)
	$(BINTOC_EXE) $< $*_sops_comp_spv ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_sops_comp.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -o $@ ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_sops_comp.c

.SECONDARY:
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_comp.spv: ../Shaders/%.comp
	$(GLSLANG) $(GLSLANG_FLAGS) $< -o $@ --depfile ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_comp.d

# The filter's shared-memory helper must also invalidate existing SPIR-V.
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/radiance_receiver_filter_comp.spv: ../Shaders/radiance_receiver_filter_axis.glsl

RECEIVER_SCALE_SPV_NAMES := radiance_receiver_half_comp radiance_receiver_half_fast_comp \
 radiance_receiver_half_msaa_comp radiance_receiver_half_fast_msaa_comp \
 radiance_receiver_reconstruct_comp radiance_receiver_filter_comp radiance_receiver_diagnostics_comp \
 world_receiver_forward_sw_frag world_receiver_forward_rt_frag \
 world_receiver_forward_fast_sw_frag world_receiver_forward_fast_rt_frag \
 alias_receiver_forward_fast_sw_frag alias_receiver_forward_fast_rt_frag \
 alias_receiver_forward_fast_sw_alphatest_frag alias_receiver_forward_fast_rt_alphatest_frag
$(addprefix ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/,$(addsuffix .spv,$(RECEIVER_SCALE_SPV_NAMES))): ../Shaders/radiance_receiver_scale.glsl

RECEIVER_SHADOW_SPV_NAMES := $(patsubst %.o,%.spv,$(filter world_%_frag.o alias_%_frag.o md5_%_frag.o radiance_receiver_direct_%.o radiance_receiver_soft_%.o,$(SHADER_OBJS)))
$(addprefix ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/,$(RECEIVER_SHADOW_SPV_NAMES)): ../Shaders/radiance_receiver_shadows.glsl ../Shaders/radiance_receiver_shadow_metadata.glsl ../Shaders/radiance_receiver_shadow_source.glsl ../Shaders/radiance_light_unshadowed.glsl ../Shaders/radiance_parameters.glsl
$(addprefix ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/,$(RECEIVER_SHADOW_SPV_NAMES) radiance_trace_sw_comp.spv radiance_trace_rt_comp.spv): ../Shaders/radiance_light_query_free.glsl

../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/ssao_mip_comp.spv \
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/ssao_mip_msaa_comp.spv \
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/ssao_evaluate_fp16_comp.spv \
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/ssao_filter_fp16_comp.spv \
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/ssao_mip_fp16_comp.spv \
../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/ssao_mip_msaa_fp16_comp.spv: GLSLANG_FLAGS += --target-env vulkan1.1

%_comp.o: ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%_comp.spv $(BINTOC_EXE)
	$(BINTOC_EXE) $< $*_comp_spv ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_comp.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -o $@ ../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/$*_comp.c

%.o: %.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -o $@ $<

ifeq ($(USE_SDL3),1)
%.o: %.m
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -fblocks -o $@ $<
endif

# mem.c includes mimalloc which has warnings that don't apply to MinGW/GCC
mem.o: mem.c
	$(CC) $(DFLAGS) -c $(CFLAGS) $(SDL_CFLAGS) -Wno-unused-function -Wno-attributes -o $@ $<

#embedded_pak.c is generated by bintoc
embedded_pak.c: $(BINTOC_EXE)
 	
embedded_pak.o:  embedded_pak.c
	$(CC) $(DFLAGS) -c $(CFLAGS) -o $@ $<

sinclude $(OBJS:.o=.d)
# shader sources compiled multiple times with different preprocessor defines
define SHADER_VARIANT
../Shaders/Compiled/$$(GLSLANG_OUT_FOLDER)/$(1).spv: ../Shaders/$(2)
	$$(GLSLANG) $$(GLSLANG_FLAGS) $(3) $$< -o $$@ --depfile ../Shaders/Compiled/$$(GLSLANG_OUT_FOLDER)/$(1).d
endef

$(eval $(call SHADER_VARIANT,basic_oit_frag,basic.frag,-DWBOIT=1))
$(eval $(call SHADER_VARIANT,basic_mboit_moment_frag,basic.frag,-DMBOIT=1))
$(eval $(call SHADER_VARIANT,basic_mboit_composite_frag,basic.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,basic_mboit_composite_msaa_frag,basic.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,world_oit_frag,world.frag,-DWBOIT=1))
$(eval $(call SHADER_VARIANT,world_mboit_moment_frag,world.frag,-DMBOIT=1))
$(eval $(call SHADER_VARIANT,world_mboit_composite_frag,world.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,world_mboit_composite_msaa_frag,world.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,alias_alphatest_frag,alias.frag,-DALIAS_ALPHA_TEST=1))
$(eval $(call SHADER_VARIANT,alias_oit_frag,alias.frag,-DWBOIT=1))
$(eval $(call SHADER_VARIANT,alias_alphatest_oit_frag,alias.frag,-DALIAS_ALPHA_TEST=1 -DWBOIT=1))
$(eval $(call SHADER_VARIANT,alias_mboit_moment_frag,alias.frag,-DMBOIT=1))
$(eval $(call SHADER_VARIANT,alias_alphatest_mboit_moment_frag,alias.frag,-DALIAS_ALPHA_TEST=1 -DMBOIT=1))
$(eval $(call SHADER_VARIANT,alias_mboit_composite_frag,alias.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,alias_alphatest_mboit_composite_frag,alias.frag,-DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,alias_mboit_composite_msaa_frag,alias.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,alias_alphatest_mboit_composite_msaa_frag,alias.frag,-DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,md5_mboit_composite_frag,alias.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_alphatest_mboit_composite_frag,alias.frag,-DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_mboit_composite_msaa_frag,alias.frag,-DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_alphatest_mboit_composite_msaa_frag,alias.frag,-DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_8_vert,md5.vert,-DEIGHT_WEIGHT_SKINNING))
$(eval $(call SHADER_VARIANT,wboit_resolve_msaa_frag,wboit_resolve.frag,-DMSAA=1))
$(eval $(call SHADER_VARIANT,mboit_resolve_msaa_frag,mboit_resolve.frag,-DMSAA=1))
$(eval $(call SHADER_VARIANT,screen_effects_8bit_comp,screen_effects.comp,))
$(eval $(call SHADER_VARIANT,screen_effects_10bit_comp,screen_effects.comp,-DUSE_10BIT=1))
$(eval $(call SHADER_VARIANT,skinning_8_comp,skinning.comp,-DEIGHT_WEIGHT_SKINNING))
$(eval $(call SHADER_VARIANT,update_lightmap_8bit_comp,update_lightmap.comp,))
$(eval $(call SHADER_VARIANT,update_lightmap_8bit_rt_comp,update_lightmap.comp,-DRAY_QUERIES=1))
$(eval $(call SHADER_VARIANT,update_lightmap_10bit_comp,update_lightmap.comp,-DUSE_10BIT=1))
$(eval $(call SHADER_VARIANT,update_lightmap_10bit_rt_comp,update_lightmap.comp,-DUSE_10BIT=1 -DRAY_QUERIES=1))
$(eval $(call SHADER_VARIANT,world_receiver_forward_sw_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1))
$(eval $(call SHADER_VARIANT,world_receiver_forward_rt_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_direct_sw_comp,radiance_receiver_direct.comp,))
$(eval $(call SHADER_VARIANT,radiance_receiver_direct_rt_comp,radiance_receiver_direct.comp,-DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_model_frag,radiance_receiver_model.frag,))
$(eval $(call SHADER_VARIANT,radiance_receiver_model_alphatest_frag,radiance_receiver_model.frag,-DALIAS_ALPHA_TEST=1))
$(eval $(call SHADER_VARIANT,alias_receiver_forward_fast_sw_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_MODEL_CULL=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_GAUSSIAN_FALLBACK=1))
$(eval $(call SHADER_VARIANT,alias_receiver_forward_fast_sw_alphatest_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_MODEL_CULL=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_GAUSSIAN_FALLBACK=1 -DALIAS_ALPHA_TEST=1))
$(eval $(call SHADER_VARIANT,alias_receiver_forward_fast_rt_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_MODEL_CULL=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_GAUSSIAN_FALLBACK=1 -DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,alias_receiver_forward_fast_rt_alphatest_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_MODEL_CULL=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_GAUSSIAN_FALLBACK=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_half_fast_comp,radiance_receiver_half.comp,-DRC_RECEIVER_TEXTURE_FILTER=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_half_msaa_comp,radiance_receiver_half.comp,-DRECEIVER_MSAA=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_half_fast_msaa_comp,radiance_receiver_half.comp,-DRECEIVER_MSAA=1 -DRC_RECEIVER_TEXTURE_FILTER=1))
$(eval $(call SHADER_VARIANT,world_receiver_forward_fast_sw_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_GAUSSIAN_FALLBACK=1))
$(eval $(call SHADER_VARIANT,world_receiver_forward_fast_rt_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_GAUSSIAN_FALLBACK=1 -DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_direct_fast_sw_comp,radiance_receiver_direct.comp,-DRC_RECEIVER_TEXTURE_FILTER=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_direct_fast_rt_comp,radiance_receiver_direct.comp,-DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_light_miss_sw_comp,radiance_receiver_light_miss.comp,))
$(eval $(call SHADER_VARIANT,radiance_receiver_light_miss_rt_comp,radiance_receiver_light_miss.comp,-DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,world_receiver_forward_lit_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_FULL_LIGHT=1))
$(eval $(call SHADER_VARIANT,alias_receiver_forward_lit_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_MODEL_CULL=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_FULL_LIGHT=1))
$(eval $(call SHADER_VARIANT,alias_receiver_forward_lit_alphatest_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_RECEIVER_REUSE=1 -DRC_RECEIVER_MODEL_CULL=1 -DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_FULL_LIGHT=1 -DALIAS_ALPHA_TEST=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_soft_sw_comp,radiance_receiver_direct.comp,-DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_SOFT_SHADOWS=1))
$(eval $(call SHADER_VARIANT,radiance_receiver_soft_rt_comp,radiance_receiver_direct.comp,-DRC_RECEIVER_TEXTURE_FILTER=1 -DRC_RECEIVER_SOFT_SHADOWS=1 -DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,world_radiance_vert,world.vert,-DRADIANCE_LIGHTING=1))
$(eval $(call SHADER_VARIANT,alias_radiance_vert,alias.vert,-DRADIANCE_LIGHTING=1))
$(eval $(call SHADER_VARIANT,alias_instanced_radiance_vert,alias_instanced.vert,-DRADIANCE_LIGHTING=1))
$(eval $(call SHADER_VARIANT,md5_radiance_vert,md5.vert,-DRADIANCE_LIGHTING=1))
$(eval $(call SHADER_VARIANT,md5_8_radiance_vert,md5.vert,-DRADIANCE_LIGHTING=1 -DEIGHT_WEIGHT_SKINNING))
$(eval $(call SHADER_VARIANT,world_radiance_sw_frag,world.frag,-DRADIANCE_LIGHTING=1))
$(eval $(call SHADER_VARIANT,world_radiance_sw_oit_frag,world.frag,-DRADIANCE_LIGHTING=1 -DWBOIT=1))
$(eval $(call SHADER_VARIANT,world_radiance_sw_mboit_moment_frag,world.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1))
$(eval $(call SHADER_VARIANT,world_radiance_sw_mboit_composite_frag,world.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,world_radiance_sw_mboit_composite_msaa_frag,world.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_frag,alias.frag,-DRADIANCE_LIGHTING=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_alphatest_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DALIAS_ALPHA_TEST=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_oit_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DWBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_alphatest_oit_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DALIAS_ALPHA_TEST=1 -DWBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_mboit_moment_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_alphatest_mboit_moment_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_alphatest_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,alias_radiance_sw_alphatest_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,md5_radiance_sw_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_radiance_sw_alphatest_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_radiance_sw_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_radiance_sw_alphatest_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,world_radiance_rt_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,world_radiance_rt_oit_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DWBOIT=1))
$(eval $(call SHADER_VARIANT,world_radiance_rt_mboit_moment_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1))
$(eval $(call SHADER_VARIANT,world_radiance_rt_mboit_composite_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,world_radiance_rt_mboit_composite_msaa_frag,world.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_alphatest_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_oit_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DWBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_alphatest_oit_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1 -DWBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_mboit_moment_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_alphatest_mboit_moment_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_alphatest_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,alias_radiance_rt_alphatest_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1))
$(eval $(call SHADER_VARIANT,md5_radiance_rt_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_radiance_rt_alphatest_mboit_composite_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_radiance_rt_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,md5_radiance_rt_alphatest_mboit_composite_msaa_frag,alias.frag,-DRADIANCE_LIGHTING=1 -DRC_HARDWARE_RAY_QUERY=1 -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4))
$(eval $(call SHADER_VARIANT,world_mesh_radiance_mesh,world_mesh.mesh,-DRADIANCE_LIGHTING=1 --target-env vulkan1.1 --target-env spirv1.4))
$(eval $(call SHADER_VARIANT,world_task_mesh_radiance_mesh,world_task_mesh.mesh,-DRADIANCE_LIGHTING=1 --target-env vulkan1.1 --target-env spirv1.4))

sinclude $(SHADER_OBJS:%.o=../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/%.d)

.PHONY:	clean debug release
.DEFAULT_GOAL := $(DEFAULT_TARGET)
