// platform/native/stubs.c
//
// Stubs for subsystems the native port does not support yet.
//
// The GameCube link, multiboot and RFU code all run ARM inline assembly or
// self-relocating Thumb trampolines that cannot execute on a host. None of it
// is reachable in single-player play, so the entry points report failure and
// let the game take its existing error path.
//
// The m4a sound engine used to be stubbed here too. It is not any more: the
// mixer is in mixer.c, the sequencer in seq.c, and SoundMain in pcmout.c.

#include "global.h"
#include "gba/m4a_internal.h"
#include "libgcnmultiboot.h"
#include "native.h"

#include <stddef.h>

// --- GameCube / Game Boy link cable ---------------------------------------

// The GameCube multiboot handshake drives the copyright screen in src/intro.c.
// COPYRIGHT_START_FADE waits for gcmb_field_2 to become 1 (a GameCube answered)
// and only then fades out; COPYRIGHT_START_INTRO then requires it to be back to
// 0 to proceed. A real GameCube would flip it 0 -> 1 -> 2 (demo finished) -> 0.
//
// There is no GameCube on the other end, so mimic the settled state: report 1
// so the fade-out runs, and 0 afterwards so the intro starts. Reporting 1 for
// both strands the screen in COPYRIGHT_START_FADE and the game never reaches
// the intro.
// Field 2 tracks the handshake across the copyright screen: intro.c waits for 1
// to start the fade, then for 0 to start the intro. With no GameCube present,
// latch it at 1 from Init and only clear it once the intro has actually begun,
// which is what lets both gates fall through in order.
static int s_handshake_seen;

void GameCubeMultiBoot_Init(struct GcmbStruct *pStruct)
{
    s_handshake_seen = 0;
    if (pStruct)
        pStruct->gcmb_field_2 = 1;
}

void GameCubeMultiBoot_Main(struct GcmbStruct *pStruct)
{
    if (!pStruct)
        return;
    if (!s_handshake_seen)
    {
        s_handshake_seen = 1;
        return;
    }
    pStruct->gcmb_field_2 = 0;
}

void GameCubeMultiBoot_Quit(void)
{
}

void GameCubeMultiBoot_ExecuteProgram(struct GcmbStruct *pStruct)
{
    (void)pStruct;
}

void GameCubeMultiBoot_HandleSerialInterrupt(struct GcmbStruct *pStruct)
{
    (void)pStruct;
}

// The BIOS slave-mode download. Returning non-zero makes MultiBootMain take
// its failure branch, which is what berry_fix_program.c expects.
int MultiBoot(struct MultiBootParam *mp)
{
    (void)mp;
    return -1;
}

// --- m4a sound engine ------------------------------------------------------
//
// The one thing still needed here is SoundMainRAM itself: src/m4a_1.s is ARM
// assembly, and m4aSoundInit copies it into this buffer at boot with
// `CpuCopy32((void *)((s32)SoundMainRAM & ~1), SoundMainRAM_Buffer, ...)`.
// The native mixer in mixer.c replaces it outright, but the copy still runs and
// needs somewhere to land.
char SoundMainRAM[0x800] = {0};

// MPlayJumpTableCopy fills the 36-entry dispatch table the sequencer uses. The
// ARM original (src/m4a_1.s:779-819) reads the template's address out of a
// literal pool and copies 0x24 words, checking each against the BIOS range on
// the way -- a guard against reading BIOS jump thunks. src/m4a_tables.c defines
// the template as a C array of function pointers, so on a host the entries are
// already native addresses and the copy is a straight read.
extern void *const gMPlayJumpTableTemplate[];

void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable)
{
    for (int i = 0; i < 36; i++)
        mplayJumpTable[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
}

// --- debug menu -----------------------------------------------------------
//
// Wired up in a later phase; declared weak so the input layer can call it.
__attribute__((weak)) bool native_debug_menu_toggle_requested(void)
{
    return false;
}