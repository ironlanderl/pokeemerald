// platform/native/memory.c
//
// Maps the GBA's memory-mapped regions onto real host memory so that the
// game's existing REG_* / VRAM / OAM pointer arithmetic works unmodified.
//
// Layout (see native.h):
//   0x02000000  EWRAM   256 KiB
//   0x03000000  IWRAM    32 KiB
//   0x04000000  IO        1 MiB  (incl. the 0x4FFF6xx debug registers)
//   0x05000000  PLTT       1 KiB
//   0x06000000  VRAM      96 KiB
//   0x07000000  OAM         1 KiB
//   0x08000000  ROM       16 MiB  (file-backed, private + writable window)
//   0x0E000000  FLASH    128 KiB  (save file)
//   0x10000000  native .text  (set by native.ld; deliberately above the ROM)

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "native.h"

// Defined by the generated patch table (build/native/rom_patches.c).
struct rom_patch
{
    uint32_t rom_offset; // byte offset from ROM base
    uint32_t target;     // index into the native symbol table
};

extern const struct rom_patch g_rom_patches[];
extern const unsigned g_rom_patch_count;
extern const uint64_t g_rom_symbol_addrs[];
extern const unsigned g_rom_symbol_count;

static char s_error[512];
static bool s_rom_ready;

const char *native_memory_error(void)
{
    return s_error[0] ? s_error : NULL;
}

void native_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
}

// Map `len` bytes of anonymous memory at `base`.
static void *map_region(uint32_t base, size_t len)
{
    void *p = mmap((void *)(uintptr_t)base, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    return p;
}

// Map the ROM file at 0x08000000.
//
// The file is mapped MAP_PRIVATE so writes are copy-on-write and never reach
// disk. The first 64 KiB additionally covers the cart GPIO port at 0x080000C4,
// which RtcInit pokes at boot; keeping that window writable prevents a fault.
static uint8_t *map_rom(const char *rom_path)
{
    int fd = open(rom_path, O_RDONLY);
    if (fd < 0)
    {
        set_error("cannot open ROM '%s': %s", rom_path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0)
    {
        set_error("cannot stat ROM: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    if ((size_t)st.st_size < NATIVE_ROM_SIZE)
    {
        set_error("ROM is %lld bytes, expected at least %u", (long long)st.st_size,
                  NATIVE_ROM_SIZE);
        close(fd);
        return NULL;
    }

    void *p = mmap((void *)(uintptr_t)NATIVE_ROM_BASE, NATIVE_ROM_SIZE, PROT_READ,
                   MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
    {
        set_error("cannot map ROM at 0x%08x: %s", NATIVE_ROM_BASE, strerror(errno));
        return NULL;
    }

    // Widen the first window to read/write. Splitting a mapping's protection
    // requires the new mapping to cover whole pages, so round up to 64 KiB
    // (the page size on every platform we target).
    if (mprotect((void *)(uintptr_t)NATIVE_ROM_BASE, NATIVE_ROM_RW_WINDOW,
                 PROT_READ | PROT_WRITE) != 0)
    {
        set_error("cannot make ROM GPIO window writable: %s", strerror(errno));
        return NULL;
    }
    return (uint8_t *)p;
}

// Map the save file at 0x0E000000, creating it if absent. The whole region is
// writable so the existing flash write state machine in src/agb_flash.c works
// unchanged.
static uint8_t *map_flash(const char *save_path)
{
    int fd = open(save_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
    {
        set_error("cannot open save '%s': %s", save_path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0)
    {
        set_error("cannot stat save: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    // Flash chips are sector-addressed; present the full size so a short save
    // still reads back as blank rather than faulting.
    void *p = mmap((void *)(uintptr_t)NATIVE_FLASH_BASE, NATIVE_FLASH_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
    {
        // File is shorter than the window: fall back to a private anonymous
        // region so the game still runs (saves simply won't persist).
        p = mmap((void *)(uintptr_t)NATIVE_FLASH_BASE, NATIVE_FLASH_SIZE,
                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    }
    close(fd);
    if (p == MAP_FAILED)
    {
        set_error("cannot map save at 0x%08x: %s", NATIVE_FLASH_BASE, strerror(errno));
        return NULL;
    }
    return (uint8_t *)p;
}

// Rewrite the ROM's baked-in 32-bit pointers so they point at the native
// binary instead of the original ARM code.
//
// The ROM was linked for ARM, so tables like gScriptCmdTable contain values
// like 0x080992cd (an ARM entry point). Every such word is listed in the
// generated patch table and rewritten to the native address of the same
// symbol. Anything the game never dereferences is left alone.
static void apply_rom_patches(uint8_t *rom)
{
    for (unsigned i = 0; i < g_rom_patch_count; i++)
    {
        const struct rom_patch *p = &g_rom_patches[i];
        if (p->target >= g_rom_symbol_count)
            continue;
        uint64_t addr = g_rom_symbol_addrs[p->target];
        if (addr == 0)
            continue; // symbol not present in the native binary
        uint32_t value = (uint32_t)addr;
        memcpy(rom + p->rom_offset, &value, sizeof(value));
    }
}

bool native_memory_init(const char *rom_path, const char *save_path)
{
    s_error[0] = '\0';

    static const struct
    {
        uint32_t base;
        size_t len;
        const char *name;
    } regions[] = {
        {NATIVE_EWRAM_BASE, NATIVE_EWRAM_SIZE, "EWRAM"},
        {NATIVE_IWRAM_BASE, NATIVE_IWRAM_SIZE, "IWRAM"},
        {NATIVE_IO_BASE, NATIVE_IO_SIZE, "IO"},
        {NATIVE_PLTT_BASE, NATIVE_PLTT_SIZE, "PLTT"},
        {NATIVE_VRAM_BASE, NATIVE_VRAM_SIZE, "VRAM"},
        {NATIVE_OAM_BASE, NATIVE_OAM_SIZE, "OAM"},
    };

    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
    {
        if (!map_region(regions[i].base, regions[i].len))
        {
            set_error("cannot map %s at 0x%08x: %s", regions[i].name, regions[i].base,
                      strerror(errno));
            return false;
        }
    }

    if (!map_rom(rom_path))
        return false;
    if (!map_flash(save_path))
        return false;

    apply_rom_patches(NATIVE_ROM);
    s_rom_ready = true;
    return true;
}

bool native_rom_ready(void)
{
    return s_rom_ready;
}

void native_flash_sync(void)
{
    // The save is MAP_SHARED against the file, so the kernel has already
    // written it back. msync is advisory but makes the durability explicit.
    msync((void *)(uintptr_t)NATIVE_FLASH_BASE, NATIVE_FLASH_SIZE, MS_SYNC);
}