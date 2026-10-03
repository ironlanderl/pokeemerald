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

// Control word bits (include/gba/io_reg.h).
#define DMA_DEST_INC 0x00000000u
#define DMA_DEST_DEC 0x00000020u
#define DMA_DEST_FIXED 0x00000040u
#define DMA_DEST_RELOAD 0x00000060u
#define DMA_SRC_INC 0x00000080u
#define DMA_SRC_DEC 0x00000100u
#define DMA_SRC_FIXED 0x00000100u
#define DMA_REPEAT 0x00000200u
#define DMA_16BIT 0x00000000u
#define DMA_32BIT 0x00000400u
#define DMA_START_NOW 0x00000000u
#define DMA_START_VBLANK 0x00001000u
#define DMA_START_HBLANK 0x00002000u
#define DMA_ENABLE 0x00008000u

// Offset of DMA0's registers within the IO page (REG_OFFSET_DMA0SAD in
// include/gba/io_reg.h). This must be a page-relative offset because the accessors
// already add NATIVE_IO: with the full address 0x040000B0 here, the sum landed at
// 0x080000B0 -- inside the mapped ROM -- so every DMA control read came from the
// wrong place and no transfer ever ran.
#define DMA_BASE 0x000000B0u
#define DMA_STRIDE 0x00Cu

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
static void dma_run_once(int n, uint32_t *cnt)
{
    uint32_t sad, dad, ctl;
    dma_read(n, &sad, &dad, &ctl);

    if (!(ctl & DMA_ENABLE))
        return;

    uint32_t mode = ctl & 0xFFFF0000u;
    uint32_t count = ctl & 0x0000FFFFu;
    bool word = (mode & DMA_32BIT) != 0;

    // A zero count means 0x10000 transfers.
    uint32_t units = count ? count : 0x10000u;

    // Only immediate transfers are serviced here; the repeating VBlank/HBlank
    // modes are driven from their own per-frame call sites.
    uint32_t timing = mode & 0x30000000u;
    if (timing == DMA_START_NOW)
    {
        volatile uint32_t *s = (volatile uint32_t *)(uintptr_t)sad;
        volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)dad;

        for (uint32_t i = 0; i < units; i++)
        {
            uint32_t v;
            if (word)
            {
                v = s[i];
                d[i] = v;
            }
            else
            {
                v = ((volatile uint16_t *)(uintptr_t)sad)[i];
                ((volatile uint16_t *)(uintptr_t)dad)[i] = (uint16_t)v;
            }
        }

        // Advance SAD/DAD unless the transfer is fixed-source or fixed-dest, and
        // reload the destination if the control word asked for it.
        uint32_t newSad = sad;
        uint32_t newDad = dad;
        if ((mode & DMA_SRC_FIXED) != DMA_SRC_FIXED)
            newSad = word ? sad + units * 4 : sad + units * 2;
        if ((mode & 0x60u) == DMA_DEST_INC)
            newDad = word ? dad + units * 4 : dad + units * 2;
        else if ((mode & 0x60u) == DMA_DEST_DEC)
            newDad = word ? dad - units * 4 : dad - units * 2;

        // A non-repeating channel disables itself when it runs out.
        uint32_t newCtl = ctl;
        if (!(mode & DMA_REPEAT))
            newCtl = 0;

        dma_write(n, newSad, newDad, newCtl);
    }
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
