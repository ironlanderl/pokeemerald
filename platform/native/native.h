// platform/native/native.h
//
// Shared declarations for the native (SDL2 + OpenGL) Linux port.
//
// The port keeps the game's own code essentially untouched: the GBA's
// memory-mapped registers, VRAM, OAM and palette RAM are emulated by mapping
// real host memory at the real GBA addresses (see memory.c). Code that pokes
// REG_* therefore keeps working unchanged.
//
// Licensed under the same terms as the pokeemerald decompilation.

#ifndef PLATFORM_NATIVE_H
#define PLATFORM_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// GBA memory map
// ---------------------------------------------------------------------------

#define NATIVE_EWRAM_BASE 0x02000000u
#define NATIVE_EWRAM_SIZE 0x00040000u
#define NATIVE_IWRAM_BASE 0x03000000u
#define NATIVE_IWRAM_SIZE 0x00008000u
#define NATIVE_IO_BASE 0x04000000u
#define NATIVE_IO_SIZE 0x00100000u // 1 MiB: covers the 0x4FFF6xx debug regs
#define NATIVE_PLTT_BASE 0x05000000u
#define NATIVE_PLTT_SIZE 0x00000400u
#define NATIVE_VRAM_BASE 0x06000000u
#define NATIVE_VRAM_SIZE 0x00018000u
#define NATIVE_OAM_BASE 0x07000000u
#define NATIVE_OAM_SIZE 0x00000400u
#define NATIVE_ROM_BASE 0x08000000u
#define NATIVE_ROM_SIZE 0x01000000u // 16 MiB
#define NATIVE_FLASH_BASE 0x0E000000u
#define NATIVE_FLASH_SIZE 0x00020000u

// The cart GPIO port lives at 0x080000C4 and is written by RtcInit at boot.
// The first 64 KiB of the ROM window is mapped writable/private so those writes
// cannot fault or corrupt the ROM image.
#define NATIVE_ROM_RW_WINDOW 0x00010000u

// Convenient typed views into the emulated memory map.
#define NATIVE_EWRAM ((uint8_t *)NATIVE_EWRAM_BASE)
#define NATIVE_IWRAM ((uint8_t *)NATIVE_IWRAM_BASE)
#define NATIVE_IO ((uint8_t *)NATIVE_IO_BASE)
#define NATIVE_PLTT ((uint8_t *)NATIVE_PLTT_BASE)
#define NATIVE_VRAM ((uint8_t *)NATIVE_VRAM_BASE)
#define NATIVE_OAM ((uint8_t *)NATIVE_OAM_BASE)
#define NATIVE_ROM ((uint8_t *)NATIVE_ROM_BASE)
#define NATIVE_FLASH ((uint8_t *)NATIVE_FLASH_BASE)

// ---------------------------------------------------------------------------
// Timing constants (must match the retail hardware exactly)
// ---------------------------------------------------------------------------

#define NATIVE_TOTAL_SCANLINES 228
#define NATIVE_VBLANK_START 160
#define NATIVE_CYCLES_PER_FRAME 280896
#define NATIVE_CYCLES_PER_SCANLINE (NATIVE_CYCLES_PER_FRAME / NATIVE_TOTAL_SCANLINES)

// src/gpu_regs.c writes GPU registers straight through during this window.
#define NATIVE_FREE_WRITE_FIRST 161
#define NATIVE_FREE_WRITE_LAST 225

// ---------------------------------------------------------------------------
// memory.c
// ---------------------------------------------------------------------------

// Creates every emulated memory region. Must run before any game code.
bool native_memory_init(const char *rom_path, const char *save_path);

// Flushes/refreshes the flash mapping after a write.
void native_flash_sync(void);

const char *native_memory_error(void);

// True once the ROM has been mapped and its pointer tables patched.
bool native_rom_ready(void);

// ---------------------------------------------------------------------------
// Timing / interrupts (timing.c)
// ---------------------------------------------------------------------------

// Advances the virtual frame clock. Returns the number of scanlines elapsed.
void native_timing_init(void);
void native_timing_set_enabled(bool enabled);
bool native_timing_enabled(void);

// Current scanline, 0..227. This is what REG_VCOUNT reads.
uint8_t native_vcount(void);

// Total virtual cycles elapsed since start.
uint64_t native_cycle_count(void);

// Call the pending interrupt if IE & IF select one. Returns true if dispatched.
bool native_timing_run_interrupt(void);

// ---------------------------------------------------------------------------
// Platform services
// ---------------------------------------------------------------------------

void native_log(const char *fmt, ...);

#endif // PLATFORM_NATIVE_H