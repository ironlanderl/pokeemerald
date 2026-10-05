// platform/native/debug_menu.c
//
// The C half of the Dear ImGui debug overlay: the lock-free command queue to
// the game's thread, plus every read-only probe of live game state that the
// C++ half (debug_menu_ui.cpp) draws.
//
// See debug_menu.h for the threading contract. The short version: this file
// only ever *reads* game memory, and everything it wants to change is posted
// to the game's thread rather than written here.
//
// Licensed under the same terms as the pokeemerald decompilation.

#include "debug_menu.h"

#include "global.h"
#include "main.h"
#include "malloc.h"
#include "save.h"
#include "sprite.h"
#include "scanline_effect.h"
#include "gba/io_reg.h"
#include "gba/m4a_internal.h"
#include "gba/defines.h"

#include "native.h"

#include <string.h>
#include <time.h>

// Accessors the two game files expose for the overlay, both guarded by
// PLATFORM_NATIVE so they do not exist in the ROM build. See dma3_manager.c
// and gpu_regs.c.
u32 GetDma3QueueDepth(void);
u32 GetDma3QueueCapacity(void);
u8 IsDma3QueueLocked(void);
u32 GetDma3PendingBytes(void);
const u8 *GetGpuRegShadowBuffer(void);
const u8 *GetGpuRegWaitingList(void);
u8 IsGpuRegBufferLocked(void);
u32 CountPendingGpuRegWrites(void);

// NUM_LAYERS is private to ppu.c's enum; native.h declares the visibility
// array as [6], so mirror the count rather than reaching into that file.
#define DEBUG_NUM_LAYERS 6

// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------
//
// Single producer (the present thread, via the overlay), single consumer (the
// game's thread, at the top of the VBlank handler). Power-of-two so the index
// wrap is a mask, and the head/tail are released/acquired so a command's
// payload is visible before the consumer can see the slot as filled.
//
// Overflow drops the newest command. That is deliberate: a full queue means
// the game is not draining, and blocking the frame to record a debug toggle
// would be the exact failure this overlay exists to avoid.

#define DEBUG_QUEUE_SIZE 256
#define DEBUG_QUEUE_MASK (DEBUG_QUEUE_SIZE - 1)

static struct debug_cmd_t s_queue[DEBUG_QUEUE_SIZE];
static volatile uint32_t s_queue_head; // producer writes, consumer reads
static volatile uint32_t s_queue_tail; // consumer writes, producer reads

void native_debug_post_command(uint32_t type, uint32_t a, uint32_t b)
{
    uint32_t head = __atomic_load_n(&s_queue_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&s_queue_tail, __ATOMIC_ACQUIRE);

    if ((head - tail) >= DEBUG_QUEUE_SIZE)
        return; // full: drop

    s_queue[head & DEBUG_QUEUE_MASK].type = type;
    s_queue[head & DEBUG_QUEUE_MASK].a = a;
    s_queue[head & DEBUG_QUEUE_MASK].b = b;
    __atomic_store_n(&s_queue_head, head + 1, __ATOMIC_RELEASE);
}

// Called from the game's thread, at the top of the VBlank handler and before
// native_ppu_render_frame(), so the flags it writes are settled before the PPU
// reads them and are never observed half-updated.
void native_debug_drain_commands(void)
{
    uint32_t tail = __atomic_load_n(&s_queue_tail, __ATOMIC_ACQUIRE);

    for (;;)
    {
        uint32_t head = __atomic_load_n(&s_queue_head, __ATOMIC_ACQUIRE);
        if (tail == head)
            return;

        struct debug_cmd_t cmd = s_queue[tail & DEBUG_QUEUE_MASK];
        // Release the slot before acting, so a full queue does not wedge
        // while this command runs.
        __atomic_store_n(&s_queue_tail, tail + 1, __ATOMIC_RELEASE);
        tail++;

        switch (cmd.type)
        {
        case DEBUG_CMD_SET_LAYER_VISIBLE:
            if (cmd.a < DEBUG_NUM_LAYERS)
                native_ppu_layer_visible[cmd.a] = (cmd.b != 0);
            break;
        case DEBUG_CMD_SET_WINDOW_VISIBLE:
            native_ppu_window_visible = (cmd.a != 0);
            break;
        case DEBUG_CMD_SET_BLEND_ENABLED:
            native_ppu_blend_enabled = (cmd.a != 0);
            break;
        case DEBUG_CMD_POPULATE_QUEUE:
            // Unused for now; kept so the op space is not silently narrowed.
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Toggle request
// ---------------------------------------------------------------------------

static volatile uint32_t s_toggle_requested;

// video.c's SDL event loop calls this when F1 or Ctrl+Shift+D goes down. The
// overlay reads it back through native_debug_menu_toggle_requested(), which
// runs on the present thread, so the hand-off between them is this one word.
void native_debug_menu_request_toggle(void)
{
    __atomic_store_n(&s_toggle_requested, 1, __ATOMIC_RELEASE);
}

bool native_debug_menu_toggle_requested(void)
{
    return __atomic_exchange_n(&s_toggle_requested, 0, __ATOMIC_ACQ_REL) != 0;
}

// ---------------------------------------------------------------------------
// Regions and the hex viewer
// ---------------------------------------------------------------------------

// The regions the port maps, in the order the memory panel lists them. These
// are the windows the mmap layer claims; the hex viewer resolves an address
// against this table, so an address outside every entry is refused rather
// than dereferenced.
static const struct
{
    const char *name;
    uint32_t base;
    uint32_t size;
    bool scannable;
} kRegions[] = {
    {"EWRAM", NATIVE_EWRAM_BASE, NATIVE_EWRAM_SIZE, true},
    {"IWRAM", NATIVE_IWRAM_BASE, NATIVE_IWRAM_SIZE, true},
    {"IO", NATIVE_IO_BASE, NATIVE_IO_SIZE, false},
    {"Palette", NATIVE_PLTT_BASE, NATIVE_PLTT_SIZE, true},
    {"VRAM", NATIVE_VRAM_BASE, NATIVE_VRAM_SIZE, true},
    {"OAM", NATIVE_OAM_BASE, NATIVE_OAM_SIZE, true},
    // The ROM window is the whole 16 MiB image, file-backed and patched in
    // place. Its "extent written" is not a slack figure and scanning it would
    // cost millions of iterations for an answer that means nothing, so it is
    // reported at full size instead.
    {"ROM", NATIVE_ROM_BASE, NATIVE_ROM_SIZE, false},
    {"Flash", NATIVE_FLASH_BASE, NATIVE_FLASH_SIZE, true},
};

#define NUM_REGIONS ((int)(sizeof(kRegions) / sizeof(kRegions[0])))

static struct debug_region s_regions[NUM_REGIONS];
static bool s_regions_scanned;

int native_debug_region_count(void)
{
    return NUM_REGIONS;
}

// Highest non-zero byte + 1, i.e. the extent written so far.
//
// This is the useful measure for mmap'd RAM. The alternative -- subtracting
// linker addresses -- does not work here: native.ld folds .ewram_data and
// .common_data into the host image at 0x40000000, so gHeap and gMain are host
// addresses and the emulated windows are nearly untouched by C globals. What
// the scan shows is what the *game* has actually written into each window,
// which for EWRAM and IWRAM is close to nothing by design and is exactly the
// slack the fixed-address contract depends on.
static uint32_t scan_used(uint32_t base, uint32_t size)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)base;
    uint32_t high = 0;

    // Scan backwards in 4-byte steps and then finish the last few bytes: the
    // top of a region is where live data sits, and a whole-region forward
    // scan every refresh would be wasteful on a 16 MiB ROM window.
    for (uint32_t off = size & ~3u; off > 0; off -= 4)
    {
        uint32_t w;
        memcpy(&w, (const void *)(p + off - 4), sizeof(w));
        if (w != 0)
        {
            high = off;
            break;
        }
    }

    if (high == 0)
        return 0;

    // Refine within the last non-zero dword.
    for (uint32_t i = high; i > high - 4 && i > 0; i--)
    {
        if (p[i - 1] != 0)
            return i;
    }

    return high;
}

static void ensure_regions_scanned(void)
{
    if (s_regions_scanned)
        return;

    for (int i = 0; i < NUM_REGIONS; i++)
    {
        s_regions[i].name = kRegions[i].name;
        s_regions[i].base = kRegions[i].base;
        s_regions[i].size = kRegions[i].size;
        // An unscannable region reports as fully used, which reads as "no
        // slack" rather than as a figure that was never measured.
        s_regions[i].used =
            kRegions[i].scannable ? scan_used(kRegions[i].base, kRegions[i].size)
                                 : kRegions[i].size;
    }

    s_regions_scanned = true;
}

const struct debug_region *native_debug_region(int index)
{
    ensure_regions_scanned();
    if (index < 0 || index >= NUM_REGIONS)
        return NULL;
    return &s_regions[index];
}

uint32_t native_debug_region_slack(int index)
{
    ensure_regions_scanned();
    if (index < 0 || index >= NUM_REGIONS)
        return 0;
    return s_regions[index].size - s_regions[index].used;
}

bool native_debug_read_hex(uint32_t addr, uint8_t *out)
{
    for (int i = 0; i < NUM_REGIONS; i++)
    {
        if (addr >= kRegions[i].base && addr - kRegions[i].base + 256 <= kRegions[i].size)
        {
            // 16 bytes at a time: the viewer always asks for a 16x16 block,
            // and doing it in one go keeps this a single bounds check.
            for (int row = 0; row < 16; row++)
                memcpy(out + row * 16, (const void *)(uintptr_t)(addr + row * 16), 16);
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Address-pinning slack
// ---------------------------------------------------------------------------
//
// The numbers the contract depends on are the ROM build's, not the native
// image's: ld_script.ld places ewram at 0x02000000 and iwram at 0x03000000
// with hard region limits, and the symbols in sym_ewram.txt / sym_common.txt
// fix the order inside them. The figures below are from `make compare`'s
// memory report (EWRAM 0x3cf64 of 0x40000, IWRAM 0x78ac of 0x8000) and are
// compiled in rather than parsed out of pokeemerald.map, which is not a build
// input of the native target.
//
// IWRAM's top 16 bytes are also reserved by hardware for SOUND_INFO_PTR,
// INTR_CHECK and INTR_VECTOR (include/gba/defines.h), so the usable slack is
// 0x754 - 0x10.
static const struct debug_slack kBuildSlack = {
    0x40000, 0x3CF64, 0x40000 - 0x3CF64,
    0x8000, 0x78AC, (0x8000 - 0x78AC) - 0x10,
};

const struct debug_slack *native_debug_build_slack(void)
{
    return &kBuildSlack;
}

// The heap is the one place where the live figure and the link-time figure
// describe the same bytes: gHeap is EWRAM_DATA, and although native.ld moves
// that section into the host image, the block chain is walked from the live
// header so the answer reflects the allocator rather than the layout.
//
// The block struct is restated here rather than reused: src/malloc.c's is
// private to that file, and the four fields the walk needs are stable (the
// header is 12 bytes and the chain is circular). A bad pointer would make this
// walk fault, so the traversal is bounded by the heap size and stops at the
// first block that is allocated or larger than the heap.
struct debug_mem_block
{
    u16 flag;
    u16 magic;
    u32 size;
    struct debug_mem_block *prev;
    struct debug_mem_block *next;
};

void native_debug_heap(uint32_t *total, uint32_t *used)
{
    *total = HEAP_SIZE;
    *used = 0;

    uintptr_t lo = (uintptr_t)gHeap;
    uintptr_t hi = lo + HEAP_SIZE;
    struct debug_mem_block *block = (struct debug_mem_block *)gHeap;

    // Walk the whole circular chain, summing the allocated blocks. The chain
    // covers the whole heap, so the traversal terminates on its own; the bounds
    // and iteration cap are there so a corrupt list cannot walk off into
    // unmapped memory, since this runs on the render thread and a fault there
    // would take the process down.
    for (uint32_t i = 0; i < HEAP_SIZE / sizeof(struct debug_mem_block); i++)
    {
        uintptr_t at = (uintptr_t)block;
        if (at < lo || at + sizeof(struct debug_mem_block) > hi)
            break;

        // flag is TRUE while the block is handed out, so this is the live
        // figure: bytes actually allocated right now.
        if (block->flag)
            *used += block->size + (uint32_t)sizeof(struct debug_mem_block);

        if (block->next == block)
            break;

        block = block->next;
    }

    if (*used > HEAP_SIZE)
        *used = HEAP_SIZE;
}

// ---------------------------------------------------------------------------
// gpu_regs shadow buffer and pending queue
// ---------------------------------------------------------------------------

const uint8_t *native_debug_gpu_shadow(void)
{
    return GetGpuRegShadowBuffer();
}

const uint8_t *native_debug_gpu_waiting(void)
{
    return GetGpuRegWaitingList();
}

uint32_t native_debug_gpu_pending_count(void)
{
    return CountPendingGpuRegWrites();
}

bool native_debug_gpu_locked(void)
{
    return IsGpuRegBufferLocked();
}

// ---------------------------------------------------------------------------
// DMA3
// ---------------------------------------------------------------------------

void native_debug_dma3(uint32_t *depth, uint32_t *capacity, uint32_t *bytes)
{
    *depth = GetDma3QueueDepth();
    *capacity = GetDma3QueueCapacity();
    *bytes = GetDma3PendingBytes();
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
//
// The panel shows the display registers straight out of the emulated IO page.
// Offsets come from include/gba/io_reg.h; the C half resolves them so the C++
// half never has to include a game header.
static struct debug_reg s_regs[DEBUG_MAX_REGS];
static bool s_regs_named;
static const uint32_t kRegOffsets[DEBUG_MAX_REGS] = {
    REG_OFFSET_DISPCNT,  REG_OFFSET_DISPSTAT, REG_OFFSET_BG0CNT,
    REG_OFFSET_BG1CNT,   REG_OFFSET_BG2CNT,   REG_OFFSET_BG3CNT,
    REG_OFFSET_BLDCNT,   REG_OFFSET_BLDALPHA, REG_OFFSET_BLDY,
    REG_OFFSET_WIN0H,    REG_OFFSET_WIN0V,    REG_OFFSET_WIN1H,
    REG_OFFSET_WIN1V,    REG_OFFSET_WININ,    REG_OFFSET_WINOUT,
    REG_OFFSET_KEYINPUT, REG_OFFSET_IE,       REG_OFFSET_IF,
    REG_OFFSET_BG0HOFS,  REG_OFFSET_BG0VOFS,
};

const struct debug_reg *native_debug_registers(int *count)
{
    if (!s_regs_named)
    {
        for (int i = 0; i < DEBUG_MAX_REGS; i++)
        {
            // Names must be literals: this table is read from the C++ half,
            // which has no game strings to reference.
            static const char *const kNames[DEBUG_MAX_REGS] = {
                "DISPCNT", "DISPSTAT", "BG0CNT", "BG1CNT", "BG2CNT", "BG3CNT",
                "BLDCNT", "BLDALPHA", "BLDY", "WIN0H", "WIN0V", "WIN1H",
                "WIN1V", "WININ", "WINOUT", "KEYINPUT", "IE", "IF",
                "BG0HOFS", "BG0VOFS",
            };
            s_regs[i].name = kNames[i];
        }
        s_regs_named = true;
    }

    for (int i = 0; i < DEBUG_MAX_REGS; i++)
        s_regs[i].value = native_debug_io_read(kRegOffsets[i]);

    *count = DEBUG_MAX_REGS;
    return s_regs;
}

uint16_t native_debug_io_read(uint32_t offset)
{
    return *(volatile uint16_t *)(NATIVE_IO + offset);
}

// ---------------------------------------------------------------------------
// BG bases
// ---------------------------------------------------------------------------
//
// Decoded the same way platform/native/ppu.c decodes them: BGCNT bits 2-3 are
// the char base (x0x4000) and bits 8-12 the screen base (x0x800).

static struct debug_bg s_bgs[4];

const struct debug_bg *native_debug_bg(int index)
{
    static const uint32_t kCntOffsets[4] = {
        REG_OFFSET_BG0CNT, REG_OFFSET_BG1CNT, REG_OFFSET_BG2CNT, REG_OFFSET_BG3CNT,
    };

    if (index < 0 || index > 3)
        return NULL;

    uint16_t cnt = native_debug_io_read(kCntOffsets[index]);
    s_bgs[index].cnt = cnt;
    s_bgs[index].charBase = NATIVE_VRAM_BASE + ((cnt >> 2) & 3) * 0x4000;
    s_bgs[index].screenBase = NATIVE_VRAM_BASE + ((cnt >> 8) & 0x1F) * 0x800;
    s_bgs[index].priority = cnt & 3;
    s_bgs[index].affine = (cnt & 0x0040) != 0;
    s_bgs[index].color256 = (cnt & 0x0080) != 0;

    return &s_bgs[index];
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
//
// gMPlayTable is the portable descriptor: {info, track, numTracks}. It is what
// m4a itself iterates (src/m4a.c:83), so walking it here visits exactly the
// players the mixer would.

extern const struct MusicPlayer gMPlayTable[];

// NUM_MUSIC_PLAYERS expands to (u16)gNumMusicPlayers, but gNumMusicPlayers is
// not an array here -- both ld_script_modern.ld and native.ld set it as an
// absolute symbol to the value 4, so the linker resolves the reference to the
// constant 4 rather than to storage. Read it as that constant explicitly
// instead of casting a pointer, which warns on a 64-bit host.
#define DEBUG_NUM_MUSIC_PLAYERS ((uint32_t)(uintptr_t)gNumMusicPlayers)

static const char *const kPlayerNames[DEBUG_MAX_PLAYERS] = {
    "BGM", "SE1", "SE2", "SE3", "?", "?", "?", "?",
};

static struct debug_player s_players[DEBUG_MAX_PLAYERS];
static int s_player_count;
static bool s_players_valid;

int native_debug_player_count(void)
{
    if (!s_players_valid)
    {
        s_player_count = 0;

        for (u32 i = 0; i < DEBUG_NUM_MUSIC_PLAYERS && s_player_count < DEBUG_MAX_PLAYERS; i++)
        {
            // uptr32, not a native pointer: on this host the table lives in
            // the ROM image and every entry is a 4-byte GBA address.
            const struct MusicPlayer *entry = &gMPlayTable[i];
            u32 infoAddr = entry->info;

            if (infoAddr == 0)
                continue;

            const volatile struct MusicPlayerInfo *info =
                (const volatile struct MusicPlayerInfo *)(uintptr_t)infoAddr;

            struct debug_player *p = &s_players[s_player_count];
            p->name = (i < DEBUG_MAX_PLAYERS) ? kPlayerNames[i] : "?";
            p->numTracks = entry->numTracks;
            p->priority = info->priority;
            p->status = info->status;
            // ident is the re-entrancy lock: SoundMain sets it to ID_NUMBER
            // when it starts and the mixer restores it, so a value other than
            // ID_NUMBER means a mix pass is in flight or was interrupted.
            p->ident = info->ident;
            p->tempoU = info->tempoU;
            p->tempoI = info->tempoI;
            p->tempoD = info->tempoD;
            p->tempoC = info->tempoC;
            p->inUse = (info->status & MUSICPLAYER_STATUS_TRACK) != 0;

            s_player_count++;
        }

        s_players_valid = true;
    }

    return s_player_count;
}

const struct debug_player *native_debug_player(int index)
{
    if (index < 0 || index >= native_debug_player_count())
        return NULL;
    return &s_players[index];
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

// Which logical region each flash sector belongs to, from include/save.h.
static const char *sector_region(u16 id)
{
    if (id == SECTOR_ID_SAVEBLOCK2)
        return "SaveBlock2";
    if (id >= SECTOR_ID_SAVEBLOCK1_START && id <= SECTOR_ID_SAVEBLOCK1_END)
        return "SaveBlock1";
    if (id >= SECTOR_ID_PKMN_STORAGE_START && id <= SECTOR_ID_PKMN_STORAGE_END)
        return "PokemonStorage";
    if (id == SECTOR_ID_HOF_1 || id == SECTOR_ID_HOF_2)
        return "HallOfFame";
    if (id == SECTOR_ID_TRAINER_HILL)
        return "TrainerHill";
    if (id == SECTOR_ID_RECORDED_BATTLE)
        return "RecordedBattle";
    return NULL;
}

static struct debug_sector s_sectors[DEBUG_MAX_SECTORS];
static bool s_sectors_valid;

int native_debug_sector_count(void)
{
    if (!s_sectors_valid)
    {
        for (int i = 0; i < DEBUG_MAX_SECTORS; i++)
        {
            // Read through the mapped flash window rather than through
            // struct SaveSector: the window is exactly SECTORS_COUNT sectors
            // of SECTOR_SIZE and this keeps the panel honest about what the
            // game actually wrote.
            const volatile u8 *base = NATIVE_FLASH + (size_t)i * SECTOR_SIZE;
            u16 id, checksum;
            u32 signature, counter;

            memcpy(&id, (const void *)(base + SECTOR_SIGNATURE_OFFSET), sizeof(id));
            memcpy(&checksum, (const void *)(base + SECTOR_SIGNATURE_OFFSET + 2), sizeof(checksum));
            memcpy(&signature, (const void *)(base + SECTOR_SIGNATURE_OFFSET + 4), sizeof(signature));
            memcpy(&counter, (const void *)(base + SECTOR_COUNTER_OFFSET), sizeof(counter));

            s_sectors[i].index = (uint32_t)i;
            s_sectors[i].id = id;
            s_sectors[i].checksum = checksum;
            s_sectors[i].signature = signature;
            s_sectors[i].counter = counter;
            s_sectors[i].valid = (signature == SECTOR_SIGNATURE);
            s_sectors[i].region = sector_region(id);
        }

        s_sectors_valid = true;
    }

    return SECTORS_COUNT;
}

const struct debug_sector *native_debug_sector(int index)
{
    native_debug_sector_count();
    if (index < 0 || index >= SECTORS_COUNT)
        return NULL;
    return &s_sectors[index];
}

const char *native_debug_save_path(void)
{
    return native_memory_save_path();
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

static struct debug_timing s_timing;

const struct debug_timing *native_debug_timing(void)
{
    s_timing.cycles = native_cycle_count();
    s_timing.scanline = native_vcount();
    s_timing.vcount = *(volatile u8 *)(NATIVE_IO + REG_OFFSET_VCOUNT);
    s_timing.vblankCounter1 = gMain.vblankCounter1;
    s_timing.vblankCounter2 = gMain.vblankCounter2;
    return &s_timing;
}

// ---------------------------------------------------------------------------
// Scanline effect
// ---------------------------------------------------------------------------

const uint16_t *native_debug_scanline_values(int buffer)
{
    if (buffer != 0 && buffer != 1)
        return NULL;
    return gScanlineEffectRegBuffers[buffer];
}

uint32_t native_debug_scanline_count(void)
{
    return 0x3C0;
}

uint32_t native_debug_scanline_dest(void)
{
    return (uint32_t)(uintptr_t)gScanlineEffect.dmaDest;
}

uint32_t native_debug_scanline_control(void)
{
    return gScanlineEffect.dmaControl;
}

int native_debug_scanline_active_buffer(void)
{
    return gScanlineEffect.srcBuffer;
}

// ---------------------------------------------------------------------------
// Sprites and tiles
// ---------------------------------------------------------------------------

void native_debug_sprites(uint32_t *oamLimit, uint32_t *reservedTiles, uint32_t *count)
{
    *oamLimit = gOamLimit;
    *reservedTiles = gReservedSpriteTileCount;
    *count = 0;

    // Index 64 is the sentinel CreateSprite hands back when the pool is full,
    // so only 0..63 are real.
    for (int i = 0; i < MAX_SPRITES; i++)
    {
        if (gSprites[i].inUse)
            (*count)++;
    }
}

const uint8_t *native_debug_oam(void)
{
    return NATIVE_OAM;
}

const uint8_t *native_debug_sprites_raw(void)
{
    return (const uint8_t *)gSprites;
}

uint64_t native_debug_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}