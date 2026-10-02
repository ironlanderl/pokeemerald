// platform/native/stubs.c
//
// Stubs for subsystems the native port does not support yet.
//
// The GameCube link, multiboot and RFU code all run ARM inline assembly or
// self-relocating Thumb trampolines that cannot execute on a host. None of it
// is reachable in single-player play, so the entry points report failure and
// let the game take its existing error path.
//
// The m4a sound engine is a different case: it is genuinely used, so these
// stubs only satisfy the linker until the real port lands in the audio phase.
// They produce silence. Signatures match include/gba/m4a_internal.h exactly,
// which is what lets the link close without casts.

#include "global.h"
#include "gba/m4a_internal.h"
#include "native.h"

#include <stddef.h>

// --- GameCube / Game Boy link cable ---------------------------------------

void GameCubeMultiBoot_Init(void)
{
}

void GameCubeMultiBoot_Main(void)
{
}

void GameCubeMultiBoot_Quit(void)
{
}

void GameCubeMultiBoot_ExecuteProgram(void)
{
}

void GameCubeMultiBoot_HandleSerialInterrupt(void)
{
}

// The BIOS slave-mode download. Returning non-zero makes MultiBootMain take
// its failure branch, which is what berry_fix_program.c expects.
int MultiBoot(struct MultiBootParam *mp)
{
    (void)mp;
    return -1;
}

// The RFU stack copies its own code into IWRAM at boot (rfu_STC_fastCopy) and
// installs a misaligned entry pointer; on a host that is a guaranteed crash.
void RealClearChain(void *x)
{
    (void)x;
}

// --- m4a sound engine ------------------------------------------------------
//
// src/m4a_1.s is ARM assembly implementing a 12-channel mixer with envelope,
// pan and pseudo-echo. It is replaced by a C port in the audio phase; until
// then these keep the link closed and output silence.

// Declared as `extern char SoundMainRAM[]`: m4aSoundInit copies the ARM mixer
// into it at boot, so it needs somewhere to land.
char SoundMainRAM[0x800] = {0};

void SoundMain(void)
{
}

void SoundMainBTM(void)
{
}

void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    (void)mplayInfo;
}

void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    (void)track;
}

// m4a's channel command handlers reach the mixer through gMPlayJumpTable, which
// MPlayJumpTableCopy fills with 36 pointers taken from a template in the ROM.
// The ARM original (src/m4a_1.s) reads the template address out of a literal
// pool and copies 0x24 words; the entries themselves are already patched to
// native addresses by the loader, so the copy is a straight memcpy.
const u32 *const gMPlayJumpTableTemplatePtr = (const u32 *)(uintptr_t)0x086759E0;

void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable)
{
    for (int i = 0; i < 36; i++)
        mplayJumpTable[i] = (MPlayFunc)gMPlayJumpTableTemplatePtr[i];
}

void m4aSoundVSync(void)
{
}

// m4a's channel command handlers. The sequencer reaches these through a jump
// table it builds at runtime; no-ops are correct while there is no mixer.
// Signatures come from include/gba/m4a_internal.h -- several of them differ
// (ply_note takes a leading command word), so they are spelled out per
// function rather than through a macro.
void ply_fine(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_goto(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_patt(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_pend(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_rept(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_prio(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_tempo(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_keysh(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_voice(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_vol(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_pan(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_bend(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_bendr(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_lfos(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_lfodl(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_mod(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_modt(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_tune(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_port(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_endtie(struct MusicPlayerInfo *a, struct MusicPlayerTrack *b) { (void)a; (void)b; }
void ply_note(u32 note_cmd, struct MusicPlayerInfo *a, struct MusicPlayerTrack *b)
{
    (void)note_cmd;
    (void)a;
    (void)b;
}

// --- m4a player track buffers ----------------------------------------------
//
// sound/music_player_table.inc reserves these as zero-filled IWRAM buffers of
// TRACK_SIZE (0x50) bytes per track. They are declared in that assembly and
// referenced from src/m4a.c, so the native build needs definitions: MPlayOpen
// walks them even when the mixer itself is stubbed.
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_BGM[10] = {0};
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_SE1[3] = {0};
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_SE2[9] = {0};
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_SE3[1] = {0};

// --- debug menu -----------------------------------------------------------
//
// Wired up in a later phase; declared weak so the input layer can call it.
__attribute__((weak)) bool native_debug_menu_toggle_requested(void)
{
    return false;
}