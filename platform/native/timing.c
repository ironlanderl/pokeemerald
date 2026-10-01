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

// Set by the interrupt dispatcher and consumed by WaitForVBlank.
static uint8_t s_vcount;
static uint64_t s_cycles;
static bool s_enabled;

// Cached interrupt state, mirroring REG_IE / REG_IF / REG_IME.
static volatile uint16_t s_ie;
static volatile uint16_t s_if;
static volatile uint16_t s_ime;

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
    io[0x04] = s_vcount; // REG_OFFSET_VCOUNT
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
    uint16_t pending = (uint16_t)(s_ie & s_if & 0x3FFF);
    if (!s_ime || pending == 0)
        return false;

    for (unsigned i = 0; i < NUM_SLOTS; i++)
    {
        uint16_t flag = sIntrPriority[i];
        if (!(pending & flag))
            continue;

        s_if &= (uint16_t)~flag; // acknowledge

        IntrFunc fn = gIntrTable[i];
        if (fn)
            fn();
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
        s_if |= INTR_FLAG_VCOUNT;

    // VBlank starts at line 160.
    if (line == NATIVE_VBLANK_START)
        s_if |= INTR_FLAG_VBLANK;

    // HBlank fires once per scanline while the game asks for it.
    s_if |= INTR_FLAG_HBLANK;

    native_timing_run_interrupt();
}

// Advance exactly one scanline of virtual time.
void native_timing_step_scanline(void)
{
    TickScanline(s_vcount);
    s_vcount++;
    if (s_vcount >= NATIVE_TOTAL_SCANLINES)
        s_vcount = 0;
    s_cycles += NATIVE_CYCLES_PER_SCANLINE;
}

// Run until the start of the next VBlank. Used by WaitForVBlank.
void native_timing_advance_to_next_vblank(void)
{
    do
    {
        native_timing_step_scanline();
    } while (s_vcount != NATIVE_VBLANK_START);
}

// Run one whole frame (228 scanlines).
void native_timing_run_frame(void)
{
    for (int i = 0; i < NATIVE_TOTAL_SCANLINES; i++)
        native_timing_step_scanline();
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
    s_if |= flags;
}

uint16_t native_timing_get_if(void)
{
    return s_if;
}

void native_timing_ack(uint16_t flags)
{
    s_if &= (uint16_t)~flags;
}