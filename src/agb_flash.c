#include "gba/gba.h"
#include "gba/flash_internal.h"

static u8 sTimerNum;
static u16 sTimerCount;
static vu16 *sTimerReg;
static u16 sSavedIme;

COMMON_DATA u8 gFlashTimeoutFlag = 0;
COMMON_DATA u8 (*PollFlashStatus)(u8 *) = NULL;
COMMON_DATA u16 (*WaitForFlashWrite)(u8 phase, u8 *addr, u8 lastData) = NULL;
COMMON_DATA u16 (*ProgramFlashSector)(u16 sectorNum, u8 *src) = NULL;
COMMON_DATA const struct FlashType *gFlash = NULL;
COMMON_DATA u16 (*ProgramFlashByte)(u16 sectorNum, u32 offset, u8 data) = NULL;
COMMON_DATA u16 gFlashNumRemainingBytes = 0;
COMMON_DATA u16 (*EraseFlashChip)() = NULL;
COMMON_DATA u16 (*EraseFlashSector)(u16 sectorNum) = 0;
COMMON_DATA const u16 *gFlashMaxTime = NULL;

void SetReadFlash1(u16 *dest);

void SwitchFlashBank(u8 bankNum)
{
    FLASH_WRITE(0x5555, 0xAA);
    FLASH_WRITE(0x2AAA, 0x55);
    FLASH_WRITE(0x5555, 0xB0);
    FLASH_WRITE(0x0000, bankNum);
}

#define DELAY()                  \
do {                             \
    vu16 i;                      \
    for (i = 20000; i != 0; i--) \
        ;                        \
} while (0)

#if PLATFORM_NATIVE
// ReadFlashId normally ARM-copies ReadFlash1 into a stack buffer and
// calls it at offset +1 (the Thumb bit), which a host cannot execute.
// Report the "ID of 0" entry instead: that is how DefaultFlash
// terminates the search in IdentifyFlash, so the rest of the driver
// behaves exactly as it does on hardware.
u16 ReadFlashId(void)
{
    // IdentifyFlash() scans sSetupInfos for a matching chip id and treats the
    // first entry with makerId == 0 as "not found". Reporting 0 therefore makes
    // it exit with failure, and AgbMain then clears the main callback, leaving
    // the game on a blank screen. Return the id of MX29L010, the first entry in
    // the table, so the normal "flash present" path is taken and the save
    // machinery is wired up.
    return 0x09C2; // MX29L010: maker 0xC2, device 0x09 (src/agb_flash_mx.c)
}
#else
u16 ReadFlashId(void)
{
    u16 flashId;
    u16 readFlash1Buffer[0x20];
    u8 (*readFlash1)(u8 *);

    SetReadFlash1(readFlash1Buffer);
    readFlash1 = (u8 (*)(u8 *))((s32)readFlash1Buffer + 1);

    // Enter ID mode.
    FLASH_WRITE(0x5555, 0xAA);
    FLASH_WRITE(0x2AAA, 0x55);
    FLASH_WRITE(0x5555, 0x90);
    DELAY();

    flashId = readFlash1(FLASH_BASE + 1) << 8;
    flashId |= readFlash1(FLASH_BASE);

    // Leave ID mode.
    FLASH_WRITE(0x5555, 0xAA);
    FLASH_WRITE(0x2AAA, 0x55);
    FLASH_WRITE(0x5555, 0xF0);
    FLASH_WRITE(0x5555, 0xF0);
    DELAY();

    return flashId;
}
#endif

void FlashTimerIntr(void)
{
    if (sTimerCount != 0 && --sTimerCount == 0)
        gFlashTimeoutFlag = 1;
}

u16 SetFlashTimerIntr(u8 timerNum, void (**intrFunc)(void))
{
    if (timerNum >= 4)
        return 1;

    sTimerNum = timerNum;
    sTimerReg = &REG_TMCNT_L(sTimerNum);
    *intrFunc = FlashTimerIntr;
    return 0;
}

void StartFlashTimer(u8 phase)
{
    const u16 *maxTime = &gFlashMaxTime[phase * 3];
    sSavedIme = REG_IME;
    REG_IME = 0;
    sTimerReg[1] = 0;
    REG_IE |= (INTR_FLAG_TIMER0 << sTimerNum);
    gFlashTimeoutFlag = 0;
    sTimerCount = *maxTime++;
    *sTimerReg++ = *maxTime++;
    *sTimerReg-- = *maxTime++;
    REG_IF = (INTR_FLAG_TIMER0 << sTimerNum);
    REG_IME = 1;
}

void StopFlashTimer(void)
{
    REG_IME = 0;
    *sTimerReg++ = 0;
    *sTimerReg-- = 0;
    REG_IE &= ~(INTR_FLAG_TIMER0 << sTimerNum);
    REG_IME = sSavedIme;
}

u8 ReadFlash1(u8 *addr)
{
    return *addr;
}

#if PLATFORM_NATIVE
// Same reasoning as ReadFlashId above: the ARM body copies the ReadFlash1
// machine code into a caller-supplied buffer and hands out a Thumb-bit
// pointer into it. A host cannot execute a copy at an odd address, and the
// (s32) casts truncate the 64-bit host address, so PollFlashStatus would
// point at garbage. ReadFlash1 is already plain portable C that returns
// *addr, so the direct function pointer is the correct host equivalent.
void SetReadFlash1(u16 *dest)
{
    PollFlashStatus = ReadFlash1;
}
#else
void SetReadFlash1(u16 *dest)
{
    u16 *src;
    u16 i;

    PollFlashStatus = (u8 (*)(u8 *))((s32)dest + 1);

    src = (u16 *)ReadFlash1;
    src = (u16 *)((s32)src ^ 1);

    i = ((s32)SetReadFlash1 - (s32)ReadFlash1) >> 1;

    while (i != 0)
    {
        *dest++ = *src++;
        i--;
    }
}
#endif

// Using volatile here to make sure the flash memory will ONLY be read as bytes, to prevent any compiler optimizations.
void ReadFlash_Core(vu8 *src, u8 *dest, u32 size)
{
    while (size-- != 0)
    {
        *dest++ = *src++;
    }
}

#if PLATFORM_NATIVE
// Same ARM-copy trick as ReadFlashId; read the mapped save directly instead.
void ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    const u8 *src = FLASH_BASE + (sectorNum << gFlash->sector.shift) + offset;
    u32 i;

    for (i = 0; i < size; i++)
        dest[i] = src[i];
}
#else
void ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    u8 *src;
    u16 i;
    vu16 readFlash_Core_Buffer[0x40];
    vu16 *funcSrc;
    vu16 *funcDest;
    void (*readFlash_Core)(vu8 *, u8 *, u32);

    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;
    }

    funcSrc = (vu16 *)ReadFlash_Core;
    funcSrc = (vu16 *)((s32)funcSrc ^ 1);
    funcDest = readFlash_Core_Buffer;

    i = ((s32)ReadFlash - (s32)ReadFlash_Core) >> 1;

    while (i != 0)
    {
        *funcDest++ = *funcSrc++;
        i--;
    }

    readFlash_Core = (void (*)(vu8 *, u8 *, u32))((s32)readFlash_Core_Buffer + 1);

    src = FLASH_BASE + (sectorNum << gFlash->sector.shift) + offset;

    readFlash_Core(src, dest, size);
}
#endif

u32 VerifyFlashSector_Core(u8 *src, u8 *tgt, u32 size)
{
    while (size-- != 0)
    {
        if (*tgt++ != *src++)
            return (u32)(tgt - 1);
    }

    return 0;
}

#if PLATFORM_NATIVE
// Same ARM-copy trick as SetReadFlash1: the ARM body copies
// VerifyFlashSector_Core's machine code into a stack buffer and calls it
// through a Thumb-bit pointer into that buffer. A host cannot execute a copy
// at an odd address, and the (s32) cast truncates the 64-bit host address, so
// the call would jump to garbage. VerifyFlashSector_Core is already portable C,
// so call it directly.
u32 VerifyFlashSector(u16 sectorNum, u8 *src)
{
    u8 *tgt;
    u16 size;

    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;
    }

    tgt = FLASH_BASE + (sectorNum << gFlash->sector.shift);
    size = gFlash->sector.size;

    return VerifyFlashSector_Core(src, tgt, size);
}

u32 VerifyFlashSectorNBytes(u16 sectorNum, u8 *src, u32 n)
{
    u8 *tgt;

    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;
    }

    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    tgt = FLASH_BASE + (sectorNum << gFlash->sector.shift);

    return VerifyFlashSector_Core(src, tgt, n);
}
#else
u32 VerifyFlashSector(u16 sectorNum, u8 *src)
{
    u16 i;
    vu16 verifyFlashSector_Core_Buffer[0x80];
    vu16 *funcSrc;
    vu16 *funcDest;
    u8 *tgt;
    u16 size;
    u32 (*verifyFlashSector_Core)(u8 *, u8 *, u32);

    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;
    }

    funcSrc = (vu16 *)VerifyFlashSector_Core;
    funcSrc = (vu16 *)((s32)funcSrc ^ 1);
    funcDest = verifyFlashSector_Core_Buffer;

    i = ((s32)VerifyFlashSector - (s32)VerifyFlashSector_Core) >> 1;

    while (i != 0)
    {
        *funcDest++ = *funcSrc++;
        i--;
    }

    verifyFlashSector_Core = (u32 (*)(u8 *, u8 *, u32))((s32)verifyFlashSector_Core_Buffer + 1);

    tgt = FLASH_BASE + (sectorNum << gFlash->sector.shift);
    size = gFlash->sector.size;

    return verifyFlashSector_Core(src, tgt, size);
}

u32 VerifyFlashSectorNBytes(u16 sectorNum, u8 *src, u32 n)
{
    u16 i;
    vu16 verifyFlashSector_Core_Buffer[0x80];
    vu16 *funcSrc;
    vu16 *funcDest;
    u8 *tgt;
    u32 (*verifyFlashSector_Core)(u8 *, u8 *, u32);

    if (gFlash->romSize == FLASH_ROM_SIZE_1M)
    {
        SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
        sectorNum %= SECTORS_PER_BANK;
    }

    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    funcSrc = (vu16 *)VerifyFlashSector_Core;
    funcSrc = (vu16 *)((s32)funcSrc ^ 1);
    funcDest = verifyFlashSector_Core_Buffer;

    i = ((s32)VerifyFlashSector - (s32)VerifyFlashSector_Core) >> 1;

    while (i != 0)
    {
        *funcDest++ = *funcSrc++;
        i--;
    }

    verifyFlashSector_Core = (u32 (*)(u8 *, u8 *, u32))((s32)verifyFlashSector_Core_Buffer + 1);

    tgt = FLASH_BASE + (sectorNum << gFlash->sector.shift);

    return verifyFlashSector_Core(src, tgt, n);
}
#endif

u32 ProgramFlashSectorAndVerify(u16 sectorNum, u8 *src)
{
    u8 i;
    u32 result;

    for (i = 0; i < 3; i++)
    {
        result = ProgramFlashSector(sectorNum, src);
        if (result != 0)
            continue;

        result = VerifyFlashSector(sectorNum, src);
        if (result == 0)
            break;
    }

    return result;
}

u32 ProgramFlashSectorAndVerifyNBytes(u16 sectorNum, u8 *src, u32 n)
{
    u8 i;
    u32 result;

    for (i = 0; i < 3; i++)
    {
        result = ProgramFlashSector(sectorNum, src);
        if (result != 0)
            continue;

        result = VerifyFlashSectorNBytes(sectorNum, src, n);
        if (result == 0)
            break;
    }

    return result;
}
