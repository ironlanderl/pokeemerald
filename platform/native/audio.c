// platform/native/audio.c
//
// The parts of the m4a sequencer that game logic actually reads.
//
// src/m4a_1.s is ARM assembly: a 12-channel software mixer plus the track
// sequencer that drives it. Porting the mixer is the audio phase of the port
// and is not done. What *is* done here is the piece that turns out not to be
// cosmetic: `MusicPlayerInfo.status`.
//
// Several screens decide what to do next by asking the sound engine what is
// playing. src/title_screen.c:816 is the important one -- the title screen
// reads the low 16 bits (one flag per track) and, if none are set, concludes
// that no BGM is playing and bails out to the copyright screen. On hardware
// those bits are written by MPlayMain. With MPlayMain stubbed out they stay
// zero forever, the title screen exits on its first frame, and because
// CB2_InitCopyrightScreenAfterTitleScreen runs the same state machine as the
// boot path (whose terminal state is COPYRIGHT_START_INTRO -> CreateTask(
// Task_Scene1_Load) + SetMainCallback2(MainCB2_Intro), src/intro.c) the game
// replays the entire intro and lands back on the copyright screen, forever.
// No amount of button pressing reaches the main menu, because the check runs
// on every frame of title-screen phase 3 and Enter only has to be new on one
// of them.
//
// So MPlayMain below keeps the status bookkeeping honest without pretending to
// be a mixer: it derives the per-track flags from the tracks themselves, which
// is what the ARM version's exit path does.
//
// A version that recomputed `status` unconditionally shipped and was wrong in
// three ways, all of them soft-locks reachable from ordinary play. It is worth
// recording why, because each one looks harmless in isolation:
//
//   - Without the negative-status gate, the recompute erased the
//     MUSICPLAYER_STATUS_PAUSE that m4aMPlayStop / m4aMPlayAllStop had just
//     set, one VBlank later. IsSEPlaying() then latched TRUE forever, and every
//     {WAIT_SE} gate stopped releasing -- the level-up message, Repel in the
//     overworld, saving, the Pokedex cry page, the trainer card, and the whole
//     evolution scene.
//   - Without the FadeOutBody call, no fade ever completed: nothing counted
//     fadeOC down, nothing cleared a track flag, nothing set PAUSE. So
//     ScrCmd_fadeoutbgm's SetupNativeScript(IsBGMPausedOrStopped) blocked its
//     script permanently -- including the Rayquaza awakening, which puts
//     `fadeoutbgm 1` inside a lockall -- and MapMusicMain stuck in state 5.
//   - Deriving from MPT_FLG_START rather than MPT_FLG_EXIST reported PAUSE
//     where hardware reports a playing mask, because m4aMPlayImmInit clears
//     START but deliberately keeps EXIST.
//
// Before this file existed `status` was permanently zero, so all three
// predicates were trivially satisfied and nothing hung -- you just got no
// audio. Trading that for a title screen that works is only a good deal if
// nothing else regresses, which is why the three points above are load-bearing
// rather than incidental.

#include "global.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "native.h"

// Advance one music player by a tick.
//
// The ARM original (src/m4a_1.s, MPlayMain at _081DD840) does, in order:
//
//   - bail unless mplayInfo->ident == ID_NUMBER, then bump ident as a
//     re-entrancy lock and restore it before returning;
//   - if status is NEGATIVE, skip everything and return -- PAUSE is
//     0x80000000, so `cmp r0, 0 / bge` (m4a_1.s:1154) is a signed test and a
//     paused player leaves with status, clock and tempoC untouched;
//   - call FadeOutBody, which is what counts the fade down, clears each track's
//     flags and finally sets PAUSE;
//   - check status's sign again, for the case where FadeOutBody just set it;
//   - run the track loop, which ORs one bit per existing track into the status
//     it is about to store, and advance tempoC/clock;
//   - at _081DD9A4, store that OR, or MUSICPLAYER_STATUS_PAUSE if no track
//     exists;
//   - release the ident lock.
//
// Only the track loop is missing here: it is the part that needs the mixer,
// and it is what would clear MPT_FLG_* as a song ends. The status store is not
// missing, and the two gates and the FadeOutBody call around it are what keep
// this honest -- FadeOutBody already clears track->flags itself, so it does not
// depend on the stubbed TrackStop.
void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    if (mplayInfo->ident != ID_NUMBER)
        return;

    mplayInfo->ident++; // re-entrancy lock, released on every path below

    // Gate 1: a paused player has a negative status and must not be touched.
    if ((s32)mplayInfo->status >= 0)
    {
        FadeOutBody(mplayInfo);

        // Gate 2: FadeOutBody may have set PAUSE during the call above.
        if ((s32)mplayInfo->status >= 0)
        {
            u32 status = 0;

            for (u32 i = 0; i < mplayInfo->trackCount; i++)
            {
                // MPT_FLG_EXIST, not MPT_FLG_START: m4aMPlayImmInit
                // (src/m4a.c:260) leaves a track as EXIST alone after clearing
                // START, and hardware keeps counting it as a playing track.
                if (mplayInfo->tracks[i].flags & MPT_FLG_EXIST)
                    status |= 1u << i;
            }

            // Hardware increments clock once per pass of the track loop, which
            // the tempo accumulator can enter more than once in a frame; here
            // there is no loop, so this is once per tick and only while
            // unpaused. Its only reader is SetPokemonCryTone's slot-stealing
            // heuristic (src/m4a.c:1690), which will be right once the mixer
            // and its tempo handling land.
            mplayInfo->clock++;

            mplayInfo->status = status != 0 ? status : MUSICPLAYER_STATUS_PAUSE;
        }
    }

    mplayInfo->ident = ID_NUMBER;
}

// The per-VBlank sound tick.
//
// The ARM SoundMain does its mixer setup once, bumps soundInfo->ident so a
// second call returns immediately, then tail-calls SoundMainRAM_Buffer + 1 --
// a copy of itself that m4aSoundInit made at boot -- and runs *that* as a
// separate BIOS task. The copy's loop is what advances the music players, once
// per VBlank.
//
// Natively there is no second task, but src/main.c:379 already calls
// m4aSoundMain() from VBlankIntr on every frame, so this function is already
// on the right cadence and the walk belongs here.
//
// The ident bump is deliberately not reproduced: on hardware it is a mutual
// exclusion lock against the concurrently-running mixer, which restores it. With
// no second task, leaving ident == ID_NUMBER permanently is what lets MPlayOpen,
// m4aSoundMode, SoundClear and MPlayExtender proceed -- all of them early-return
// while ident is bumped. The one reader whose behaviour changes is
// m4aSoundVSyncOn, whose `if (ident == ID_NUMBER) return;` now always fires; it
// only programs DMA1CNT_H/DMA2CNT_H FIFO mode and pcmDmaCounter, all inert while
// nothing mixes PCM.
//
// The ARM version calls MPlayMain on the head player only and recurses through
// the list from inside MPlayMain, so it visits the oldest player first. This
// walks musicPlayerNext from the head, which is newest-first. The set visited is
// identical and order is unobservable while each MPlayMain touches only its own
// mplayInfo -- but it becomes significant the moment channel stealing depends on
// visit order, so the mixer port should revisit this.
void SoundMain(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    // Same guard the ARM version uses: run only once m4aSoundInit has published
    // a SoundInfo and identified it.
    if (soundInfo->ident != ID_NUMBER)
        return;

    if (soundInfo->MPlayMainHead == NULL)
        return;

    // m4aMPlayOpen links every player it opens onto musicPlayerHead; the BGM and
    // both SE players are all on the list.
    for (struct MusicPlayerInfo *mplayInfo = soundInfo->musicPlayerHead;
         mplayInfo != NULL;
         mplayInfo = mplayInfo->musicPlayerNext)
    {
        soundInfo->MPlayMainHead(mplayInfo);
    }
}