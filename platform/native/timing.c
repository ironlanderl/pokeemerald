// platform/native/timing.c
//
// Virtual frame clock and interrupt dispatch.
//
// The game is written around a hardware that raises interrupts at precise
// moments: WaitForVBlank spins on a flag the VBlank handler sets,
// ProcessDma3Requests reads VCOUNT and gives up once it passes 224, and
// m4aSoundInit busy-waits for VCOUNT to become 159. A separate "hardware"
// thread would race all of that, so the whole console is driven from one
// thread: this file advances a virtual scanline counter and calls interrupt
// handlers directly at the right virtual cycle.

#include "global.h"
#include "main.h"
#include "native.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// The scanline, published by the clock thread and read by the game (m4aSoundInit
// busy-waits for 159, ProcessDma3Requests compares against 224). Declared
// volatile so the compiler reloads it rather than caching it in a register.
static volatile uint8_t s_vcount;
static uint64_t s_cycles;
static bool s_enabled;
static bool s_shutdown_requested;

// Cached interrupt state, mirroring REG_IE / REG_IF / REG_IME.
//
// s_if is raised by the clock thread and acknowledged by the game thread, so
// every read-modify-write has to be atomic or an interrupt can be lost (or a
// stale bit replayed). The IE/IME pair are only written by the game and only
// read by it, so they need no synchronisation.
static volatile uint16_t s_ie;
static volatile uint16_t s_if;
static volatile uint16_t s_ime;

// Atomic set/clear of the pending-interrupt flags: the clock thread raises
// them and the game thread acknowledges them.
static inline void if_raise(uint16_t bits)
{
    __atomic_fetch_or(&s_if, bits, __ATOMIC_SEQ_CST);
}

static inline void if_clear(uint16_t bits)
{
    __atomic_fetch_and(&s_if, (uint16_t)~bits, __ATOMIC_SEQ_CST);
}

void native_timing_init(void)
{
    s_vcount = 0;
    s_cycles = 0;
    s_enabled = true;
    s_ie = 0;
    s_if = 0;
    s_ime = 0;
}

void native_timing_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

bool native_timing_enabled(void)
{
    return s_enabled;
}

// Asked for by the input layer when the window is closed. AgbMain runs an
// infinite loop, so this unwinds it from inside WaitForVBlank instead.
void native_request_shutdown(void)
{
    s_shutdown_requested = true;
}

bool native_shutdown_requested(void)
{
    return s_shutdown_requested;
}

uint8_t native_vcount(void)
{
    return s_vcount;
}

uint64_t native_cycle_count(void)
{
    return s_cycles;
}

// REG_VCOUNT is a byte read through the IO page; reflect the virtual scanline
// there so game code reading it directly sees the right value.
static void PublishVcount(void)
{
    volatile uint8_t *io = (volatile uint8_t *)NATIVE_IO;
    io[0x06] = s_vcount; // REG_OFFSET_VCOUNT (not 0x04, which is DISPSTAT)
}

// Interrupt bits, matching constants/gba_constants.inc.
#define INTR_FLAG_VBLANK 0x0001
#define INTR_FLAG_HBLANK 0x0002
#define INTR_FLAG_VCOUNT 0x0004
#define INTR_FLAG_SERIAL 0x0080
#define INTR_FLAG_TIMER0 0x0008
#define INTR_FLAG_TIMER1 0x0010
#define INTR_FLAG_TIMER2 0x0020
#define INTR_FLAG_TIMER3 0x0040
#define INTR_FLAG_DMA0 0x0100
#define INTR_FLAG_DMA1 0x0200
#define INTR_FLAG_DMA2 0x0400
#define INTR_FLAG_DMA3 0x0800
#define INTR_FLAG_KEYPAD 0x1000
#define INTR_FLAG_GAMEPAK 0x2000

// gIntrTable from src/main.c, indexed by the slot order IntrMain used.
extern IntrFunc gIntrTable[];
extern int gIntrCount;

static const uint16_t sIntrPriority[] = {
    INTR_FLAG_VCOUNT, INTR_FLAG_SERIAL,  INTR_FLAG_TIMER3, INTR_FLAG_HBLANK,
    INTR_FLAG_VBLANK, INTR_FLAG_TIMER0,  INTR_FLAG_TIMER1, INTR_FLAG_TIMER2,
    INTR_FLAG_DMA0,   INTR_FLAG_DMA1,    INTR_FLAG_DMA2,   INTR_FLAG_DMA3,
    INTR_FLAG_KEYPAD, INTR_FLAG_GAMEPAK,
};
#define NUM_SLOTS (sizeof(sIntrPriority) / sizeof(sIntrPriority[0]))

// Dispatch the highest-priority pending interrupt, mirroring src/crt0.s.
// Returns true if a handler ran.
bool native_timing_run_interrupt(void)
{
    // The game writes REG_IE / REG_IME / REG_IF directly in the emulated IO
    // page rather than through this module, so read the authoritative values
    // back out of memory before deciding what is pending.
    volatile uint16_t *io = (volatile uint16_t *)NATIVE_IO;
    s_ie = io[REG_OFFSET_IE / 2];
    s_ime = io[REG_OFFSET_IME / 2];
    if_clear(io[REG_OFFSET_IF / 2]);

    uint16_t pending = (uint16_t)(s_ie & __atomic_load_n(&s_if, __ATOMIC_SEQ_CST) & 0x3FFF);
    if (!s_ime || pending == 0)
        return false;

    for (unsigned i = 0; i < NUM_SLOTS; i++)
    {
        uint16_t flag = sIntrPriority[i];
        if (!(pending & flag))
            continue;

        if_clear(flag); // acknowledge

        IntrFunc fn = gIntrTable[i];
        if (fn)
            fn();

        // Acknowledge in the IO page too, so the game's own reads agree.
        *(volatile uint16_t *)(NATIVE_IO + REG_OFFSET_IF) = __atomic_load_n(&s_if, __ATOMIC_SEQ_CST);

        // VBlank is where the real hardware scans out the frame, and where
        // AgbMain's WaitForVBlank unblocks. Render and present here so the
        // image is produced at exactly the moment the game expects it, and so
        // the window/input stay responsive without a second thread.
        if (flag == INTR_FLAG_VBLANK && s_enabled)
        {
            void native_ppu_render_frame(void);
            void native_video_render(void);
            bool native_input_poll(void);
            native_ppu_render_frame();
            native_video_render();
            {
                void native_timing_note_frame(void);
                native_timing_note_frame();
            }
            if (!native_input_poll())
                native_request_shutdown();
        }
        return true;
    }
    return false;
}

// Called by the frame driver once per scanline. Raises the interrupt sources
// that occur at this line, then runs any handler that is now pending.
static void TickScanline(uint8_t line)
{
    PublishVcount();

    // VCOUNT interrupt: the game programs DISPSTAT for line 150 at boot.
    if (line == 150)
        if_raise(INTR_FLAG_VCOUNT);

    // VBlank starts at line 160.
    if (line == NATIVE_VBLANK_START)
        if_raise(INTR_FLAG_VBLANK);

    // HBlank fires once per scanline while the game asks for it.
    if_raise(INTR_FLAG_HBLANK);

    native_timing_run_interrupt();
}

// Wait for the next VBlank. The clock thread owns VCOUNT and raises IF; all
// this does is run the game's handler when the flag arrives.
void native_timing_advance_to_next_vblank(void)
{
    while (!(__atomic_load_n(&s_if, __ATOMIC_SEQ_CST) & INTR_FLAG_VBLANK))
        native_timing_run_interrupt();
    native_timing_run_interrupt(); // dispatch the VBlank handler itself
}

// --- Interrupt enable/acknowledge, used by src/gpu_regs.c and src/m4a.c ---

void native_timing_set_ie(uint16_t v)
{
    s_ie = v;
}

uint16_t native_timing_get_ie(void)
{
    return s_ie;
}

void native_timing_set_ime(uint16_t v)
{
    s_ime = v;
}

uint16_t native_timing_get_ime(void)
{
    return s_ime;
}

void native_timing_raise(uint16_t flags)
{
    if_raise(flags);
}

uint16_t native_timing_get_if(void)
{
    return __atomic_load_n(&s_if, __ATOMIC_SEQ_CST);
}

void native_timing_ack(uint16_t flags)
{
    if_clear(flags);
}
// ---------------------------------------------------------------------------
// Frame clock
//
// VCOUNT has to advance independently of the game loop. m4aSoundInit busy-waits
// for scanline 159 while still inside AgbMain's setup code, before the main
// loop ever calls WaitForVBlank, so a clock that only advanced during interrupt
// dispatch would deadlock there.
//
// A dedicated thread owns nothing but VCOUNT: it publishes the scanline to the
// emulated IO page and nothing else. The game reads VCOUNT, so that is the only
// shared state. Everything else (dispatching handlers, rendering) still happens
// on the game's own thread, which keeps the spin-waits race-free: VBlankIntr is
// called from WaitForVBlank, not from the clock thread.

#include <pthread.h>
#include <time.h>

static pthread_t s_clock_thread;
static bool s_clock_running;
static volatile bool s_clock_stop;

static void *clock_main(void *arg)
{
    (void)arg;
    // A GBA frame is 280896 cycles at 16.78 MHz; spread 228 scanlines evenly.
    const double kNsPerScanline = 16777216.0 / NATIVE_CYCLES_PER_SCANLINE / 1000.0;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!s_clock_stop)
    {
        uint8_t line = s_vcount;
        PublishVcount();

        s_vcount++;
        if (s_vcount >= NATIVE_TOTAL_SCANLINES)
            s_vcount = 0;
        s_cycles += NATIVE_CYCLES_PER_SCANLINE;

        // Raise the interrupt sources for this scanline and publish IF, which
        // is where the game reads it (native_timing_run_interrupt reads it back
        // out of the IO page).
        if_raise(INTR_FLAG_HBLANK);
        if (line == 150)
            if_raise(INTR_FLAG_VCOUNT);
        if (line == NATIVE_VBLANK_START)
            if_raise(INTR_FLAG_VBLANK);
        *(volatile uint16_t *)(NATIVE_IO + REG_OFFSET_IF) = __atomic_load_n(&s_if, __ATOMIC_SEQ_CST);

        long ns = (long)kNsPerScanline;
        next.tv_nsec += ns;
        while (next.tv_nsec >= 1000000000L)
        {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

void native_timing_start_clock(void)
{
    if (s_clock_running)
        return;
    s_clock_stop = false;
    if (pthread_create(&s_clock_thread, NULL, clock_main, NULL) == 0)
        s_clock_running = true;
    else
        native_log("warning: could not start frame clock; VCOUNT will not advance");
}

void native_timing_stop_clock(void)
{
    if (!s_clock_running)
        return;
    s_clock_stop = true;
    pthread_join(s_clock_thread, NULL);
    s_clock_running = false;
}

// --- Headless capture ------------------------------------------------------
//
// For verification without a window: run a fixed number of frames, then write
// the framebuffer out as a PPM and exit. Used to check the PPU against a
// reference capture without needing an X server.

static int s_frame_limit;
static const char *s_shot_path;
static int s_frames;

void native_set_headless(int frames, const char *shotPath)
{
    s_frame_limit = frames;
    s_shot_path = shotPath;
}

// Called after each frame is rendered. Writes the capture and asks the game to
// unwind once the limit is reached.
void native_timing_note_frame(void)
{
    if (s_frame_limit <= 0)
        return;

    if (++s_frames < s_frame_limit)
        return;

    if (s_shot_path)
    {
        extern const uint32_t *native_ppu_framebuffer(void);
        FILE *f = fopen(s_shot_path, "wb");
        if (f)
        {
            fprintf(f, "P6\n240 160\n255\n");
            const uint32_t *fb = native_ppu_framebuffer();
            for (int i = 0; i < 240 * 160; i++)
            {
                uint32_t c = fb[i];
                fputc((c >> 0) & 0xFF, f);
                fputc((c >> 8) & 0xFF, f);
                fputc((c >> 16) & 0xFF, f);
            }
            fclose(f);
            fprintf(stderr, "wrote %s after %d frames\n", s_shot_path, s_frames);
        }
    }

    native_request_shutdown();
}
