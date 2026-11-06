# GNU Makefile for vkQuake unix x86_64 targets.
# You need the SDL3 (or SDL2, see USE_SDL3) library fully installed.
# "make DEBUG=1" to build a debug client.
# "make USE_SDL3=0" to build against SDL2 instead of SDL3.
# "make SDL_CONFIG=/path/to/sdl2-config" for unusual SDL2 installations.
# "make DO_USERDIRS=1" to enable user directories support
# "make VULKAN_SDK=/path/to/sdk" if it is not already in path

# Enable/Disable user directories support
DO_USERDIRS = 0

### Enable/Disable codecs for streaming music support
USE_CODEC_WAVE = 1
USE_CODEC_FLAC = 0
USE_CODEC_MP3 = 1
USE_CODEC_VORBIS = 1
USE_CODEC_OPUS = 0
# either xmp or mikmod (or modplug)
USE_CODEC_MIKMOD = 0
USE_CODEC_XMP = 0
USE_CODEC_MODPLUG = 0
USE_CODEC_UMX = 0

# which library to use for mp3 decoding: mad or mpg123
MP3LIB = mpg123
# which library to use for ogg decoding: vorbis or tremor
VORBISLIB = vorbis

# ---------------------------
# build variables
# ---------------------------
CPUFLAGS =
LDFLAGS ?=
DFLAGS ?=
CFLAGS ?=

ifeq ($(DO_USERDIRS),1)
CFLAGS += -DDO_USERDIRS=1
endif

# ---------------------------
# objects
# ---------------------------
SYSOBJ_NET := net_bsd.o net_udp.o
SYSOBJ_SYS := pl_linux.o sys_sdl.o sys_sdl_unix.o
DEFAULT_TARGET := vkquake
BINTOC_EXE = ../Shaders/bintoc

# ---------------------------
# common
# ---------------------------
include common.make

# ---------------------------
# libraries
# ---------------------------
ifeq ($(USE_SDL3),1)
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)
else
SDL_CONFIG ?= sdl2-config
SDL_CFLAGS := $(shell $(SDL_CONFIG) --cflags)
SDL_LIBS := $(shell $(SDL_CONFIG) --libs)
endif

ifneq ($(VULKAN_SDK),)
LDFLAGS += -L$(VULKAN_SDK)/lib
CFLAGS += -I$(VULKAN_SDK)/include
endif

ifeq ($(USE_CODEC_OPUS),1)
# opus and opusfile put their *.h under <includedir>/opus,
# but they include the headers without the opus directory
# prefix and rely on pkg-config. ewww...
CFLAGS += $(shell pkg-config --cflags opusfile)
CODECLIBS += $(shell pkg-config --libs opusfile)
endif

COMMON_LIBS:= -lm -lpthread

ifneq ($(HOST_OS),darwin)
COMMON_LIBS+= -lvulkan
CFLAGS+= -DLINUX
CFLAGS+= -D_GNU_SOURCE
else
ifeq ($(shell test -d /opt/homebrew && echo true),true)
COMMON_LIBS+= -L/opt/homebrew/lib
CFLAGS+= -I/opt/homebrew/include
endif
COMMON_LIBS+= -lMoltenVK
ifeq ($(USE_SDL3),1)
COMMON_LIBS+= -framework Foundation -framework GameController
endif
endif

LIBS := $(COMMON_LIBS) $(CODECLIBS)

# ---------------------------
# targets / rules
# ---------------------------
all: $(BINTOC_EXE) .WAIT $(DEFAULT_TARGET)

QCC_SRCS := qccmain.c qcc_pr_comp.c qcc_pr_lex.c packager.c decomp.c \
	comprout.c hash.c qcc_cmdlib.c qcd_main.c qcctui.c
QCC_SRCS := $(addprefix ../ThirdParty/fteqcc/,$(QCC_SRCS))

quiverscript: ../Tools/quiverscript.c
	$(CC) -std=c11 -O2 $< -o $@

fteqcc: $(QCC_SRCS)
	$(CC) -std=gnu17 -O2 -DNO_ZLIB -Wno-pointer-sign $(QCC_SRCS) -lm -o $@

id1-quakec/.stamp: $(wildcard ../ThirdParty/id1-quakec/*)
	mkdir -p id1-quakec
	cp -f ../ThirdParty/id1-quakec/* id1-quakec/
	touch $@

quiver_ripple/progs.dat: quiverscript fteqcc id1-quakec/.stamp ../Misc/scripts/ecs_ripple.qs
	mkdir -p quiver_ripple
	$(CURDIR)/quiverscript build --script $(CURDIR)/../Misc/scripts/ecs_ripple.qs --game-dir $(CURDIR)/quiver_ripple --qcc $(CURDIR)/fteqcc --qc-source $(CURDIR)/id1-quakec

vkquake: $(SHADER_OBJS) $(OBJS) ../Misc/editor_defaults.wad quiver_ripple/progs.dat
	$(LINKER) $(SHADER_OBJS) $(OBJS) $(LDFLAGS) $(LIBS) $(SDL_LIBS) -o $@
	cp -f ../Misc/editor_defaults.wad ./editor_defaults.wad

#Never strip, we want stack traces	
#	$(call DO_STRIP,$@)

release: vkquake
debug:
	$(error Use "make DEBUG=1")

clean:
	make -C ../Misc/vq_pak clean
	$(RM) -r .editor-tools
	$(RM) -r id1-quakec quiver_ripple
	$(RM) quiverscript fteqcc
	$(RM) *.o *.d $(DEFAULT_TARGET) \
	../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/*.c \
	../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/*.spv \
	../Shaders/Compiled/$(GLSLANG_OUT_FOLDER)/*.d

prefix ?= /usr
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/games
sbindir ?= $(exec_prefix)/sbin
INSTALL ?= install
INSTALL_PROGRAM ?= $(INSTALL)
INSTALL_DATA ?= ${INSTALL} -m 644
install: vkquake
	$(INSTALL_PROGRAM) $(CURDIR)/vkquake $(DESTDIR)$(bindir)/vkquake
	$(INSTALL_PROGRAM) $(CURDIR)/quiverscript $(DESTDIR)$(bindir)/quiverscript
	$(INSTALL_PROGRAM) $(CURDIR)/fteqcc $(DESTDIR)$(bindir)/fteqcc
	mkdir -p $(DESTDIR)$(bindir)/id1-quakec
	cp -f id1-quakec/* $(DESTDIR)$(bindir)/id1-quakec/
	$(INSTALL_DATA) ../Misc/editor_defaults.wad $(DESTDIR)$(bindir)/editor_defaults.wad
