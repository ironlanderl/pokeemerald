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
#include "palette.h"
#include "native.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NATIVE_PROFILE
#include <time.h>
// NOT <stdlib.h>: global.h defines min/max/abs as macros, which collide with
// the declarations there and turn atexit() into syntax errors. Only that one
// function is needed, so declare it directly.
extern void atexit(void (*func)(void));

// --- Frame-stage profiler (compiled out unless -DNATIVE_PROFILE) -----------
//
// The VBlank path runs three very different things back to back: the game's
// own logic (everything between one VBlank returning and the next one being
// dispatched), the software rasterizer, and the GL present. Their relative
// cost is not visible from wall time alone, because SDL_GL_SwapWindow blocks
// on vsync and so absorbs whatever the other two did or failed to do.
//
// Times are nanoseconds from CLOCK_MONOTONIC, accumulated over the run.
static uint64_t prof_ppu_ns, prof_video_ns, prof_logic_ns, prof_input_ns;
static uint64_t prof_frames;

// VBlank counters: how many the clock thread raised, how many the main thread
// actually dispatched. raised > dispatched means the clock is running ahead and
// frames are being dropped.
static uint64_t prof_clock_vblanks, prof_dispatched_vblanks;

// Clock-thread lateness. kNsPerScanline is the budget; if the thread wakes more
// than a scanline period late the virtual clock is already slipping.
static uint64_t prof_clock_late_ns;   // summed overshoot past the deadline
static uint64_t prof_clock_late_cnt;  // iterations that overshot at all
static uint64_t prof_clock_max_late_ns;

// Per-frame histogram: bucket index = microseconds. 65536 covers up to 65ms,
// which a 60Hz budget of 16.67ms fits inside comfortably.
#define PROF_HIST_BUCKETS 65536
static uint64_t prof_hist_ppu[PROF_HIST_BUCKETS];
static uint64_t prof_hist_video[PROF_HIST_BUCKETS];
static uint64_t prof_hist_total[PROF_HIST_BUCKETS];

static inline uint64_t prof_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void prof_hist_add(uint64_t *hist, uint64_t ns)
{
    uint64_t us = ns / 1000;
    if (us >= PROF_HIST_BUCKETS)
        us = PROF_HIST_BUCKETS - 1;
    hist[us]++;
}

static int prof_hist_percentile(const uint64_t *hist, uint64_t total, double pct)
{
    if (!total)
        return 0;
    uint64_t want = (uint64_t)(total * pct + 0.5);
    uint64_t acc = 0;
    for (int i = 0; i < PROF_HIST_BUCKETS; i++)
    {
        acc += hist[i];
        if (acc >= want)
            return i;
    }
    return PROF_HIST_BUCKETS - 1;
}

// Point at which the previous VBlank finished its work; the gap from there to
// the next VBlank's render start is the game's own logic plus its idle spin.
static uint64_t prof_last_frame_end_ns;

static void prof_report(void)
{
    if (!prof_frames)
        return;

    fprintf(stderr, "\n=== native frame profile ===\n");
    fprintf(stderr, "frames dispatched      : %llu\n", (unsigned long long)prof_frames);
    fprintf(stderr, "clock-thread VBlanks   : %llu\n", (unsigned long long)prof_clock_vblanks);
    fprintf(stderr, "dispatched VBlanks     : %llu\n", (unsigned long long)prof_dispatched_vblanks);
    if (prof_clock_vblanks)
        fprintf(stderr, "dispatched / raised    : %.3f  (<1 means the clock runs ahead)\n",
                (double)prof_dispatched_vblanks / (double)prof_clock_vblanks);
    fprintf(stderr, "clock late events      : %llu, total %llu ms, max %.3f ms\n",
            (unsigned long long)prof_clock_late_cnt,
            (unsigned long long)(prof_clock_late_ns / 1000000),
            (double)prof_clock_max_late_ns / 1e6);

    const double n = (double)prof_frames;
    fprintf(stderr, "\nper frame (us): mean / p50 / p99 / max\n");
    fprintf(stderr, "  game logic : %8.1f / %6d / %6d\n",
            (double)prof_logic_ns / 1000.0 / n, prof_hist_percentile(prof_hist_total, prof_frames, 0.50),
            prof_hist_percentile(prof_hist_total, prof_frames, 0.99));
    fprintf(stderr, "  ppu raster : %8.1f / %6d / %6d\n",
            (double)prof_ppu_ns / 1000.0 / n, prof_hist_percentile(prof_hist_ppu, prof_frames, 0.50),
            prof_hist_percentile(prof_hist_ppu, prof_frames, 0.99));
    fprintf(stderr, "  gl present : %8.1f / %6d / %6d\n",
            (double)prof_video_ns / 1000.0 / n, prof_hist_percentile(prof_hist_video, prof_frames, 0.50),
            prof_hist_percentile(prof_hist_video, prof_frames, 0.99));
    fprintf(stderr, "  input poll : %8.1f\n", (double)prof_input_ns / 1000.0 / n);

    // Where the total frame time goes: the >16.67ms buckets are the frames that
    // cannot fit in a 60Hz budget and therefore cost a whole extra display frame.
    // The histogram is indexed by microseconds, so index 16667 is already 16.67ms.
    uint64_t over = 0;
    for (int i = 16667; i < PROF_HIST_BUCKETS; i++)
        over += prof_hist_total[i];

    // PPU sub-stage breakdown (see ppu.c). Zero unless the PPU was built with
    // -DNATIVE_PROFILE too, which the Makefile does for PROFILE=1.
    extern uint64_t prof_ppu_obj_calls;
    fprintf(stderr, "\nppu obj pass  : %llu calls/frame\n",
            (unsigned long long)(prof_ppu_obj_calls / prof_frames));
    fprintf(stderr, "\nframes over 16.67ms    : %llu / %llu (%.1f%%)\n",
            (unsigned long long)over, (unsigned long long)prof_frames,
            100.0 * (double)over / n);
    fprintf(stderr, "  => implied FPS if each miss costs one extra vsync: %.1f\n",
            n / (n + over));
    fprintf(stderr, "=== end profile ===\n\n");
}
#endif // NATIVE_PROFILE

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
#ifdef NATIVE_PROFILE
    // Seed the "game logic" window from process start, so the first frame's
    // logic bucket includes the whole boot rather than reading as zero.
    prof_last_frame_end_ns = prof_now_ns();
    atexit(prof_report);
#endif
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

static void native_timing_apply_press(void);

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

    // IF in the IO page is written both by the clock thread (raising) and by
    // this dispatcher (acknowledging), and s_if is the authoritative copy.
    // Merge anything the page has that s_if does not, then dispatch.
    uint16_t page_if = io[REG_OFFSET_IF / 2];
    if_raise(page_if & (uint16_t)~__atomic_load_n(&s_if, __ATOMIC_SEQ_CST));

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
            void native_timing_note_frame(void);
            volatile uint16_t *io = (volatile uint16_t *)NATIVE_IO;

            // Run the rest of VBlank with VCOUNT parked at the first VBlank line.
            //
            // ProcessDma3Requests (called from VBlankIntr) services the deferred
            // VRAM and palette copies, and gives up once VCOUNT passes 224. The
            // clock thread advances VCOUNT independently, so leaving it wherever
            // it happened to be made those loads drop out on some frames and the
            // picture came out partially drawn and flickering. VBlank runs from
            // line 160, so hold VCOUNT at 160 for the duration.
            uint16_t saved = io[REG_OFFSET_VCOUNT / 2];
            io[REG_OFFSET_VCOUNT / 2] = NATIVE_VBLANK_START;

            // Poll input before rendering so a key pressed during this VBlank is
            // visible to the game's main loop, which runs after WaitForVBlank
            // returns and reads REG_KEYINPUT before drawing the next frame.
            // Run DMA the game armed (palette and VRAM copies use DMA_START_NOW),
            // then poll input so a key pressed this VBlank is visible to the
            // main loop.
            void native_dma_service_now(void);
            native_dma_service_now();
            native_timing_apply_press();
#ifdef NATIVE_PROFILE
            uint64_t t0 = prof_now_ns();
#endif
            if (!native_input_poll())
                native_request_shutdown();
#ifdef NATIVE_PROFILE
            uint64_t t1 = prof_now_ns();
            prof_input_ns += t1 - t0;
            native_ppu_render_frame();
            uint64_t t2 = prof_now_ns();
            prof_ppu_ns += t2 - t1;
            prof_hist_add(prof_hist_ppu, t2 - t1);
            native_video_render();
            uint64_t t3 = prof_now_ns();
            prof_video_ns += t3 - t2;
            prof_hist_add(prof_hist_video, t3 - t2);
            native_timing_note_frame();
            {
                uint64_t t4 = prof_now_ns();
                // Game logic = the gap between finishing the previous VBlank and
                // starting this one. Includes WaitForVBlank's spin, which on its
                // own is "the clock has not reached VBlank yet".
                prof_logic_ns += t0 - prof_last_frame_end_ns;
                prof_hist_add(prof_hist_total, t4 - prof_last_frame_end_ns);
                prof_last_frame_end_ns = t4;
                prof_frames++;
                prof_dispatched_vblanks++;
            }
#else
            native_ppu_render_frame();
            native_video_render();
            native_timing_note_frame();
#endif

            io[REG_OFFSET_VCOUNT / 2] = saved;
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
    // The clock thread raises INTR_FLAG_VBLANK independently, so waiting on the
    // flag and then dispatching is racy: the flag can be observed before the
    // pending bit is visible to run_interrupt, or run_interrupt can be entered
    // between the check and the dispatch. Instead dispatch repeatedly until the
    // game's own intrCheck flag is set -- that is set only by VBlankIntr, so it
    // is the real completion signal.
    while (!(gMain.intrCheck & INTR_FLAG_VBLANK))
    {
        native_timing_run_interrupt();
    }
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
    // One scanline of GBA time. The CPU runs at 16.777216 MHz and a frame is
    // 280896 cycles over 228 lines, which gives 59.7275 Hz -- the real LCD
    // refresh. The previous expression divided by 1000 rather than multiplying
    // by 1e9, so it was ~5400x too small and the clock raced.
    const double kNsPerScanline = 1e9 * (double)NATIVE_CYCLES_PER_SCANLINE / 16777216.0;

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
        {
            if_raise(INTR_FLAG_VBLANK);
#ifdef NATIVE_PROFILE
            prof_clock_vblanks++;
#endif
        }
        *(volatile uint16_t *)(NATIVE_IO + REG_OFFSET_IF) = __atomic_load_n(&s_if, __ATOMIC_SEQ_CST);


        long ns = (long)kNsPerScanline;
        next.tv_nsec += ns;
        while (next.tv_nsec >= 1000000000L)
        {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
#ifdef NATIVE_PROFILE
        // How far past its own deadline did this iteration wake? Absolute
        // deadlines mean lateness accumulates into every later one, so a
        // sustained positive value is the clock losing real time.
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t late = ((int64_t)now.tv_sec - (int64_t)next.tv_sec) * 1000000000ll
                         + ((int64_t)now.tv_nsec - (int64_t)next.tv_nsec);
            if (late > 0)
            {
                prof_clock_late_ns += (uint64_t)late;
                prof_clock_late_cnt++;
                if ((uint64_t)late > prof_clock_max_late_ns)
                    prof_clock_max_late_ns = (uint64_t)late;
            }
        }
#endif
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

// Simulate a key press at a given frame so headless runs can get past screens
// that wait for input -- the intro exits on any button. This also exercises the
// input path end to end without needing a real key event.
static int s_press_at = -1;
static uint16_t s_press_bits;
static int s_press_clear;

void native_input_schedule(int frame, uint16_t bits)
{
    s_press_at = frame;
    s_press_bits = bits;
}

// Called once per frame from the VBlank path, before input is published.
static void native_timing_apply_press(void)
{
    extern uint16_t native_input_set_bits(uint16_t bits);

    // Hold the injected bits for a few frames so ReadKeys definitely samples
    // them, then release so the next press produces a fresh rising edge.
    if (s_press_clear > 0)
    {
        if (--s_press_clear == 0)
            native_input_set_bits(0);
        return;
    }

    if (s_press_at < 0 || s_frames < s_press_at)
        return;

    native_input_set_bits(s_press_bits);
    s_press_clear = 4; // hold for 4 frames
    s_press_at = -1;
}

// Called after each frame is rendered. Writes the capture and asks the game to
// unwind once the limit is reached.
void native_timing_note_frame(void)
{
    if (s_frame_limit <= 0 && s_press_at < 0)
        return;

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
