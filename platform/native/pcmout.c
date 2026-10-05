// platform/native/pcmout.c
//
// SoundMain and the host side of the audio path.
//
// src/m4a_1.s's SoundMain sets the mixer up once, bumps soundInfo->ident as a
// lock against the concurrently running mixer task, and tail-calls a copy of
// itself that m4aSoundInit made in IWRAM. Here there is only one task, so the
// ident lock is taken and released in the same function and the mixer is just
// called directly -- but the shape is kept, because game code (MPlayOpen,
// m4aSoundMode, SoundClear, m4aSoundVSyncOn) all early-return while it is
// held and those are load-bearing.
//
// SoundMainRAM writes 8-bit signed stereo PCM into SoundInfo.pcmBuffer, which
// on hardware DMA1/DMA2 push into the two sound FIFOs. There is no FIFO here
// and no reason to emulate one: the mixer has just produced exactly the samples
// the DMA would have consumed this frame, so they are taken straight out of the
// buffer.
//
// The ring buffer is single-producer/single-consumer with no locks. The
// producer is the game's VBlank handler, the consumer is SDL's audio thread;
// they share only the two indices, and each is written by exactly one side.
// That matters because the producer runs inside the interrupt handler and must
// not block.
//
// A missing audio device is not an error. The binary has to run under
// SDL_VIDEODRIVER=offscreen on a build machine with no sound card and keep
// rendering frames; SDL_OpenAudioDevice failing leaves every call below a no-op
// and the ring simply stays empty.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "global.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "native.h"

// mixer.c
void NativeMixFrame(struct SoundInfo *si);

// --- ring buffer -----------------------------------------------------------

#define PCM_RING_FRAMES (1 << 16) // stereo frames; ~5 s at 13.4 kHz
#define PCM_RING_MASK (PCM_RING_FRAMES * 2 - 1)

static s16 sRing[PCM_RING_FRAMES * 2];
static u32 sWrite; // written only by the game thread
static u32 sRead;  // written only by the SDL audio thread

// --- WAV sink --------------------------------------------------------------
//
// Not debug scaffolding: this is how the port's audio is checked. The mixer is
// verified by measuring the samples the sink actually receives, and on a
// headless build there is no device to measure at all.

static FILE *sWav;
static u32 sWavFrames;
static u32 sWavRate;

static void WavWriteHeader(FILE *f, u32 frames)
{
    long dataBytes = (long)frames * 4;
    u8 hdr[44];

    memcpy(hdr + 0, "RIFF", 4);
    memcpy(hdr + 4, &(u32){(u32)(36 + dataBytes)}, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    memcpy(hdr + 16, &(u32){16}, 4);
    memcpy(hdr + 20, &(u16){1}, 2);  // PCM
    memcpy(hdr + 22, &(u16){2}, 2);  // stereo
    memcpy(hdr + 24, &sWavRate, 4);
    memcpy(hdr + 28, &(u32){sWavRate * 4}, 4);
    memcpy(hdr + 32, &(u16){4}, 2);  // block align
    memcpy(hdr + 34, &(u16){16}, 2); // bits per sample
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &(u32){dataBytes}, 4);

    fseek(f, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), f);
}

// --- SDL device ------------------------------------------------------------

static SDL_AudioDeviceID sDevice;
static u32 sDeviceRate; // 0 until the device has been opened (or refused)
static bool sDeviceTried;

static void AudioCallback(void *user, Uint8 *stream, int len)
{
    int frames = len / 4;
    int done = 0;

    (void)user;

    while (done < frames && sRead != sWrite)
    {
        u32 l = sRead * 2;
        u32 r = l + 1;

        stream[0] = (Uint8)(sRing[l & PCM_RING_MASK]);
        stream[1] = (Uint8)(sRing[l & PCM_RING_MASK] >> 8);
        stream[2] = (Uint8)(sRing[r & PCM_RING_MASK]);
        stream[3] = (Uint8)(sRing[r & PCM_RING_MASK] >> 8);
        stream += 4;
        sRead++;
        done++;
    }

    if (done < frames)
        memset(stream, 0, (size_t)(frames - done) * 4); // underrun
}

// The mixer produces samples at whatever rate the game is actually running at,
// which on a host that cannot hold 60 fps is not the GBA's 59.7. Opening the
// device at the rate the mixer was configured for keeps the two ends in
// agreement, even though real-time playback then runs slow. Resampling would
// fix the pitch but would also hide exactly the drift this path exposes.
static void OpenDeviceIfNeeded(u32 rate)
{
    SDL_AudioSpec want, have;

    if (sDeviceTried || rate == 0)
        return;
    sDeviceTried = true;

    memset(&want, 0, sizeof(want));
    want.freq = (int)rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = AudioCallback;
    want.userdata = NULL;

    sDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sDevice == 0)
    {
        // No sound card. Everything below stays a no-op and the game runs on.
        native_log("audio: no output device (%s), continuing without", SDL_GetError());
    }
    else
    {
        sDeviceRate = (u32)have.freq;
        SDL_PauseAudioDevice(sDevice, 0);
        native_log("audio: output at %d Hz, %d channels", have.freq, have.channels);
    }
}

// Staging buffer for the 16-bit conversion. Static because PushPcm runs inside
// the VBlank interrupt handler, where taking the allocator's lock would be a
// bad idea; PCM_DMA_BUF_SIZE is the largest `samples` can be.
static s16 sStage[PCM_DMA_BUF_SIZE * 2];

static void PushPcm(const struct SoundInfo *si, u32 offset, u32 samples)
{
    const s8 *right = (const s8 *)si->pcmBuffer + offset;
    const s8 *left = (const s8 *)si->pcmBuffer + offset + PCM_DMA_BUF_SIZE;
    s16 *dst;
    u32 n;

    if (samples == 0 || samples > PCM_DMA_BUF_SIZE)
        return;

    // pcmBuffer's first half is the right speaker: it is what DMA1/FIFO_A,
    // configured SOUND_A_RIGHT_OUTPUT, reads.
    dst = sStage;

    for (n = 0; n < samples; n++)
    {
        dst[n * 2 + 0] = (s16)((s32)right[n] << 8);
        dst[n * 2 + 1] = (s16)((s32)left[n] << 8);
    }

    if (sWav != NULL)
        fwrite(dst, sizeof(s16), samples * 2, sWav);
    sWavFrames += samples;

    if (sDevice != 0 && PCM_RING_FRAMES - (sWrite - sRead) >= samples)
    {
        memcpy(&sRing[(sWrite * 2) & PCM_RING_MASK], dst, samples * 2 * sizeof(s16));
        sWrite += samples;
    }
    // Otherwise the host is behind and this frame is dropped: the interrupt
    // handler that called us cannot afford to block.
}

// --- public API ------------------------------------------------------------

// Called from native_main before the game starts. `wav_path` may be NULL.
void native_audio_init(const char *wav_path)
{
    if (wav_path != NULL)
    {
        sWav = fopen(wav_path, "wb");
        if (sWav == NULL)
            native_log("audio: cannot write %s", wav_path);
    }

    // video.c only brings up the video subsystem, so SDL_OpenAudioDevice would
    // otherwise fail with "Audio subsystem is not initialized" on a machine
    // that does have a sound card.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        native_log("audio: SDL_InitSubSystem(audio) failed: %s", SDL_GetError());
}

// Called from native_main's exit path. Finalises the WAV header, if any.
void native_audio_shutdown(void)
{
    if (sDevice != 0)
    {
        SDL_CloseAudioDevice(sDevice);
        sDevice = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    if (sWav != NULL)
    {
        WavWriteHeader(sWav, sWavFrames);
        fclose(sWav);
        sWav = NULL;
        native_log("audio: wrote %u frames at %u Hz", sWavFrames, sWavRate);
    }
}

void SoundMain(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    u32 samples;
    u32 offset = 0;

    // Same guard the ARM version uses: run only once m4aSoundInit has published
    // a SoundInfo and identified it.
    if (soundInfo->ident != ID_NUMBER)
        return;

    soundInfo->ident++;

    // The ARM version calls MPlayMain on the head player only and recurses
    // through the list from inside MPlayMain, so every player is serviced.
    // Channel stealing in ply_note compares track pointers, which makes visit
    // order observable: walking the list here instead would visit newest first
    // and hand out channels differently.
    if (soundInfo->MPlayMainHead != NULL)
        soundInfo->MPlayMainHead(soundInfo->musicPlayerHead);

    // Drives the four legacy channels' NRxx registers, which mixer.c emulates.
    soundInfo->CgbSound();

    samples = (u32)soundInfo->pcmSamplesPerVBlank;

    // Where the mixer is writing this frame, and therefore where these samples
    // are. Same expression the ARM SoundMain uses for its write head.
    if (soundInfo->pcmDmaCounter > 1)
        offset = samples * (soundInfo->pcmDmaPeriod - (soundInfo->pcmDmaCounter - 1));

    NativeMixFrame(soundInfo);

    if (sWav != NULL && sWavRate == 0)
        sWavRate = (u32)soundInfo->pcmFreq;

    OpenDeviceIfNeeded((u32)soundInfo->pcmFreq);
    PushPcm(soundInfo, offset, samples);

    soundInfo->ident = ID_NUMBER;
}

// src/m4a_1.s:1070-1127. Runs on the VCount interrupt, ahead of VBlank's
// m4aSoundMain. It keeps the PCM DMA cadence -- which is what moves the mixer's
// write head -- and restarts the FIFOs.
//
// The DMA reprogramming is dropped: there is no FIFO to feed. The counter is
// not, because SoundMainRAM's buffer offset is computed from it.
void m4aSoundVSync(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    u32 ident = soundInfo->ident;
    u8 counter;

    if (ident != ID_NUMBER && ident != ID_NUMBER + 1)
        return;

    // The original stores the decremented byte first and then branches on the
    // *signed* result, so a counter of 0 or 1 (1 underflows to -1) reloads
    // from pcmDmaPeriod and the rest simply counts down. Reproducing the store
    // before the test matters: it is why the counter ends up cycling 7..1 on a
    // 13379 Hz stream rather than 6..0.
    counter = soundInfo->pcmDmaCounter;
    soundInfo->pcmDmaCounter = (u8)(counter - 1);
    if (counter <= 1)
        soundInfo->pcmDmaCounter = soundInfo->pcmDmaPeriod;
}