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

// This header is included from C (the whole platform layer) and from C++ (the
// ImGui overlay's UI half), so everything it declares is wrapped here.
#ifdef __cplusplus
extern "C" {
#endif

// SDL_Window is a typedef of `struct SDL_Window`; typedef it here so this
// header does not have to pull in SDL.h for the one accessor below. Including
// SDL.h twice is harmless (it has its own guard), but every C file in the
// platform layer would then be recompiled when SDL's headers change.
typedef struct SDL_Window SDL_Window;

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

// The host file currently mapped as the flash window. Shown by the debug menu.
const char *native_memory_save_path(void);

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

// The frame clock runs on its own thread because m4aSoundInit busy-waits for
// VCOUNT during boot, before the game loop starts. It owns VCOUNT only.
void native_timing_start_clock(void);
void native_timing_stop_clock(void);

// Block until the next VBlank, dispatching the game's handler.
void native_timing_advance_to_next_vblank(void);

// Interrupt enable/flag state, shared with src/gpu_regs.c and src/m4a.c.
void native_timing_set_ie(uint16_t v);
uint16_t native_timing_get_ie(void);
void native_timing_set_ime(uint16_t v);
uint16_t native_timing_get_ime(void);
void native_timing_raise(uint16_t flags);
uint16_t native_timing_get_if(void);
void native_timing_ack(uint16_t flags);

// ---------------------------------------------------------------------------
// Video (video.c) and input
// ---------------------------------------------------------------------------

// Creates the SDL2 window and GL context.
bool native_video_init(int scale);

// Hands the just-rendered frame to the present thread.
//
// This is deliberately a non-blocking publish, not a draw. It used to be
// native_video_render(), which uploaded the texture and blocked in
// SDL_GL_SwapWindow on vsync -- from inside the VBlank interrupt handler,
// which itself runs inside the game's WaitForVBlank spin. The game's VBlank
// servicing was therefore gated on the display, and there was nowhere off the
// interrupt path to run an overlay. See video.c.
void native_video_publish_frame(void);

// Blocks until the present thread has drained the last published frame and
// torn the GL context down. Called once, at shutdown.
void native_video_shutdown(void);

// Pumps SDL events and refreshes REG_KEYINPUT. Returns false if the window was
// closed.
//
// Runs on the game's thread inside the VBlank handler: the input has to be
// sampled at the top of VBlank (see the memory notes on ReadKeys and
// one-frame-late presses), which only this thread can honour.
bool native_input_poll(void);

// Hands every event the poll above dequeued to the overlay. Call from the
// present thread before native_debug_menu_render(). The queue is lock-free and
// sized to absorb a burst between two VBlanks; it drops rather than blocks.
void native_video_drain_events(void);

// The present thread's copy of the keyboard state, for the overlay to draw.
uint16_t native_video_last_keyinput(void);

// The window and GL context, for the ImGui SDL2 backend to bind to. Both are
// created on the game's thread (video.c) and handed to the present thread,
// which is where they stay current.
SDL_Window *native_video_window(void);
void *native_video_gl_context(void);

// ---------------------------------------------------------------------------
// PPU (ppu.c)
// ---------------------------------------------------------------------------

// Service any immediate DMA the game has armed since the last call.
void native_dma_service_now(void);

// Channel state for the debug overlay. Reads the emulated DMA registers only.
void native_dma_get_channel(int n, uint32_t *sad, uint32_t *dad, uint32_t *cnt);
bool native_dma_channel_armed(int n);
bool native_dma_channel_is_hblank(int n);

void native_ppu_render_frame(void);
const uint32_t *native_ppu_framebuffer(void);

// Layer isolation toggles for the debug menu: BG0..BG3, OBJ, backdrop.
extern bool native_ppu_layer_visible[6];

// Window and blend isolation, set by the debug menu through the command queue
// (never written directly by the overlay -- see debug_menu.c).
extern bool native_ppu_window_visible;
extern bool native_ppu_blend_enabled;

// ---------------------------------------------------------------------------
// Debug overlay (debug_menu.c)
// ---------------------------------------------------------------------------
//
// Split across two translation units because the overlay needs the game's
// headers (which are C -- sprite.h uses `template` as an identifier) and ImGui
// (which is C++ and has no extern "C" guards). debug_menu.c is the C half: the
// lock-free command queue and every read-only probe of live game state.
// debug_menu_ui.cpp is the C++ half: it drains that queue, then builds and
// draws the panels. Only debug_menu_ui.cpp talks to ImGui.

// Brings up the overlay. Call from the present thread, with a current GL
// context. A no-op if the context is unusable (the offscreen/headless path).
void native_debug_menu_init(void);

// Opens the overlay at startup instead of waiting for F1. Set from --menu.
void native_debug_menu_set_visible(bool visible);

// Drains the command queue and draws one frame of the overlay. Call from the
// present thread after the game's GL work for the frame, before the swap.
//
// `fb` is the RGBA frame the caller just drew, so the PPU inspector shows the
// same image that was presented rather than re-reading the game's framebuffer
// (which the game's thread is concurrently rewriting). NULL means "nothing new
// this iteration".
void native_debug_menu_render(const uint32_t *fb);

// Publishes the present thread's measured rate, for the overlay's header.
void native_debug_menu_note_frame(float fps);

// Feeds one SDL event to ImGui. Called from the present thread by
// native_video_drain_events(); the parameter is a `const SDL_Event *` in the
// video layer and is declared as an opaque pointer here so native.h does not
// have to include SDL.h.
void native_debug_menu_process_event(const void *sdlEvent);

void native_debug_menu_shutdown(void);

// ---------------------------------------------------------------------------
// Platform services
// ---------------------------------------------------------------------------

void native_log(const char *fmt, ...);

// Run a fixed number of frames and optionally write the last one to a PPM.
void native_set_headless(int frames, const char *shotPath);

// Simulate holding the given REG_KEYINPUT bits from `frame` onward.
void native_input_schedule(int frame, uint16_t bits);

// Set when the user closes the window; AgbMain's loop cannot return on its own.
void native_request_shutdown(void);
bool native_shutdown_requested(void);

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_NATIVE_H