// platform/native/seq.c
//
// C port of the m4a sequencer: MPlayMain and the ply_* command handlers from
// src/m4a_1.s, plus the three small channel-chain routines it calls through
// gMPlayJumpTable.
//
// src/m4a_1.s is ARM assembly and does not run on a host, so this file
// reimplements it. It is a transcription rather than a rewrite: the tempo
// accumulator, the running-status dispatch, the command byte layout (note
// durations are gClockTable lookups, repeats are a count byte followed by a
// four-byte absolute address) and the per-track LFO are all m4a semantics that
// the song data in sound/ is written against. Changing any of them detunes
// every song in the game.
//
// What the earlier stub got wrong is worth recording. It kept track->flags
// permanently set, so MPT_FLG_START was never cleared, no song could ever
// reach its end, and status could never fall to zero. Three things in the game
// depend on that happening:
//
//   - src/title_screen.c:816 reads the low 16 bits of status as "no BGM is
//     playing" and bails to the copyright screen. With status permanently
//     non-zero this gate would *stop* firing, but its inverse -- a status that
//     never goes positive -- strands everything else.
//   - IsSEPlaying() latches on a negative status. m4aMPlayStop and
//     m4aMPlayAllStop set MUSICPLAYER_STATUS_PAUSE, and every {WAIT_SE} gate
//     in the game (level-up, Repel, saving, the Pokedex cry page, evolution)
//     blocks until it clears.
//   - ScrCmd_fadeoutbgm's SetupNativeScript waits on
//     IsBGMPausedOrStopped, which needs the track loop to actually finish so
//     that FadeOutBody can set PAUSE.
//
// Both of MPlayMain's signed-status gates and the FadeOutBody call that
// surround the track loop are load-bearing and are reproduced verbatim below.

#include "global.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "native.h"

// Defined in src/m4a_tables.c but absent from include/gba/m4a_internal.h, which
// only declares what the C code needs.
extern const u8 gClockTable[];

// Defined in src/m4a.c, also not in the header.
u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

// src/m4a_1.s:9-18. The high 32 bits of a 32x32->64 multiply. This has to
// exist natively: without a definition here rompatch emits it as an absolute
// ROM address, and MidiKeyToFreq -- reached from every pitched note -- jumps
// into the mapped ARM image and takes SIGBUS.
u32 umul3232H32(u32 multiplier, u32 multiplicand)
{
    return (u32)(((uint64_t)multiplier * multiplicand) >> 32);
}

// --- m4a player track buffers ----------------------------------------------
//
// sound/music_player_table.inc reserves these as zero-filled IWRAM buffers of
// TRACK_SIZE (0x50) bytes per track. They are declared in that assembly and
// referenced from src/m4a.c, so the native build needs definitions here: the
// sequencer walks them whether or not anything is playing.
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_BGM[10] = {0};
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_SE1[3] = {0};
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_SE2[9] = {0};
COMMON_DATA struct MusicPlayerTrack gMPlayTrack_SE3[1] = {0};

// ---------------------------------------------------------------------------
// Command stream access
// ---------------------------------------------------------------------------

// The command stream is ROM data addressed by absolute 32-bit pointers, and it
// is read through the game's mmap'd ROM window, so no bounds checking is
// possible or needed -- a malformed stream would read adjacent ROM exactly as
// it would on hardware.

// src/m4a_1.s:1878-1885. Loads the byte at cmdPtr, then advances cmdPtr past
// it, which is the operand layout every one-byte command uses.
static u32 LoadCmdByte(struct MusicPlayerTrack *track)
{
    u8 *ptr = track->cmdPtr;

    track->cmdPtr = ptr + 1;
    return *ptr;
}

// src/m4a_1.s:831-849. Reads the four-byte absolute jump target at cmdPtr and
// makes it the new command pointer.
static void GotoCmd(struct MusicPlayerTrack *track)
{
    u8 *ptr = track->cmdPtr;

    track->cmdPtr = (u8 *)(uintptr_t)((uintptr_t)ptr[0]
                                      | ((uintptr_t)ptr[1] << 8)
                                      | ((uintptr_t)ptr[2] << 16)
                                      | ((uintptr_t)ptr[3] << 24));
}

// ---------------------------------------------------------------------------
// Channel chain
// ---------------------------------------------------------------------------

// src/m4a_1.s:726-748. Unhooks a channel from its track's chain and from its
// neighbours. A track can own several channels at once: the same note played
// through two voices, or a key-split voice.
void RealClearChain(void *x)
{
    struct SoundChannel *chan = (struct SoundChannel *)x;
    struct MusicPlayerTrack *track = chan->track;

    if (track == NULL)
        return;

    if (chan->prevChannelPointer == NULL)
        track->chan = (struct SoundChannel *)chan->nextChannelPointer;
    else
        ((struct SoundChannel *)chan->prevChannelPointer)->nextChannelPointer = chan->nextChannelPointer;

    if (chan->nextChannelPointer != NULL)
        ((struct SoundChannel *)chan->nextChannelPointer)->prevChannelPointer = chan->prevChannelPointer;

    chan->track = NULL;
}

// src/m4a_1.s:711-724. gMPlayJumpTable[35] -- MPlayStart, MPlayImmInit and the
// track loop clear a track's first 64 bytes, which covers everything up to and
// including its ToneData copy but not cmdPtr or the pattern stack.
void SoundMainBTM(void *x)
{
    memset(x, 0, 0x40);
}

static inline void ClearTrack64(struct MusicPlayerTrack *track)
{
    SoundMainBTM(track);
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

// src/m4a_1.s:1508-1536. Turns the track's volume and pan into the per-channel
// left/right envelope ceilings the mixer scales samples by. rhythmPan is the
// per-note pan override a drumset voice carries; for everything else it is 0
// and this reduces to a plain pan law.
static void ChnVolSet(struct MusicPlayerTrack *track, struct SoundChannel *chan)
{
    s32 vel = chan->velocity;
    s32 pan = (s8)chan->rhythmPan;
    s32 right, left;

    right = (s32)track->volMR * (vel * (0x80 + pan)) >> 14;
    chan->rightVolume = (u8)(right > 0xFF ? 0xFF : right);

    left = (s32)track->volML * (vel * (0x7F - pan)) >> 14;
    chan->leftVolume = (u8)(left > 0xFF ? 0xFF : left);
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

// src/m4a_1.s:750-777. End of track: stop every channel the track owns and
// clear its flags, which is what eventually empties the status mask.
void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan = track->chan;

    (void)mplayInfo;

    while (chan != NULL)
    {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(chan);
        chan = chan->nextChannelPointer;
    }

    track->flags = 0;
}

// src/m4a_1.s:1818-1857. Releases the still-ringing channel whose note matches,
// rather than waiting for its envelope to run out.
void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;
    u8 key;
    u8 *ptr = track->cmdPtr;

    (void)mplayInfo;

    if (*ptr < 0x80)
    {
        track->key = *ptr;
        track->cmdPtr = ptr + 1;
    }
    key = track->key;

    for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
    {
        if (!(chan->statusFlags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)))
            continue;
        if (chan->statusFlags & SOUND_CHANNEL_SF_STOP)
            continue;
        if (chan->midiKey != key)
            continue;
        chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        break;
    }
}

// src/m4a_1.s:851-866. Pushes a loop target on a three-deep pattern stack.
void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->patternLevel < 3)
    {
        track->patternStack[track->patternLevel] = track->cmdPtr + 4;
        track->patternLevel++;
        GotoCmd(track);
    }
    else
    {
        ply_fine(mplayInfo, track);
    }
}

// src/m4a_1.s:869-882. Pops the innermost pattern. Unlike ply_patt it does *not*
// jump: the pattern body has to run out on its own.
void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;

    if (track->patternLevel != 0)
    {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

// src/m4a_1.s:884-910. A repeat is a count byte followed by a four-byte target,
// except that a zero count makes it a plain four-byte goto.
void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 *ptr = track->cmdPtr;
    u32 count;

    (void)mplayInfo;

    if (ptr[0] == 0)
    {
        track->cmdPtr = ptr + 1;
        GotoCmd(track);
        return;
    }

    track->repN++;
    LoadCmdByte(track); // advances cmdPtr to the target, as ply_rept wants
    count = ptr[0];

    if (track->repN >= count)
    {
        track->repN = 0;
        track->cmdPtr = ptr + 5;
    }
    else
    {
        GotoCmd(track);
    }
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->priority = (u8)LoadCmdByte(track);
}

// src/m4a_1.s:920-931. The command carries a tempo in BPM/2; tempoD is the
// per-tick interval and tempoI the scaled accumulator step.
void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)track;
    mplayInfo->tempoD = (u16)(LoadCmdByte(track) * 2);
    mplayInfo->tempoI = (u16)((u32)mplayInfo->tempoD * mplayInfo->tempoU >> 8);
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->keyShift = (s8)LoadCmdByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

// src/m4a_1.s:945-967. Selects one of the song's twelve ToneData slots. Only
// three fields are copied; the rest of track->tone keeps whatever ply_voice or
// MPlayStart left there, which is how a key-split group keeps a base voice.
void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 index = LoadCmdByte(track);
    struct ToneData *tone = &mplayInfo->tone[index];

    // type is a byte, wav a word, and attack a *word*: for a key-split voice
    // those last four bytes are the split table pointer, not an envelope. The
    // ARM original's three ldr/str pairs copy exactly that much.
    track->tone.type = tone->type;
    track->tone.wav = tone->wav;
    memcpy(&track->tone.attack, &tone->attack, sizeof(u32));
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->vol = (u8)LoadCmdByte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->pan = (s8)(LoadCmdByte(track) - C_V);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bend = (s8)(LoadCmdByte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bendRange = (u8)LoadCmdByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoDelay = (u8)LoadCmdByte(track);
}

// src/m4a_1.s:1027-1041. modT picks what the LFO depth modulates: 0 pitches,
// anything else pans and volumes. Setting it is a no-op if it is unchanged.
void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 val;

    (void)mplayInfo;
    val = (u8)LoadCmdByte(track);
    if (track->modT != val)
    {
        track->modT = val;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->tune = (s8)(LoadCmdByte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoSpeed = (u8)LoadCmdByte(track);
    if (track->lfoSpeed == 0)
        ClearModM(track);
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->mod = (u8)LoadCmdByte(track);
    if (track->mod == 0)
        ClearModM(track);
}

// src/m4a_1.s:1056-1068. Writes a raw GBA sound register. Only the direct
// sound control register is reachable this way in practice.
void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 reg;

    (void)mplayInfo;
    reg = LoadCmdByte(track);
    *(vu8 *)(REG_SOUND1CNT_L + reg) = reg;
}

// src/m4a_1.s:881-848 -- ply_goto has no operands of its own; it is reached
// through the jump table only as part of ply_patt/ply_rept, which call
// GotoCmd directly above. The symbol stays because gMPlayJumpTableTemplate in
// src/m4a_tables.c names it.
void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    GotoCmd(track);
}

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------

// src/m4a_1.s:1538-1816. The command word is the note opcode (0xCF note, 0xD0
// end) and the stream that follows it is up to three optional bytes: key,
// velocity, gate time. Anything with bit 7 set ends the sequence early.
//
// The interesting half of this is channel allocation. A note may need a
// channel and get refused, which is the whole of m4a's voice-stealing policy:
//
//   - For the four legacy channels the request is decided by one fixed channel
//     per voice type, and a busy one is dropped unless the incoming note has a
//     lower-or-equal priority and an older track.
//   - For direct sound the eight channels are scanned for the one that matters
//     least, scored by (priority, track) with priority dominating -- lowest
//     priority number wins, ties go to the older track.
//
// Anything refused is silently dropped, which is how pokeemerald's dense
// sound effects stay from turning into noise.
void ply_note(u32 note_cmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    struct SoundChannel *chan;
    struct ToneData *tone;
    u8 *ptr;
    u8 key;
    u8 rhythmPan = 0;
    u8 cgbType;
    u32 priority;
    s32 pitch;

    track->gateTime = gClockTable[note_cmd];

    ptr = track->cmdPtr;
    if (*ptr < 0x80)
    {
        track->key = *ptr;
        ptr++;
        if (*ptr < 0x80)
        {
            track->velocity = *ptr;
            ptr++;
            if (*ptr < 0x80)
            {
                track->gateTime = (u8)(track->gateTime + *ptr);
                ptr++;
            }
        }
        track->cmdPtr = ptr;
    }

    // A key-split voice resolves the note through its key table into one of
    // twelve sub-voices; a rhythm (drumset) voice takes its pan from the
    // sub-voice's pan_sweep field and its "key" from the sub-voice's type byte,
    // which is how a drumset picks a different sample per MIDI channel. A pure
    // key split keeps the track's own key. The key table shares ToneData's
    // `attack` slot -- see asm/macros/music_voice.inc's voice_keysplit and the
    // o_MusicPlayerTrack_ToneData_keySplitTable alias in
    // constants/m4a_constants.inc.
    if (track->tone.type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
    {
        u32 idx = track->key;

        if (track->tone.type & TONEDATA_TYPE_SPL)
        {
            u32 table;
            memcpy(&table, &track->tone.attack, sizeof(table));
            idx = ((const u8 *)(uintptr_t)table)[track->key];
        }

        {
            struct ToneData *sub = &((struct ToneData *)SAVE_PTR_FROM(track->tone.wav))[idx];

            if (sub->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY))
                return; // a nested group, not a voice

            tone = sub;
            if (track->tone.type & TONEDATA_TYPE_RHY)
            {
                key = sub->type;
                if (sub->pan_sweep & 0x80)
                    rhythmPan = (u8)((sub->pan_sweep - TONEDATA_P_S_PAN) * 2);
            }
        }
    }
    else
    {
        tone = &track->tone;
        key = track->key;
    }

    priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF)
        priority = 0xFF;

    cgbType = tone->type & TONEDATA_TYPE_CGB;
    if (cgbType != 0)
    {
        if (soundInfo->cgbChans == NULL)
            return;
        chan = (struct SoundChannel *)(soundInfo->cgbChans + cgbType - 1);
        // A legacy channel is one voice per hardware channel, so there is
        // nothing to steal from: the note is dropped unless the channel is
        // free, already releasing, or is strictly less important than us.
        if ((chan->statusFlags & SOUND_CHANNEL_SF_ON)
         && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP)
         && (chan->priority < priority
             || (chan->priority == priority && chan->track < track)))
            return;
    }
    else
    {
        struct SoundChannel *best = NULL;
        struct MusicPlayerTrack *bestTrack = track;
        u8 bestPriority = priority;
        int i;
        int n = soundInfo->maxChans;

        for (i = 0; i < n && i < MAX_DIRECTSOUND_CHANNELS; i++)
        {
            struct SoundChannel *cand = &soundInfo->chans[i];

            if (!(cand->statusFlags & SOUND_CHANNEL_SF_ON))
            {
                best = cand;
                bestTrack = track;
                bestPriority = priority;
                break;
            }
            if (cand->statusFlags & SOUND_CHANNEL_SF_STOP)
                continue; // still releasing: not a candidate for a steal at all
            if (cand->priority < bestPriority)
            {
                best = cand;
                bestTrack = cand->track;
                bestPriority = cand->priority;
            }
            else if (cand->priority == bestPriority && cand->track > bestTrack)
            {
                best = cand;
                bestTrack = cand->track;
            }
        }

        if (best == NULL)
            return;
        chan = best;
    }

    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL)
        track->chan->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        ClearModM(track);

    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->priority = priority;
    chan->key = key;
    chan->rhythmPan = rhythmPan;
    chan->type = tone->type;
    chan->wav = SAVE_PTR_FROM(tone->wav);
    chan->attack = tone->attack;
    // A 16-bit store in the ARM original, which covers the pseudo-echo length
    // as well: the tail has not started yet, so its countdown starts from zero.
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = 0;
    ChnVolSet(track, chan);

    pitch = (s32)chan->key + (s8)track->keyM;
    if (pitch < 0)
        pitch = 0;

    if (cgbType != 0)
    {
        struct CgbChannel *cgb = (struct CgbChannel *)chan;

        cgb->length = tone->length;
        if (tone->pan_sweep & 0x80)
            cgb->sweep = 8;
        else if (tone->pan_sweep & 0x70)
            cgb->sweep = tone->pan_sweep;
        else
            cgb->sweep = 8;
        chan->frequency = soundInfo->MidiKeyToCgbFreq(cgbType, (u8)pitch, track->pitM);
    }
    else
    {
        chan->count = track->unk_3C;
        chan->frequency = MidiKeyToFreq(chan->wav, (u8)pitch, track->pitM);
    }

    // SF_START, not SF_START|SF_STOP: the mixer distinguishes "attack" from
    // "kill on sight" by exactly this bit.
    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

// ---------------------------------------------------------------------------
// Track stop
// ---------------------------------------------------------------------------

// src/m4a_1.s:1468-1506. Silence every channel the track owns and unlink it.
// The legacy channels need CgbOscOff first because their oscillators are
// hardware and stay running until told otherwise.
void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;

    (void)mplayInfo;

    if (!(track->flags & MPT_FLG_EXIST))
        return;

    for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
    {
        if (chan->statusFlags != 0 && (chan->type & TONEDATA_TYPE_CGB))
            SOUND_INFO_PTR->CgbOscOff(chan->type & TONEDATA_TYPE_CGB);
        chan->statusFlags = 0;
        chan->track = NULL;
    }

    track->chan = NULL;
}

// ---------------------------------------------------------------------------
// MPlayMain
// ---------------------------------------------------------------------------

// src/m4a_1.s:871-938, the command-dispatch half of one tempo tick. Returns
// with the track's wait consumed if it ran a command.
static void RunTrackCommands(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    // src/m4a_1.s:1213-1231. MPlayStart leaves MPT_FLG_START set on every
    // track it starts, and ply_note is what clears it. A track that still has
    // it on its first tick therefore gets its state reset to the defaults here
    // -- and, because the reset zeroes `wait` while leaving cmdPtr alone, it
    // falls straight through into the command loop below and reads its first
    // command in the same tick. That is where a song's opening KEYSH/TEMPO/
    // VOICE run.
    if (track->flags & MPT_FLG_START)
    {
        ClearTrack64(track);
        track->flags = MPT_FLG_EXIST;
        track->bendRange = 2;
        track->volX = 0x40;
        track->lfoSpeed = 0x16;
        track->tone.type = 1;
    }

    while (track->wait == 0)
    {
        u8 cmd = *track->cmdPtr;

        // Running status: a byte below 0x80 repeats the last real command.
        // Only commands at 0xBD and above are worth remembering, because the
        // ones below are single-operand and would consume the next byte wrong.
        if (cmd >= 0x80)
        {
            track->cmdPtr++;
            if (cmd >= 0xBD)
                track->runningStatus = cmd;
        }
        else
        {
            cmd = track->runningStatus;
        }

        if (cmd >= 0xCF)
        {
            // The note opcode arrives *rebased* to zero: ply_note indexes
            // gClockTable with it for the default gate time, and the table has
            // one entry per note from 0xCF up.
            soundInfo->plynote(cmd - 0xCF, mplayInfo, track);
            break;
        }

        if (cmd <= 0xB0)
        {
            track->wait = gClockTable[cmd - 0x80];
            break;
        }

        mplayInfo->cmd = (u8)(cmd - 0xB1);
        soundInfo->MPlayJumpTable[cmd - 0xB1](mplayInfo, track);
        // ply_fine clears the flags to end the track; anything else loops to
        // read the next command in the same tick.
        if (track->flags == 0)
            break;
    }

    if (track->wait != 0)
    {
        track->wait--;

        // src/m4a_1.s:1285-1330. The per-track LFO, advanced once per tick. Its
        // output is a triangle in modM, which TrkVolPitSet turns into either a
        // pitch offset or a pan/volume offset depending on modT.
        if (track->lfoSpeed != 0 && track->mod != 0)
        {
            if (track->lfoDelayC != 0)
            {
                track->lfoDelayC--;
            }
            else
            {
                u32 phase = (u32)(track->lfoSpeedC + track->lfoSpeed);
                s32 tri;
                s32 depth;

                track->lfoSpeedC = (u8)phase;
                tri = ((s8)phase < 0) ? (s8)phase : 0x80 - (s32)phase;
                depth = ((s32)track->mod * tri) >> 6;
                if ((track->modM ^ depth) != 0)
                {
                    track->modM = (s8)depth;
                    track->flags |= track->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                }
            }
        }
    }
}

// src/m4a_1.s:1360-1448. Applies whatever volume and pitch change the tracks
// flagged, to every channel they own. Run once per tick after the note stream,
// which is why a `vol` command is heard at the start of the next tick.
static void RunVolPit(struct MusicPlayerInfo *mplayInfo)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    int n = mplayInfo->trackCount;
    struct MusicPlayerTrack *track = mplayInfo->tracks;

    while (n-- > 0)
    {
        struct SoundChannel *chan;

        if (!(track->flags & MPT_FLG_EXIST))
            goto next;
        if (!(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
            goto next;

        TrkVolPitSet(mplayInfo, track);

        for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
        {
            if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
            {
                ClearChain(chan);
                continue;
            }

            if (track->flags & MPT_FLG_VOLCHG)
            {
                ChnVolSet(track, chan);
                if (chan->type & TONEDATA_TYPE_CGB)
                    ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
            }

            if (track->flags & MPT_FLG_PITCHG)
            {
                s32 pitch = (s32)chan->key + (s8)track->keyM;

                if (pitch < 0)
                    pitch = 0;
                if (chan->type & TONEDATA_TYPE_CGB)
                {
                    struct CgbChannel *cgb = (struct CgbChannel *)chan;
                    cgb->frequency = soundInfo->MidiKeyToCgbFreq(chan->type & TONEDATA_TYPE_CGB,
                                                                 (u8)pitch, track->pitM);
                    cgb->modify |= CGB_CHANNEL_MO_PIT;
                }
                else
                {
                    chan->frequency = MidiKeyToFreq(chan->wav, (u8)pitch, track->pitM);
                }
            }
        }

        // Leave only the persistent bits: START and EXIST. Every "something
        // changed" bit is consumed exactly once, so a change is never applied
        // twice.
        track->flags &= 0xF0;
next:   ;
        track++;
    }
}

// src/m4a_1.s:1129-1466. One frame of one music player.
//
// The tempo accumulator is the loop at the bottom: tempoC is topped up by
// tempoI every frame, and the track loop runs once per 150 counts. tempoI is
// `(tempoD * tempoU) >> 8`, so tempoD sets the tick length and tempoU scales it
// for fades and tempo changes.
void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    struct SoundInfo *soundInfo;

    if (mplayInfo->ident != ID_NUMBER)
        return;

    mplayInfo->ident++; // re-entrancy lock, released on every path below

    // Players are chained: each one's MPlayMainNext is the function that
    // advances the next player. The head is the only one SoundMain names, so
    // the whole list is serviced from here in one recursion.
    if (mplayInfo->MPlayMainNext != NULL)
        mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);

    // Gate 1: a paused player has a negative status and must not be touched.
    // MUSICPLAYER_STATUS_PAUSE is 0x80000000, so this is a signed test and a
    // paused player leaves with status, clock and tempoC untouched.
    if ((s32)mplayInfo->status < 0)
        goto done;

    FadeOutBody(mplayInfo);

    // Gate 2: FadeOutBody may have set PAUSE during the call above.
    if ((s32)mplayInfo->status < 0)
        goto done;

    soundInfo = SOUND_INFO_PTR;
    mplayInfo->tempoC = (u16)(mplayInfo->tempoC + mplayInfo->tempoI);

    while (mplayInfo->tempoC >= 150)
    {
        u32 mask = 0;
        u32 bit = 1;
        int n = mplayInfo->trackCount;
        struct MusicPlayerTrack *track = mplayInfo->tracks;

        while (n-- > 0)
        {
            struct SoundChannel *chan;

            if (!(track->flags & MPT_FLG_EXIST))
                goto next_track;

            mask |= bit;

            // Note gate: a channel with a gate time left stops being retriggered
            // when it expires, which is how a note is silenced before its
            // sample data runs out.
            for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
            {
                if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
                {
                    ClearChain(chan);
                    continue;
                }
                if (chan->gateTime != 0 && --chan->gateTime == 0)
                    chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
            }

            RunTrackCommands(mplayInfo, track);
next_track:
            bit <<= 1;
            track++;
        }

        mplayInfo->clock++;

        // MPT_FLG_EXIST, not MPT_FLG_START: m4aMPlayImmInit leaves a track as
        // EXIST alone after clearing START, and hardware keeps counting it as a
        // playing track.
        mplayInfo->status = mask != 0 ? mask : MUSICPLAYER_STATUS_PAUSE;

        mplayInfo->tempoC -= 150;
    }

    RunVolPit(mplayInfo);

done:
    mplayInfo->ident = ID_NUMBER;
}