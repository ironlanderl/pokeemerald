# platform/native/Makefile
#
# Builds the native Linux port of pokeemerald.
#
# The pipeline is:
#   1. build the ROM (and its ELF) with the normal, unmodified ROM build, so
#      the assets and the symbol layout are exactly the retail ones;
#   2. run tools/rompatch to emit the absolute symbol definitions and the
#      pointer-patch table;
#   3. compile every game source natively for the host, plus the platform layer;
#   4. link at 0x10000000, above the emulated GBA window.
#
# Usage:
#   make -C platform/native            # native build for this host
#   make -C platform/native ARCH=x86_64
#   make -C platform/native run         # build and run
#
# Nothing here is committed: build/native/ is generated, like build/.

ARCH      ?=
OUT       := build/native
ROOT      := ../..
TARGET    := pokeemerald-native

# Cross toolchain prefix. Empty means build for the host directly.
ifeq ($(ARCH),)
  CC  := gcc
  OBJDUMP :=
else
  CC  := $(ARCH)-linux-gnu-gcc
  OBJDUMP := $(CC) -dumpversion
endif

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 glew)
SDL_LIBS   := $(shell pkg-config --libs sdl2 glew)

# Passing native.ld with -T replaces ld's default linker script, and with it the
# SEARCH_DIR list saying where libraries live. ld then cannot find the transitive
# dependencies of the SDL/GL libraries -- libGLdispatch, libGLX, libxcb and
# libstdc++, none of which carry an RPATH -- and the link dies with "undefined
# reference" for their symbols. -L does not help: it only governs libraries named
# on the command line, not indirect ones. -rpath-link is the option that governs
# that lookup. The directory comes from pkg-config so it tracks the layout of
# whichever machine builds this: /usr/lib on a single-prefix distro, the multiarch
# directory on a Debian derivative.
RPATH_LINK := -Wl,-rpath-link,$(shell pkg-config --variable=libdir sdl2)

# The game code is GBA C. These flags mirror the ROM build's MODERN=1 settings,
# with PLATFORM_NATIVE added to switch on the handful of host-specific paths.
# The GBA is 32-bit, so the game freely converts between u32 script operands and
# pointers (T1_READ_PTR and friends, (void *)BG_CHAR_ADDR, (u32)fn >> 16 for the
# split task pointers, REG_* indices). Those are all benign widenings from a 32-bit
# value. The genuinely dangerous case is a *function* pointer built by casting an
# integer: that is what the agb_flash.c Thumb-bit trampolines did, and it crashed
# every save. Those are gone now (see SetReadFlash1 / VerifyFlashSector), so this
# pair of flags only hides the benign remainder -- removing it re-surfaces ~850
# warnings spread over ~50 shared src/ files with no functional gain.
CFLAGS := -O2 -g \
  -iquote $(ROOT)/include \
  -iquote $(CURDIR) \
  -DMODERN=1 -DPLATFORM_NATIVE=1 \
  -fno-strict-aliasing -fno-strict-overflow \
  -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
  -Werror=incompatible-pointer-types -Werror=implicit-function-declaration \
  $(SDL_CFLAGS)

# Per-stage frame profiler (timing.c). Off by default; it adds two clock_gettime
# calls per frame and an atexit report. Build with `make PROFILE=1`.
ifeq ($(PROFILE),1)
  CFLAGS += -DNATIVE_PROFILE
endif

# Escape hatch for ad-hoc defines, e.g. `make PROFILE=1 EXTRA_CFLAGS=-DPPU_ABLATE_OBJ`.
# Used to attribute PPU cost by ablation (compile out one stage, re-measure).
CFLAGS += $(EXTRA_CFLAGS)

PREPROC := $(abspath $(ROOT)/tools/preproc/preproc)

# --- sources -----------------------------------------------------------------

GAME_SRCS := $(wildcard $(ROOT)/src/*.c)
PLAT_SRCS := main.c memory.c timing.c ppu.c video.c bios.c dma.c \
             mixer.c seq.c pcmout.c debug_menu.c stubs.c

GAME_OBJS := $(patsubst $(ROOT)/src/%.c,$(OUT)/src/%.o,$(GAME_SRCS))
PLAT_OBJS := $(patsubst %.c,$(OUT)/plat/%.o,$(PLAT_SRCS))

GEN_OBJS  := $(OUT)/rom_syms.o $(OUT)/rom_patches.o

# --- Dear ImGui debug overlay ----------------------------------------------
#
# Vendored from upstream v1.92.9b (commit f1cc2ae), unmodified: only the core
# and the two backends this port uses. examples/ and imgui_demo.cpp are
# deliberately not vendored -- the demo window is ~10k lines the overlay does
# not need.
#
# ImGui is C++ with no extern "C" guards, and the overlay has to read the game's
# headers, which are C (include/sprite.h uses `template` as an identifier). So
# the overlay is split: debug_menu.c is the C half (command queue, read-only
# probes of live game state) and debug_menu_ui.cpp is the C++ half (the panels).
IMGUI_DIR := imgui
IMGUI_SRCS := \
  $(IMGUI_DIR)/imgui.cpp \
  $(IMGUI_DIR)/imgui_draw.cpp \
  $(IMGUI_DIR)/imgui_tables.cpp \
  $(IMGUI_DIR)/imgui_widgets.cpp \
  $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp \
  $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
IMGUI_OBJS := $(patsubst $(IMGUI_DIR)/%.cpp,$(OUT)/imgui/%.o,$(IMGUI_SRCS))

# CXX follows CC so a cross build stays consistent.
ifeq ($(ARCH),)
  CXX := g++
else
  CXX := $(ARCH)-linux-gnu-g++
endif

# The ImGui OpenGL3 backend would otherwise pull in its own bundled gl3w
# loader (imgui_impl_opengl3_loader.h), which this port deliberately does not
# vendor: video.c already uses GLEW, and having two loaders disagree about the
# entry points is how you get a backend that works in isolation and not in the
# program. IMGUI_IMPL_OPENGL_LOADER_CUSTOM tells it to use ours, and
# -include GL/glew.h puts those declarations ahead of the backend's own body.
#
# imgui_impl_sdl2.cpp needs its own -I for <SDL.h>.
IMGUI_CXXFLAGS := -O2 -g -std=c++17 \
  -DIMGUI_IMPL_OPENGL_LOADER_CUSTOM \
  -include GL/glew.h \
  -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends \
  $(SDL_CFLAGS)

UI_CXXFLAGS := -O2 -g -std=c++17 \
  -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends \
  $(SDL_CFLAGS)

# rompatch needs to know which symbols the native objects already define, so
# that ROM-resident names with a native C definition are not emitted twice.
ROMPATCH  := $(OUT)/rompatch

.PHONY: all clean run dirs
all: $(OUT)/$(TARGET)

dirs:
	@mkdir -p $(OUT)/src $(OUT)/plat $(OUT)/gen $(OUT)/imgui $(OUT)/imgui/backends

# --- 1. the ROM build --------------------------------------------------------
#
# Unmodified: this produces the byte-exact retail ROM plus the ARM ELF that
# rompatch reads for symbols and relocations.
$(ROOT)/pokeemerald.gba $(ROOT)/pokeemerald.elf $(ROOT)/pokeemerald.map:
	$(MAKE) -C $(ROOT) compare

# --- 2. rompatch -------------------------------------------------------------
$(ROMPATCH): $(ROOT)/tools/rompatch/rompatch.c | dirs
	$(CC) -O2 -o $@ $< -lelf

# Symbols the native link already provides. Anything here must NOT also be
# emitted as an absolute ROM address, or the native linker resolves calls to it
# and jumps into the mapped ROM image instead -- which is exactly what
# happened with strcpy.
#
# Derived from the host libc/SDL/GL rather than hand-listed: an earlier
# hand-written list missed strcpy, and the game called into the ROM image and
# died with SIGBUS the first time a real GL context made that path reachable.
# Symbols the native link already provides. Anything here must NOT also be
# emitted as an absolute ROM address, or the native linker resolves calls to it
# and jumps into the mapped ROM image instead. That is what happened with
# strcpy: the first hand-written exclusion list missed it, and the port died
# with SIGBUS the moment a real GL context made the path reachable.
#
# Derived from the host libc and the SDL/GL libraries rather than hand-listed,
# so this cannot silently drift out of date again.
#
# Ask $(CC) where libc.so.6 is rather than globbing for it. The list used to be
# read from /lib/*-linux-gnu/libc.so.6, which only exists on a Debian-style
# multiarch layout; anywhere else `ls` failed, `nm` was handed no file, and the
# exclusion list came out EMPTY -- silently, because the pipeline's `2>/dev/null`
# swallowed the error. That put memcpy, memset, strcmp and strcpy back into the
# ROM as absolute addresses, and the binary died in a std::locale constructor
# during static init, before main(): libstdc++ called memset and jumped into the
# mapped ROM's ARM code at 0x082e9435.
NATIVE_EXTERN_SYMS := $(OUT)/extern_syms.txt

$(OUT)/extern_syms.txt: | dirs
	@{ \
	   nm -D --defined-only "$$($(CC) -print-file-name=libc.so.6)" 2>/dev/null; \
	   for l in $$(pkg-config --libs-only-L sdl2 gl 2>/dev/null); do \
	     nm -D --defined-only $$l/lib*.so* 2>/dev/null; \
	   done; \
	 } | awk 'NF==3 {sub(/@@.*$$/, "", $$3); print $$3}' | sort -u > $@

$(OUT)/native_syms.txt: $(GAME_OBJS) $(PLAT_OBJS) $(CXX_OBJS) $(NATIVE_EXTERN_SYMS)
	@for f in $(GAME_OBJS) $(PLAT_OBJS) $(CXX_OBJS); do \
	   nm --defined-only --demangle $$f 2>/dev/null | awk 'NF==3 {print $$3}'; \
	   nm --defined-only $$f 2>/dev/null | awk 'NF==3 {print $$3}'; done \
	  | sort -u > $@
	@cat $(NATIVE_EXTERN_SYMS) >> $@
	@sort -u -o $@ $@

# Produce all three outputs together, stamped by a sentinel, so that a parallel
# build cannot assemble rom_syms.o from the previous run's .S. With -j the old
# multi-target rule let make satisfy rom_syms.o first and rompatch rewrite the
# .S afterwards, leaving an object that was missing symbols the .S plainly
# contained ("undefined reference to gText_ApprenticeWhichMon0").
STAMP := $(OUT)/.rompatch.stamp

$(STAMP): $(ROMPATCH) $(ROOT)/pokeemerald.elf $(ROOT)/pokeemerald.map $(OUT)/native_syms.txt | dirs
	$(ROMPATCH) $(ROOT)/pokeemerald.elf $(ROOT)/pokeemerald.map $(ROOT)/build/emerald \
		$(OUT)/native_syms.txt $(OUT)/native_syms.txt \
		$(OUT)/rom_syms.h $(OUT)/rom_patches.c $(OUT)/rom_syms.S
	@touch $@

$(OUT)/rom_syms.h $(OUT)/rom_patches.c $(OUT)/rom_syms.S: $(STAMP)
	@:  # generated together by the stamp rule above

# Recompile whenever the generated assembly changes. Editing rom_syms.S by hand
# used to leave a stale .o that still lacked the symbols, producing confusing
# "undefined reference" errors for data that was plainly in the .S.
$(OUT)/rom_syms.o: $(OUT)/rom_syms.S native.ld
	$(CC) -c -x assembler-with-cpp -o $@ $<

$(OUT)/rom_patches.o: $(OUT)/rom_patches.c
	$(CC) -c -O1 -o $@ $<

# --- 3. compile --------------------------------------------------------------
#
# Two stages, matching the ROM build: cpp, then tools/preproc (which expands
# INCGFX/INCBIN into byte arrays and converts text via charmap.txt), then cc.
$(OUT)/src/%.o: $(ROOT)/src/%.c | dirs
	@$(CC) -E -iquote $(ROOT)/include -iquote $(CURDIR) \
	   -DMODERN=1 -DPLATFORM_NATIVE=1 -Wno-trigraphs $< -o $(OUT)/src/$*.i
	@# preproc resolves INCGFX/INCBIN paths relative to its working directory,
	@# which must be the repo root (the ROM build runs from there too).
	@cd $(ROOT) && $(PREPROC) -i -g build/assets $< charmap.txt \
	   < $(CURDIR)/$(OUT)/src/$*.i > $(CURDIR)/$(OUT)/src/$*.pp.c
	@$(CC) $(CFLAGS) -c -o $@ $(OUT)/src/$*.pp.c

$(OUT)/plat/%.o: %.c native.h debug_menu.h | dirs
	$(CC) $(CFLAGS) -I$(CURDIR) -c -o $@ $<

# --- 3b. C++ overlay -------------------------------------------------------
$(OUT)/imgui/%.o: $(IMGUI_DIR)/%.cpp | dirs
	$(CXX) $(IMGUI_CXXFLAGS) -c -o $@ $<

# debug_menu_ui.cpp and debug_menu.c share debug_menu.h, and the linkage
# between them depends on it: without the extern "C" guard the C++ half
# mangles every name the C half defines and the link fails. Listing the header
# as a prerequisite matters for the same reason -- an edit to it has to
# invalidate both objects, or the next build links a stale one against a fresh
# one.
$(OUT)/plat/debug_menu_ui.o: debug_menu_ui.cpp debug_menu.h native.h | dirs
	$(CXX) $(UI_CXXFLAGS) -I$(CURDIR) -c -o $@ $<

CXX_OBJS := $(OUT)/plat/debug_menu_ui.o $(IMGUI_OBJS)

# --- 4. link -----------------------------------------------------------------
# native.ld is an order-only prerequisite: it must not reach the link line twice.
#
# The C++ objects come last and pull in libstdc++ explicitly: the link driver is
# $(CC) (gcc), not $(CXX), so nothing would add the C++ runtime otherwise and the
# ImGui statics in it would be left undefined. -lstdc++ is listed after the
# objects for the ordinary reason -- link order -- and also on the command line
# so the ordering does not depend on where the objects sit.
$(OUT)/$(TARGET): $(GAME_OBJS) $(PLAT_OBJS) $(CXX_OBJS) $(GEN_OBJS) | native.ld
# -nostartfiles is deliberately NOT used: the port has no crt0 of its own, so
# the normal Scrt1.o provides _start, which runs the constructors libc needs
# (including SDL's) before main() forwards to native_main with a real argc/argv.
	$(CC) -no-pie -T native.ld -o $@ $(GAME_OBJS) $(PLAT_OBJS) $(CXX_OBJS) $(GEN_OBJS) \
	   -lc -lm -lpthread -lstdc++ $(SDL_LIBS) $(RPATH_LINK)
	@echo "built $@"

run: all
	$(OUT)/$(TARGET) --rom $(ROOT)/pokeemerald.gba

clean:
	rm -rf $(OUT)