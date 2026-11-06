# Build Windows runtime tools with the target compiler, but build the demo
# package with host tools so Linux-to-Windows cross compilation needs no Wine.
HOSTCC ?= cc
ifeq ($(OS),Windows_NT)
QSHOST_SUFFIX := .exe
endif
QCC_TOOL_SOURCES := qccmain.c qcc_pr_comp.c qcc_pr_lex.c packager.c decomp.c \
    comprout.c hash.c qcc_cmdlib.c qcd_main.c qcctui.c
QCC_TOOL_SOURCES := $(addprefix ../ThirdParty/fteqcc/,$(QCC_TOOL_SOURCES))

quiverscript.exe: ../Tools/quiverscript.c
	$(CC) -std=c11 -O2 -static-libgcc $< -o $@

fteqcc.exe: $(QCC_TOOL_SOURCES)
	$(CC) -std=gnu17 -O2 -DNO_ZLIB -Wno-pointer-sign -static-libgcc $(QCC_TOOL_SOURCES) -lm -o $@

.editor-tools/quiverscript-host$(QSHOST_SUFFIX): ../Tools/quiverscript.c
	mkdir -p .editor-tools
	$(HOSTCC) -std=c11 -O2 $< -o $@

.editor-tools/fteqcc-host$(QSHOST_SUFFIX): $(QCC_TOOL_SOURCES)
	mkdir -p .editor-tools
	$(HOSTCC) -std=gnu17 -O2 -DNO_ZLIB -Wno-pointer-sign $(QCC_TOOL_SOURCES) -lm -o $@

id1-quakec/.stamp: $(wildcard ../ThirdParty/id1-quakec/*)
	mkdir -p id1-quakec
	cp -f ../ThirdParty/id1-quakec/* id1-quakec/
	touch $@

quiver_ripple/progs.dat: quiverscript.exe fteqcc.exe id1-quakec/.stamp \
    .editor-tools/quiverscript-host$(QSHOST_SUFFIX) .editor-tools/fteqcc-host$(QSHOST_SUFFIX) ../Misc/scripts/ecs_ripple.qs
	mkdir -p quiver_ripple
	"$(CURDIR)/.editor-tools/quiverscript-host$(QSHOST_SUFFIX)" build \
	  --script "$(CURDIR)/../Misc/scripts/ecs_ripple.qs" --game-dir "$(CURDIR)/quiver_ripple" \
	  --qcc "$(CURDIR)/.editor-tools/fteqcc-host$(QSHOST_SUFFIX)" --qc-source "$(CURDIR)/id1-quakec"
