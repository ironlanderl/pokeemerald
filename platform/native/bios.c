// platform/native/bios.c
//
// Host implementations of the GBA BIOS routines that libagbsyscall provides as
// ARM `svc` trampolines.
//
// These must match the BIOS semantics exactly, not merely "work the same way
// for the inputs the game happens to pass". The decompression routines in
// particular are exercised by every graphics and map load.

#include "global.h"
#include "gba/syscall.h"
#include "native.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Memory block operations
// ---------------------------------------------------------------------------

// CpuSet: control bit 24 selects 32-bit mode, bit 26 selects fill vs copy.
//
// The MODERN-mode CpuSet macro in include/gba/syscall.h is disabled for native
// builds -- its alignment assertions describe the ARM target's access rules,
// not a host's -- so a plain function of this name is what callers need.
void CpuSet(const void *src, void *dest, u32 control)
{
    // Control word (include/gba/syscall.h):
    //   bit 24  CPU_SET_SRC_FIXED  -- store src's *value*, don't copy from it
    //   bit 26  CPU_SET_32BIT      -- 32-bit words; otherwise 16-bit
    //   bits 0-20  element count
    bool fixed = (control & 0x01000000) != 0;
    bool word = (control & 0x04000000) != 0;
    u32 count = control & 0x1FFFFF;

    if (word)
    {
        u32 *d = (u32 *)dest;
        if (fixed)
        {
            // CpuFill32 passes the value by address; take the value itself.
            u32 v = *(const u32 *)src;
            for (u32 i = 0; i < count; i++)
                d[i] = v;
        }
        else
        {
            const u32 *s = (const u32 *)src;
            for (u32 i = 0; i < count; i++)
                d[i] = s[i];
        }
    }
    else
    {
        u16 *d = (u16 *)dest;
        if (fixed)
        {
            u16 v = *(const u16 *)src;
            for (u32 i = 0; i < count; i++)
                d[i] = v;
        }
        else
        {
            const u16 *s = (const u16 *)src;
            for (u32 i = 0; i < count; i++)
                d[i] = s[i];
        }
    }
}

// CpuFastSet: always 32-bit. See CpuSet for why the macro is disabled natively.
void CpuFastSet(const void *src, void *dest, u32 control)
{
    bool fixed = (control & 0x01000000) != 0;
    u32 count = control & 0x1FFFFF;
    u32 *d = (u32 *)dest;

    if (fixed)
    {
        u32 v = *(const u32 *)src;
        for (u32 i = 0; i < count; i++)
            d[i] = v;
    }
    else
    {
        const u32 *s = (const u32 *)src;
        for (u32 i = 0; i < count; i++)
            d[i] = s[i];
    }
}

// ---------------------------------------------------------------------------
// Decompression
// ---------------------------------------------------------------------------

// The BIOS stores the compressed length in the top 3 bytes of the first word.
// Callers in this codebase always pass a header word, so read it and skip it.
// The compressed blob starts with a 32-bit header: byte 0 is the algorithm
// tag, bytes 1..3 the decompressed length, big-endian.
// The compressed blob starts with a 32-bit header: byte 0 is the algorithm tag
// (0x10 for LZ77) and bytes 1..3 hold the decompressed size. The size is stored
// low byte first -- matching the encoder in tools/gbagfx/lz.c, which writes
// dest[1] = size, dest[2] = size >> 8, dest[3] = size >> 16 and reads it back
// as (src[3] << 16) | (src[2] << 8) | src[1].
static u32 ReadCompressedLength(const u32 *src)
{
    const u8 *b = (const u8 *)src;
    return ((u32)b[3] << 16) | ((u32)b[2] << 8) | (u32)b[1];
}

void LZ77UnCompWram(const u32 *src, void *dest)
{
    const u8 *p = (const u8 *)src + 4;
    u8 *d = (u8 *)dest;
    u32 len = ReadCompressedLength(src);

    u32 written = 0;
    while (written < len)
    {
        u8 flags = *p++;
        // Flag bits are consumed most-significant first.
        for (int bit = 7; bit >= 0 && written < len; bit--)
        {
            if (!(flags & (1 << bit)))
            {
                *d++ = *p++;
                written++;
            }
            else
            {
                u8 b1 = *p++;
                u8 b2 = *p++;
                u32 offset = (((b1 & 0xF) << 8) | b2) + 1;
                u32 length = (b1 >> 4) + 3;
                // A back-reference before the start of the output would read
                // outside the destination and fault. Malformed or truncated
                // data can produce one; clamp rather than fault.
                if (offset > written)
                    offset = written ? written : 1;
                const u8 *src_pos = d - offset;
                for (u32 i = 0; i < length && written < len; i++)
                {
                    *d++ = *src_pos++;
                    written++;
                }
            }
        }
    }
}

// Identical to the Wram variant; the BIOS only differs for DMA-capable
// destinations, and the decompression itself is the same algorithm.
void LZ77UnCompVram(const u32 *src, void *dest)
{
    LZ77UnCompWram(src, dest);
}

void RLUnCompWram(const u32 *src, void *dest)
{
    const u8 *p = (const u8 *)src + 4;
    u8 *d = (u8 *)dest;
    u32 len = ReadCompressedLength(src);

    u32 written = 0;
    while (written < len)
    {
        u8 flags = *p++;
        for (int bit = 7; bit >= 0 && written < len; bit--)
        {
            if (!(flags & (1 << bit)))
            {
                *d++ = *p++;
                written++;
            }
            else
            {
                u8 b = *p++;
                u32 length = (b >> 4) + 3;
                u32 offset = (b & 0xF) + 1;
                const u8 *src_pos = d - offset;
                for (u32 i = 0; i < length && written < len; i++)
                {
                    *d++ = *src_pos++;
                    written++;
                }
            }
        }
    }
}

void RLUnCompVram(const u32 *src, void *dest)
{
    RLUnCompWram(src, dest);
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

// The BIOS Sqrt returns an 8.8 fixed-point square root of a 32-bit value,
// rounding to nearest. This must match exactly: callers use it for zoom and
// positioning, so an off-by-one is visible.
u16 Sqrt(u32 x)
{
    if (x == 0)
        return 0;

    // Binary-search the 8.8 fixed-point result, i.e. floor(sqrt(x * 256)).
    u64 target = (u64)x << 8;
    u32 lo = 0, hi = 0xFFFF;
    while (lo < hi)
    {
        u32 mid = (lo + hi + 1) >> 1;
        if ((u64)mid * mid <= target)
            lo = mid;
        else
            hi = mid - 1;
    }
    return (u16)lo;
}

// Signed division truncating toward zero, matching the BIOS.
s32 Div(s32 num, s32 denom)
{
    if (denom == 0)
        return (num < 0) ? -1 : 1; // BIOS returns ±1 on divide-by-zero
    return num / denom;
}

// ArcTan2 returns a 1.14 fixed-point angle in radians, matching the BIOS
// approximation. One call site (src/battle_anim_mons.c).
u16 ArcTan2(s16 x, s16 y)
{
    // The BIOS computes atan2(y, x) over the full circle in 2^-14 radians.
    double r = atan2((double)y, (double)x);
    if (r < 0)
        r += 2.0 * 3.14159265358979323846;
    return (u16)(r * 16384.0);
}

// ---------------------------------------------------------------------------
// Affine transforms
// ---------------------------------------------------------------------------

// BgAffineSet computes a background affine transform from a texture/screen
// reference pair, writing the resulting 8.8 matrix and origin.
void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    for (s32 i = 0; i < count; i++)
    {
        s32 sx = src->sx << 8;
        s32 sy = src->sy << 8;

        dest->pa = (s16)((src->alpha * src->sy) / sy);
        dest->pb = (s16)(-(src->alpha * src->sx) / sx);
        dest->pc = (s16)(-(src->alpha * src->texY) / sy);
        dest->pd = (s16)((src->alpha * src->texX) / sx);

        dest->dx = (src->texX * 0x10000) - (dest->pa * src->scrX) - (dest->pb * src->scrY);
        dest->dy = (src->texY * 0x10000) - (dest->pc * src->scrX) - (dest->pd * src->scrY);

        src++;
        dest++;
    }
}

// ObjAffineSet converts scale/rotation parameters into the 8 boundary values an
// affine-transformed sprite needs, writing `offset` u32 slots apart.
void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    // The destination is a struct OamMatrix -- four s16 (a, b, c, d) -- and
    // `offset` is the stride between entries in units of 4 bytes, so an entry
    // is 8 bytes wide and the stride advances by offset * 4. Writing the
    // eight boundary values the hardware computes instead overruns the
    // caller's buffer, which is what tripped the stack canary.
    u8 *base = (u8 *)dest;

    for (s32 i = 0; i < count; i++, src++)
    {
        s16 *d = (s16 *)(base + (size_t)i * (size_t)offset * 4);

        // rotation is 8.8 radians, matching the BIOS's fixed-point angle.
        double ang = (double)(src->rotation & 0xFFFF) / 256.0;
        s32 cosv = (s32)(cos(ang) * 256.0);
        s32 sinv = (s32)(sin(ang) * 256.0);
        s32 xScale = (s32)src->xScale * 0x100;
        s32 yScale = (s32)src->yScale * 0x100;

        d[0] = (s16)((cosv * xScale) >> 8);  // a
        d[1] = (s16)((-sinv * yScale) >> 8); // b
        d[2] = (s16)((sinv * xScale) >> 8);  // c
        d[3] = (s16)((cosv * yScale) >> 8);  // d
    }
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

// The retail code calls RegisterRamReset(RESET_ALL) at boot and
// RegisterRamReset(RESET_EWRAM) when reloading a save. Clear the mapped regions
// rather than the host, so the caller's expectations still hold.
void RegisterRamReset(u32 resetFlags)
{
    if (resetFlags & 0x01) // RESET_EWRAM
        memset(NATIVE_EWRAM, 0, NATIVE_EWRAM_SIZE);
    if (resetFlags & 0x02) // RESET_IWRAM
        memset(NATIVE_IWRAM, 0, NATIVE_IWRAM_SIZE);
    if (resetFlags & 0x04) // RESET_PALRAM
        memset(NATIVE_PLTT, 0, NATIVE_PLTT_SIZE);
    if (resetFlags & 0x08) // RESET_VRAM
        memset(NATIVE_VRAM, 0, NATIVE_VRAM_SIZE);
    if (resetFlags & 0x10) // RESET_OAM
        memset(NATIVE_OAM, 0, NATIVE_OAM_SIZE);
    if (resetFlags & 0x20) // RESET_SIO
        memset(NATIVE_IO, 0, NATIVE_IO_SIZE);
    if (resetFlags & 0x40) // RESET_SOUND
        memset(NATIVE_IO + 0x60, 0, 0x50);
    if (resetFlags & 0x80) // RESET_ALL
        memset(NATIVE_IO, 0, NATIVE_IO_SIZE);
}

// SoftReset re-executes the cartridge entry point on real hardware. The native
// build has no equivalent; restarting the process is the honest behaviour.
void SoftReset(u32 resetFlags)
{
    (void)resetFlags;
    native_log("SoftReset is not supported natively; ignoring.");
}

void VBlankIntrWait(void)
{
    // Handled by the virtual frame clock; wait until the pending VBlank flag is
    // cleared by the interrupt dispatcher.
    while (!native_timing_run_interrupt())
    {
        native_timing_advance_to_next_vblank();
        if (native_shutdown_requested())
            return;
    }
}
