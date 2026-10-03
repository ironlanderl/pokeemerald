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

# The game code is GBA C. These flags mirror the ROM build's MODERN=1 settings,
# with PLATFORM_NATIVE added to switch on the handful of host-specific paths.
CFLAGS := -O2 -g \
  -iquote $(ROOT)/include \
  -iquote $(CURDIR) \
  -DMODERN=1 -DPLATFORM_NATIVE=1 \
  -fno-strict-aliasing -fno-strict-overflow \
  -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
  -Werror=incompatible-pointer-types -Werror=implicit-function-declaration \
  $(SDL_CFLAGS)

PREPROC := $(abspath $(ROOT)/tools/preproc/preproc)

# --- sources -----------------------------------------------------------------

GAME_SRCS := $(wildcard $(ROOT)/src/*.c)
PLAT_SRCS := main.c memory.c timing.c ppu.c video.c bios.c dma.c stubs.c

GAME_OBJS := $(patsubst $(ROOT)/src/%.c,$(OUT)/src/%.o,$(GAME_SRCS))
PLAT_OBJS := $(patsubst %.c,$(OUT)/plat/%.o,$(PLAT_SRCS))

GEN_OBJS  := $(OUT)/rom_syms.o $(OUT)/rom_patches.o

# rompatch needs to know which symbols the native objects already define, so
# that ROM-resident names with a native C definition are not emitted twice.
ROMPATCH  := $(OUT)/rompatch

.PHONY: all clean run dirs
all: $(OUT)/$(TARGET)

dirs:
	@mkdir -p $(OUT)/src $(OUT)/plat $(OUT)/gen

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
NATIVE_EXTERN_SYMS := $(OUT)/extern_syms.txt

$(OUT)/extern_syms.txt: | dirs
	@{ \
	   nm -D --defined-only $$(ls /lib/*-linux-gnu/libc.so.6 2>/dev/null | head -1) 2>/dev/null; \
	   for l in $$(pkg-config --libs-only-L sdl2 gl 2>/dev/null); do \
	     nm -D --defined-only $$l/lib*.so* 2>/dev/null; \
	   done; \
	 } | awk 'NF==3 {sub(/@@.*$$/, "", $$3); print $$3}' | sort -u > $@

$(OUT)/native_syms.txt: $(GAME_OBJS) $(PLAT_OBJS) $(NATIVE_EXTERN_SYMS)
	@for f in $(GAME_OBJS) $(PLAT_OBJS); do \
	   nm --defined-only $$f 2>/dev/null | awk 'NF==3 {print $$3}'; done \
	  | sort -u > $@
	@cat $(NATIVE_EXTERN_SYMS) >> $@
	@sort -u -o $@ $@

$(OUT)/rom_syms.h $(OUT)/rom_patches.c $(OUT)/rom_syms.S: $(ROMPATCH) \
		$(ROOT)/pokeemerald.elf $(ROOT)/pokeemerald.map $(OUT)/native_syms.txt | dirs
	$(ROMPATCH) $(ROOT)/pokeemerald.elf $(ROOT)/pokeemerald.map $(ROOT)/build/emerald \
		$(OUT)/native_syms.txt $(OUT)/native_syms.txt \
		$(OUT)/rom_syms.h $(OUT)/rom_patches.c $(OUT)/rom_syms.S

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

$(OUT)/plat/%.o: %.c | dirs
	$(CC) $(CFLAGS) -I$(CURDIR) -c -o $@ $<

# --- 4. link -----------------------------------------------------------------
# native.ld is an order-only prerequisite: it must not reach the link line twice.
$(OUT)/$(TARGET): $(GAME_OBJS) $(PLAT_OBJS) $(GEN_OBJS) | native.ld
# -nostartfiles is deliberately NOT used: the port has no crt0 of its own, so
# the normal Scrt1.o provides _start, which runs the constructors libc needs
# (including SDL's) before main() forwards to native_main with a real argc/argv.
	$(CC) -no-pie -T native.ld -o $@ $(GAME_OBJS) $(PLAT_OBJS) $(GEN_OBJS) \
	   -lc -lm -lpthread $(SDL_LIBS)
	@echo "built $@"

run: all
	$(OUT)/$(TARGET) --rom $(ROOT)/pokeemerald.gba

clean:
	rm -rf $(OUT)