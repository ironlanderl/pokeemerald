// platform/native/dma.c
//
// Host implementation of the GBA's DMA controllers.
//
// The game arms DMA by writing raw registers -- src/dma3_manager.c,
// src/palette.c and src/scanline_effect.c all go through the DmaSet() macro in
// include/gba/macro.h, which pokes REG_ADDR_DMA0CNT and friends. On hardware
// the transfer happens immediately (for DMA_START_NOW) or once per HBlank/VBlank
// (for the repeating modes).
//
// The IO page is ordinary mapped memory here, so a write to the control
// register is just a store and the transfer silently never happens. This file
// services the controllers at the points where the hardware would fire them:
// immediately after any code has armed them, and once per VBlank/HBlank for the
// repeating modes.

#include "global.h"
#include "native.h"

#include <string.h>

// DMA control word, as written by the game.
//
// DmaSetUnchecked (include/gba/macro.h) stores a full 32-bit word: the flag set
// from include/gba/io_reg.h shifted into the upper half, and the transfer count
// in bits 0-15. These constants therefore carry the <<16, unlike the REG_DMAxCNT_H
// spellings they come from -- testing the unshifted masks against the live word
// reads the count instead of the flags and rejects every channel.
#define DMA_DEST_INC 0x00000000u
#define DMA_DEST_DEC 0x00200000u
#define DMA_DEST_FIXED 0x00400000u
#define DMA_DEST_RELOAD 0x00600000u
#define DMA_SRC_INC 0x00000000u
#define DMA_SRC_DEC 0x00800000u
#define DMA_SRC_FIXED 0x01000000u
#define DMA_REPEAT 0x02000000u
#define DMA_16BIT 0x00000000u
#define DMA_32BIT 0x04000000u
#define DMA_START_NOW 0x00000000u
#define DMA_START_VBLANK 0x10000000u
#define DMA_START_HBLANK 0x20000000u
#define DMA_ENABLE 0x80000000u

// Mask covering the dest-address mode bits, so the three control cases can be
// told apart with a single compare.
#define DMA_DEST_MODE_MASK 0x00600000u

// Offset of DMA0's registers within the IO page (REG_OFFSET_DMA0SAD in
// include/gba/io_reg.h). This must be a page-relative offset because the accessors
// already add NATIVE_IO: with the full address 0x040000B0 here, the sum landed at
// 0x080000B0 -- inside the mapped ROM -- so every DMA control read came from the
// wrong place and no transfer ever ran.
#define DMA_BASE 0x000000B0u
#define DMA_STRIDE 0x00Cu

// Fill transfers carry the value itself, not a pointer to a stack temporary.
//
// DmaFill and CpuFill both route the value through a stack local's address. On
// the GBA that lives in IWRAM and survives the trip through a 32-bit register;
// natively the stack sits near 0x7fff_ffff_ffff, so the address is truncated and
// the loader dereferences an unmapped page. Recording the truncated register
// value against the real address works only while the temporary is still alive,
// which is not guaranteed, so the value is passed through directly instead.
//
// Each armed fill channel records its value here; the control word's count and
// width still come from the channel registers, so the transfer behaves normally.
#define MAX_FILL_SOURCES 8
static uint32_t s_fill_value[MAX_FILL_SOURCES];
static int s_fill_count;

// Called with the channel that was just armed and the value it should write.
void DmaFillValue(int dmaNum, uint32_t value, void *dest, u32 control);
void DmaFillValue(int dmaNum, uint32_t value, void *dest, u32 control)
{
    if (s_fill_count < MAX_FILL_SOURCES)
    {
        s_fill_value[s_fill_count] = value;
        s_fill_count++;
    }

    volatile uint32_t *r = (volatile uint32_t *)(NATIVE_IO + DMA_BASE + dmaNum * DMA_STRIDE);
    // SAD is left at 0 for a fill: it is unused, and a real address here would
    // only invite the loader to dereference it.
    r[0] = 0;
    r[1] = (uint32_t)(uintptr_t)dest;
    r[2] = control;
}

// DMA control word, as written by the game.
//
// DmaSetUnchecked (include/gba/macro.h) stores a full 32-bit word: the flag set
// from include/gba/io_reg.h shifted into the upper half, and the transfer count
// in bits 0-15. These constants therefore carry the <<16, unlike the REG_DMAxCNT_H
// spellings they come from -- testing the unshifted masks against the live word
// reads the count instead of the flags and rejects every channel.
#define DMA_DEST_INC 0x00000000u
#define DMA_DEST_DEC 0x00200000u
#define DMA_DEST_FIXED 0x00400000u
#define DMA_DEST_RELOAD 0x00600000u
#define DMA_SRC_INC 0x00000000u
#define DMA_SRC_DEC 0x00800000u
#define DMA_SRC_FIXED 0x01000000u
#define DMA_REPEAT 0x02000000u
#define DMA_16BIT 0x00000000u
#define DMA_32BIT 0x04000000u
#define DMA_START_NOW 0x00000000u
#define DMA_START_VBLANK 0x10000000u
#define DMA_START_HBLANK 0x20000000u
#define DMA_ENABLE 0x80000000u

// Mask covering the dest-address mode bits, so the three control cases can be
// told apart with a single compare.
#define DMA_DEST_MODE_MASK 0x00600000u

// Offset of DMA0's registers within the IO page (REG_OFFSET_DMA0SAD in
// include/gba/io_reg.h). This must be a page-relative offset because the accessors
// already add NATIVE_IO: with the full address 0x040000B0 here, the sum landed at
// 0x080000B0 -- inside the mapped ROM -- so every DMA control read came from the
// wrong place and no transfer ever ran.
#define DMA_BASE 0x000000B0u
#define DMA_STRIDE 0x00Cu

// Fill transfers record the *live* address of the caller's stack local.
//
// DmaFill's value lives in a stack temporary; on the host that address is near
// 0x7fff_ffff_ffff and does not survive truncation into the 32-bit SAD register.
// The loader therefore resolves the truncated pointer back to the real one by
// matching it against the live stack of the calling thread.
#define MAX_FILL_SOURCES 8
struct fill_source
{
    uint32_t truncated; // the value the game wrote into SAD
    const void *real;   // the address that actually holds the fill value
};
static struct fill_source s_fill_sources[MAX_FILL_SOURCES];
static int s_fill_count;

void native_dma_register_fill(uint32_t truncated, const void *real)
{
    for (int i = 0; i < s_fill_count; i++)
    {
        if (s_fill_sources[i].truncated == truncated)
        {
            s_fill_sources[i].real = real;
            return;
        }
    }
    if (s_fill_count < MAX_FILL_SOURCES)
    {
        s_fill_sources[s_fill_count].truncated = truncated;
        s_fill_sources[s_fill_count].real = real;
        s_fill_count++;
    }
}

// Record the fill source at the point the game writes the registers, then fall
// through to the normal DmaSet so the channel state stays identical.
void DmaSetFill(int dmaNum, const void *valuePtr, void *dest, u32 control);
void DmaSetFill(int dmaNum, const void *valuePtr, void *dest, u32 control)
{
    native_dma_register_fill((uint32_t)(uintptr_t)valuePtr, valuePtr);
    // Write the channel registers directly; DmaSetUnchecked pastes its argument
    // into REG_ADDR_DMA##dmaNum, which does not work through a function parameter.
    volatile uint32_t *r = (volatile uint32_t *)(NATIVE_IO + DMA_BASE + dmaNum * DMA_STRIDE);
    r[0] = (uint32_t)(uintptr_t)valuePtr;
    r[1] = (uint32_t)(uintptr_t)dest;
    r[2] = control;
}


// Read the four 32-bit registers of DMA channel n.
static void dma_read(int n, uint32_t *sad, uint32_t *dad, uint32_t *cnt)
{
    volatile uint32_t *r = (volatile uint32_t *)(NATIVE_IO + DMA_BASE + n * DMA_STRIDE);
    *sad = r[0];
    *dad = r[1];
    *cnt = r[2];
}

static void dma_write(int n, uint32_t sad, uint32_t dad, uint32_t cnt)
{
    volatile uint32_t *r = (volatile uint32_t *)(NATIVE_IO + DMA_BASE + n * DMA_STRIDE);
    r[0] = sad;
    r[1] = dad;
    r[2] = cnt;
}

// Perform one transfer and update the source/dest pointers the way the hardware
// does, so a repeating channel advances.
static void dma_run_once(int n, uint32_t *unused)
{
    (void)unused;
    uint32_t sad, dad, ctl;
    dma_read(n, &sad, &dad, &ctl);

    if (!(ctl & DMA_ENABLE))
        return;

    uint32_t mode = ctl & 0xFFFF0000u;
    uint32_t count = ctl & 0x0000FFFFu;
    bool word = (mode & DMA_32BIT) != 0;
    uint32_t step = word ? 4u : 2u;

    // A zero count means 0x10000 transfers.
    uint32_t units = count ? count : 0x10000u;

    // Only immediate transfers are serviced here; the repeating VBlank/HBlank
    // modes are driven from their own per-frame call sites.
    uint32_t timing = mode & 0x30000000u;
    if (timing != DMA_START_NOW)
        return;

    bool srcFixed = (mode & DMA_SRC_FIXED) != 0;
    uint32_t destMode = mode & DMA_DEST_MODE_MASK;

    volatile uint8_t *src = (volatile uint8_t *)(uintptr_t)sad;
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)dad;

    // With a fixed source the hardware latches one unit and writes it repeatedly;
    // indexing by the loop counter (as this did) walked off the end of the single
    // unit and into unmapped memory.
    // DmaFill passes the address of a 1- or 2-byte stack local as the source.
    // Reading it as a word would run past the end of that local, so take exactly
    // as many bytes as the transfer width.
    // A fixed-source channel is a fill: write the recorded value.
    uint32_t held = 0;
    if (srcFixed)
        held = s_fill_count ? s_fill_value[s_fill_count - 1] : 0;

    uint32_t sp = sad, dp = dad;
    for (uint32_t i = 0; i < units; i++)
    {
        uint32_t v;
        if (word)
        {
            v = srcFixed ? held : *(volatile uint32_t *)(uintptr_t)(sad + i * step);
            *(volatile uint32_t *)(uintptr_t)(dad + i * step) = v;
        }
        else
        {
            v = srcFixed ? held : *(volatile uint16_t *)(uintptr_t)(sad + i * step);
            *(volatile uint16_t *)(uintptr_t)(dad + i * step) = (uint16_t)v;
        }
    }

    // Advance SAD/DAD per the addressing mode, and reload the destination when
    // the channel asked for it (that is how the scanline effect walks a table
    // of per-line register values from the same starting point each frame).
    uint32_t newSad = sad;
    uint32_t newDad = dad;
    if (!srcFixed)
        newSad = sad + units * step;
    if (destMode == DMA_DEST_RELOAD)
        newDad = dad; // stays put; SAD keeps advancing
    else if (destMode == DMA_DEST_FIXED)
        newDad = dad;
    else if (destMode == DMA_DEST_DEC)
        newDad = dad - units * step;
    else
        newDad = dad + units * step;

    // A non-repeating channel disables itself when it runs out.
    uint32_t newCtl = (mode & DMA_REPEAT) ? ctl : 0;

    dma_write(n, newSad, newDad, newCtl);
    (void)dst;
}

// Run one immediate transfer on every channel. Called right after the game's
// code has had a chance to arm DMA and before the frame is drawn.
void native_dma_service_now(void)
{
    for (int n = 0; n < 4; n++)
        dma_run_once(n, NULL);
}

// Run one transfer for a channel armed with DMA_START_HBLANK. The scanline
// effect uses this to write one register value per line.
void native_dma_service_hblank(int scanline)
{
    for (int n = 0; n < 4; n++)
    {
        uint32_t sad, dad, ctl;
        dma_read(n, &sad, &dad, &ctl);
        if (!(ctl & DMA_ENABLE))
            continue;
        uint32_t mode = ctl & 0xFFFF0000u;
        if ((mode & 0x30000000u) != DMA_START_HBLANK)
            continue;

        // Walk one unit from the current pointer, then advance as the hardware
        // would. Repeating channels keep their enable bit.
        uint32_t count = ctl & 0xFFFFu;
        uint32_t units = count ? count : 0x10000u;
        (void)units;
        (void)scanline;
        dma_run_once(n, NULL);
    }
}
