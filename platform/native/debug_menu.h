// platform/native/debug_menu.h
//
// Shared between the overlay's C half (debug_menu.c) and its C++ half
// (debug_menu_ui.cpp). Native-only: nothing in src/ includes this.
//
// Why two files: the overlay needs the game's headers to read live state, and
// those are C (include/sprite.h uses `template` as an identifier, which is a
// C++ keyword). ImGui, meanwhile, is C++ with no extern "C" guards. So the C
// file owns the command queue and every probe of game memory, and the C++ file
// owns the ImGui calls.
//
// THREADING. Two threads are involved:
//
//   * the game's thread, which is inside the VBlank handler when it renders a
//     frame, and which drains the command queue;
//   * the present thread (video.c), which owns the GL context and draws the
//     overlay.
//
// The rule the overlay follows is that it never writes a struct the game
// thread might be mid-update on. Everything it wants to change goes through
// native_debug_post_command() and is applied by the game thread at a point
// where it is not inside the PPU. Reads are the other way round: they are of
// live state the game thread does write, so individual fields can be a frame
// stale or torn. For a diagnostic that is the right trade -- it cannot affect
// the game -- but it does mean a value read twice may not match, and every
// read is volatile so the compiler cannot hoist one out of a frame.
//
// Licensed under the same terms as the pokeemerald decompilation.

#ifndef PLATFORM_NATIVE_DEBUG_MENU_H
#define PLATFORM_NATIVE_DEBUG_MENU_H

#include <stdbool.h>
#include <stdint.h>

// This header is included from C (debug_menu.c, which defines these) and from
// C++ (debug_menu_ui.cpp, which calls them). Without the guard the C++ side
// mangles every name and the link fails with C++-mangled undefined
// references.
#ifdef __cplusplus
extern "C" {
#endif

// Commands the overlay sends to the game thread. The payload is deliberately
// fixed-width and trivially copyable so the queue stays lock-free: a plain
// ring of these, published with release/acquire stores, no allocation and no
// mutex on either side.
enum debug_cmd
{
    DEBUG_CMD_NONE = 0,
    DEBUG_CMD_SET_LAYER_VISIBLE, // a = layer index, b = 0/1
    DEBUG_CMD_SET_WINDOW_VISIBLE, // a = 0/1
    DEBUG_CMD_SET_BLEND_ENABLED,  // a = 0/1
    DEBUG_CMD_POPULATE_QUEUE,     // a = 0/1
};

struct debug_cmd_t
{
    uint32_t type;
    uint32_t a;
    uint32_t b;
};

// Queues one command for the game thread. Called only from the present thread.
// Silently drops when the queue is full: a debug toggle that did not land is
// not worth stalling a frame for, and the UI reflects the true state anyway
// because it reads the flags back rather than caching its own.
void native_debug_post_command(uint32_t type, uint32_t a, uint32_t b);

// Applies everything the overlay has queued. Called from the game's thread at
// the top of the VBlank handler, before the PPU runs, so the flags it writes
// are settled before anything reads them.
void native_debug_drain_commands(void);

// Set by video.c's event loop when F1 or Ctrl+Shift+D goes down.
void native_debug_menu_request_toggle(void);

// True if the toggle key was pressed since the last call. video.c consumes it
// on the game's thread and forwards the result to the overlay, so the key is
// seen at the same point in the frame as every other input.
bool native_debug_menu_toggle_requested(void);

// --- C-half probes of live game state (debug_menu.c) ----------------------
//
// Each returns a freshly sampled value; nothing here caches between frames
// except where noted.

// Region usage for the memory panel. `used` is the highest non-zero byte plus
// one, i.e. the extent the region has actually been written to, not an
// allocation figure. That is the honest measure for mmap'd RAM here, and it is
// what makes the address-pinning slack visible.
struct debug_region
{
    const char *name;
    uint32_t base;
    uint32_t size;
    uint32_t used;
};

int native_debug_region_count(void);
const struct debug_region *native_debug_region(int index);

// Free slack in the emulated windows, which is what the fixed-address mmap
// contract depends on. Reported per region alongside the scan above.
uint32_t native_debug_region_slack(int index);

// A 256-byte snapshot of any mapped region, for the hex viewer. Returns false
// if the address is outside every mapped region, in which case `out` is
// untouched.
bool native_debug_read_hex(uint32_t addr, uint8_t *out);

// The ROM build's own free-slack figures, which are the ones the address
// pinning actually protects: EWRAM and IWRAM are sized by ld_script.ld, and
// the game is linked to fill them to within a few hundred bytes.
struct debug_slack
{
    uint32_t ewram_size, ewram_used, ewram_free;
    uint32_t iwram_size, iwram_used, iwram_free;
};

const struct debug_slack *native_debug_build_slack(void);

// Heap headroom, walked from the live block list in src/malloc.c.
void native_debug_heap(uint32_t *total, uint32_t *used);

// The gpu_regs.c shadow buffer and pending-write queue. Returned as pointers
// into the game's own storage so the panel can walk them.
const uint8_t *native_debug_gpu_shadow(void);
const uint8_t *native_debug_gpu_waiting(void);
uint32_t native_debug_gpu_pending_count(void);
bool native_debug_gpu_locked(void);

// DMA3 request queue depth against the 40 KiB/VBlank cap.
void native_debug_dma3(uint32_t *depth, uint32_t *capacity, uint32_t *bytes);

// Per-channel MusicPlayerInfo, sampled from gMPlayTable.
#define DEBUG_MAX_PLAYERS 8

struct debug_player
{
    const char *name;
    uint8_t numTracks;
    uint8_t priority;
    uint32_t status;
    uint32_t ident;
    uint16_t tempoU, tempoI, tempoD, tempoC;
    bool inUse;
};

int native_debug_player_count(void);
const struct debug_player *native_debug_player(int index);

// Flash save: sector id/signature per slot, plus the host file path.
#define DEBUG_MAX_SECTORS 32

struct debug_sector
{
    uint32_t index;
    uint16_t id;
    uint16_t checksum;
    uint32_t signature;
    uint32_t counter;
    bool valid; // signature == SECTOR_SIGNATURE
    const char *region;
};

int native_debug_sector_count(void);
const struct debug_sector *native_debug_sector(int index);
const char *native_debug_save_path(void);

// Timing figures.
struct debug_timing
{
    uint64_t cycles;
    uint32_t scanline;
    uint32_t vblankCounter1;
    uint32_t vblankCounter2;
    uint32_t vcount;
};

const struct debug_timing *native_debug_timing(void);

// Scanline-effect visualiser: the value the effect writes for each line, and
// the register it is aimed at.
const uint16_t *native_debug_scanline_values(int buffer);
uint32_t native_debug_scanline_count(void);
uint32_t native_debug_scanline_dest(void);
uint32_t native_debug_scanline_control(void);
int native_debug_scanline_active_buffer(void);

// Sprite/tile figures for the tiles panel.
void native_debug_sprites(uint32_t *oamLimit, uint32_t *reservedTiles, uint32_t *count);

// The OAM window, so the panel can dump raw OAM entries.
const uint8_t *native_debug_oam(void);

// The gSprites[] array as raw storage. The panel reads a handful of fields at
// their documented offsets rather than through struct Sprite, because the C++
// half cannot include sprite.h (it uses `template` as an identifier).
const uint8_t *native_debug_sprites_raw(void);

// Microseconds from CLOCK_MONOTONIC, for the overlay's own frame cost.
uint64_t native_debug_now_us(void);

// One row of the registers panel. The UI half cannot include io_reg.h's
// REG_OFFSET_* set without dragging in the game's typedefs, so the C half
// samples the emulated IO page and hands over plain values.
#define DEBUG_MAX_REGS 19

struct debug_reg
{
    const char *name;
    uint16_t value;
};

const struct debug_reg *native_debug_registers(int *count);

// The panel also wants the raw shadow buffer indexed by register offset; the
// C half resolves the offset for it.
uint16_t native_debug_io_read(uint32_t offset);

// Resolved BG bases for the tiles panel: the char and screen base addresses
// decoded out of BG0CNT..BG3CNT, which is where the PPU gets them too.
struct debug_bg
{
    uint16_t cnt;
    uint32_t charBase;  // absolute, in the emulated window
    uint32_t screenBase;
    uint8_t priority;
    bool affine;
    bool color256;
};

const struct debug_bg *native_debug_bg(int index);

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_NATIVE_DEBUG_MENU_H