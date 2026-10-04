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
// which is what lets the link close without casts. The exception is MPlayMain,
// which lives in audio.c because game logic reads what it writes.

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

void SoundMainBTM(void)
{
}

// MPlayMain is not here: it is real code in audio.c. See the comment there --
// leaving it a stub keeps MusicPlayerInfo.status permanently zero, which locks
// the title screen out of the main menu.

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