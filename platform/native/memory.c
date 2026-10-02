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
extern const char *const g_rom_symbol_names[];
extern const unsigned g_rom_symbol_count;

// Addresses of every symbol the patch table can target, emitted by
// tools/rompatch. ROM-resident entries are the ROM address; entries that were
// recompiled natively are emitted as C references so the native linker
// resolves them. A zero entry means the symbol was local in the ROM build and
// cannot be referenced natively, so that word is left alone.
extern const uint64_t g_rom_symbol_addrs[];

static char s_error[512];
static bool s_rom_ready;

const char *native_memory_error(void)
{
    return s_error[0] ? s_error : NULL;
}

// Diagnostics go through write(2) rather than stdio: before the memory map is
// up, libc's FILE layer may allocate inside the very window we are about to
// claim, and a lazy stdio buffer turns that into a confusing crash.
void native_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    ssize_t ignored = write(2, buf, (size_t)n);
    (void)ignored;
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
}


// Direct syscall wrappers. See sys_mmap() for why libc's PLT is bypassed.
static long sys_call(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
}

#define SYS_openat 56
#define SYS_close 57
#define SYS_fstat 80
#define SYS_ftruncate 46
#define SYS_msync 26
#define SYS_munmap 215
#define SYS_read 63

// Path string passed to openat() must live on a page that is mapped; our own
// .bss is fine.
static int sys_open_ro(const char *path)
{
    return (int)sys_call(SYS_openat, -100 /*AT_FDCWD*/, (long)path,
                         O_RDONLY | O_CLOEXEC, 0, 0, 0);
}

static int sys_open_rw(const char *path, int flags, mode_t mode)
{
    return (int)sys_call(SYS_openat, -100 /*AT_FDCWD*/, (long)path, flags, (long)mode, 0, 0);
}

static int sys_fstat(int fd, struct stat *st)
{
    return (int)sys_call(SYS_fstat, fd, (long)st, 0, 0, 0, 0);
}

static int sys_ftruncate(int fd, off_t len)
{
    return (int)sys_call(SYS_ftruncate, fd, (long)len, 0, 0, 0, 0);
}

static int sys_close(int fd)
{
    return (int)sys_call(SYS_close, fd, 0, 0, 0, 0, 0);
}

// Issue the mmap syscall directly.
//
// Going through libc's mmap() depends on a PLT stub whose GOT slot must match
// the R_AARCH64_JUMP_SLOT relocation; getting that wrong makes the call land in
// fopen() with mmap's arguments, which faults. The syscall number is stable on
// aarch64 (SYS_mmap == 222) and needs no relocation at all, so it cannot be
// misdirected.
static long sys_mmap(uintptr_t addr, size_t len, uint32_t prot, uint32_t flags, int fd,
                     off_t off)
{
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = (long)len;
    register long x2 __asm__("x2") = (long)prot;
    register long x3 __asm__("x3") = (long)flags;
    register long x4 __asm__("x4") = (long)fd;
    register long x5 __asm__("x5") = (long)off;
    register long x8 __asm__("x8") = 222; // __NR_mmap on aarch64
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
}

// Map `len` bytes of anonymous memory at `base`.
static void *map_region(uint32_t base, size_t len)
{
    void *p = (void *)sys_mmap((uintptr_t)base, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
    {
        native_log("mmap 0x%08x len=0x%zx failed: %s", base, len, strerror(errno));
        return NULL;
    }
    return p;
}

// Map the ROM file at 0x08000000.
//
// The whole image must be writable: the pointer-patch pass rewrites words
// throughout it (up to ~9.6 MiB in), and RtcInit pokes the cartridge GPIO port
// at 0x080000C4. The mapping is MAP_PRIVATE, so these writes are copy-on-write
// and never reach the file on disk.
static uint8_t *map_rom(const char *rom_path)
{
    int fd = sys_open_ro(rom_path);
    if (fd < 0)
    {
        set_error("cannot open ROM '%s': %s", rom_path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (sys_fstat(fd, &st) != 0)
    {
        set_error("cannot stat ROM: %s", strerror(errno));
        sys_close(fd);
        return NULL;
    }
    if ((size_t)st.st_size < NATIVE_ROM_SIZE)
    {
        set_error("ROM is %lld bytes, expected at least %u", (long long)st.st_size,
                  NATIVE_ROM_SIZE);
        sys_close(fd);
        return NULL;
    }

    void *p = (void *)sys_mmap((uintptr_t)NATIVE_ROM_BASE, NATIVE_ROM_SIZE,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
    sys_close(fd);
    if (p == MAP_FAILED)
    {
        set_error("cannot map ROM at 0x%08x: %s", NATIVE_ROM_BASE, strerror(errno));
        return NULL;
    }

    return (uint8_t *)p;
}

// Map the save file at 0x0E000000, creating it if absent. The whole region is
// writable so the existing flash write state machine in src/agb_flash.c works
// unchanged.
static uint8_t *map_flash(const char *save_path)
{
    int fd = sys_open_rw(save_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        set_error("cannot open save '%s': %s", save_path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (sys_fstat(fd, &st) != 0)
    {
        set_error("cannot stat save: %s", strerror(errno));
        sys_close(fd);
        return NULL;
    }

    // A shared file mapping only covers whole pages that exist in the file.
    // A fresh (or short) save would map successfully yet SIGBUS on every
    // access, so make the file exactly the flash size first.
    if (sys_ftruncate(fd, (off_t)NATIVE_FLASH_SIZE) != 0)
    {
        set_error("cannot size save to %u bytes: %s", NATIVE_FLASH_SIZE, strerror(errno));
        close(fd);
        return NULL;
    }

    void *p = (void *)sys_mmap((uintptr_t)NATIVE_FLASH_BASE, NATIVE_FLASH_SIZE,
                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
    {
        // Fall back to a private anonymous region so the game still runs
        // (saves simply will not persist).
        p = (void *)sys_mmap((uintptr_t)NATIVE_FLASH_BASE, NATIVE_FLASH_SIZE,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
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
    if (g_rom_patch_count == 0)
    {
        native_log("warning: ROM patch table is empty; pointer tables will be stale");
        return;
    }

    native_log("applying %u ROM pointer patches", g_rom_patch_count);

    // The generated table holds, for every patch target, the address it should
    // take: either the ROM address (data still read in place) or the native
    // address of a symbol that was recompiled. A zero means the symbol was
    // local to the ROM build and cannot be referenced natively, in which case
    // the original ROM word is already correct for data, and the game never
    // calls through it.
    for (unsigned i = 0; i < g_rom_patch_count; i++)
    {
        const struct rom_patch *p = &g_rom_patches[i];
        if (p->target >= g_rom_symbol_count)
            continue;

        uint64_t addr = g_rom_symbol_addrs[p->target];
        if (addr == 0)
            continue;

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
        native_log("mapping %s at 0x%08x len 0x%zx", regions[i].name, regions[i].base, regions[i].len);
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