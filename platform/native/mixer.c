// platform/native/mixer.c
//
// C port of SoundMainRAM (src/m4a_1.s:88-666) plus a small Game Boy APU.
//
// src/m4a_1.s is ARM assembly and cannot run on a host, so the whole direct
// sound mixer is reimplemented here. It writes exactly the bytes the ARM
// version writes into SoundInfo.pcmBuffer -- the same buffer src/m4a.c points
// DMA1/DMA2 at -- and platform/native/pcmout.c drains that into SDL.
//
// Several ARM tricks here *are* the algorithm, so they are transcribed rather
// than tidied:
//
//   * The per-sample accumulation runs through a 32-bit register holding four
//     output bytes. Each sample contributes to the top byte and the register is
//     rotated right by eight, so after four samples the word is back in output
//     order -- and after four *channels* have each done that, the rotations
//     cancel and the word holds their per-byte sum. That is how the original
//     gets 8- (or 12-) way summation into a byte stream. The rotation is kept
//     here because MixAcc below has to match it exactly.
//
//   * The channel mixer runs a linear interpolator at (divFreq * frequency)
//     per output sample in Q9.23 fixed point and re-reads a *pair* of source
//     samples whenever the accumulator crosses a whole sample, so pitch costs
//     two fetches per step. Linear rather than the hardware's cubic.
//
// The CGB channels are a different problem. m4a's CgbSound (src/m4a.c, already
// C) computes envelope, sweep, duty, length and frequency in software and then
// *writes them to the NRxx hardware registers*; on hardware the four legacy
// channels are produced by the sound chip, which a host does not have. So the
// registers are emulated here: a compact pulse/sweep, wave and noise generator
// clocked at the PCM rate. Almost every sound effect in the game is square or
// noise, so without this the port would play music and drop every effect.
//
// Two deviations from the ARM original, both deliberate:
//
//   * SoundMainRAM_Unk1's sample-end path (m4a_1.s:365-375) is transcribed in
//     the GBA SDK as a loop that adds `soundInfo` to a signed count until the
//     result is positive. On a 64-bit host that can run for 2^32 iterations and
//     it indexes a line budget as though it were a wave pointer. It is a
//     loop-wrap that lost its operands; this port does the wrap the
//     surrounding code plainly means.
//
//   * The ARM tail loop emits N samples into a 4-byte slot and the rotate
//     places them at offsets 4-N..3 rather than 0..N-1, delaying the last few
//     samples of every note by up to three output samples. The tail here still
//     uses the rotate (it has to, to match MixAcc) but emits them at the right
//     offsets; by then they are in release and the difference is inaudible.

#include "global.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "native.h"

// Not in include/gba/m4a_internal.h, which carries only the ones the C code
// needs. From constants/m4a_constants.inc.
#define TONEDATA_TYPE_FIX 0x08
#define TONEDATA_TYPE_REV 0x10
#define TONEDATA_TYPE_CMP 0x20
#define WAVE_DATA_FLAG_LOOP 0xC0
#define SOUND_CHANNEL_SF_SPECIAL 0x20

// Defined in src/m4a_tables.c but absent from include/gba/m4a_internal.h.
extern const s8 gDeltaEncodingTable[];

// ---------------------------------------------------------------------------
// Fixed-point helpers
// ---------------------------------------------------------------------------

static inline u32 Ror8(u32 v)
{
    return (v >> 8) | (v << 24);
}

// One sample into one 32-bit output word, as m4a_1.s:329-333.
//
// `mul r1, r10, r0; bic r1, r1, 0xFF0000` keeps only the top byte of the
// product, so a sample scaled by its envelope volume becomes an 8-bit value in
// bits 24..31 -- and, because the product wraps, a negative sample arrives
// biased to 0x80, which is exactly the signed 8-bit PCM the DMA reads back.
static inline u32 MixAcc(u32 acc, s32 sample, u32 vol)
{
    u32 t = (vol * (u32)sample) & 0xFF00FFFFu;
    return t + Ror8(acc);
}

// ---------------------------------------------------------------------------
// DPCM decode
// ---------------------------------------------------------------------------

// src/m4a_1.s:670-709. A compressed wave holds 0x40 samples per 0x21 bytes,
// delta-coded through gDeltaEncodingTable; SoundMainRAM decodes one block at a
// time and indexes it by (pointer & 0x3F). `index` is a sample index, not an
// address: block = index >> 6, and the block's bytes start at wav->data +
// 0x21 * block.
static s8 sDecodingBuffer[0x40];

static s32 DecodeDPCM(struct SoundChannel *chan, u32 index)
{
    u32 block = index >> 6;

    if (block != chan->xpi)
    {
        const u8 *src = (const u8 *)chan->wav->data + 0x21 * block;
        s32 cur = src[0];
        u32 i;

        sDecodingBuffer[0] = (s8)cur;

        // One source byte carries two samples: the low nibble of src[n] first,
        // then the high nibble of src[n+1]. 0x40 samples come out of 0x21
        // bytes. (The ARM loop writes one sample past the end of its 0x40-byte
        // buffer and relies on the extra IWRAM byte; here the read side is
        // masked to 0x3F anyway, so the store is simply left off.)
        for (i = 1; i < 0x40; i++)
        {
            u8 byte = src[(i + 1) / 2];

            cur += gDeltaEncodingTable[(i & 1) ? (byte & 0xF) : (byte >> 4)];
            sDecodingBuffer[i] = (s8)cur;
        }
        chan->xpi = (u16)block;
    }

    return sDecodingBuffer[index & 0x3F];
}

// ---------------------------------------------------------------------------
// Channel mixing
// ---------------------------------------------------------------------------

struct LoopInfo
{
    u32 size;
    // The ARM original precomputes `wav->data + wav->loopStart` once per
    // channel per frame and jumps straight to it on the seam, so it is an
    // absolute address for a plain wave.
    s8 *start;
    // A DPCM wave addresses the compressed stream by sample index instead, and
    // for that stream the seam is the raw loopStart value.
    u32 startIndex;
};

static void GetLoopInfo(struct SoundChannel *chan, struct LoopInfo *loop)
{
    loop->size = 0;
    loop->start = NULL;
    loop->startIndex = 0;

    if (chan->statusFlags & SOUND_CHANNEL_SF_LOOP)
    {
        loop->size = chan->wav->size - chan->wav->loopStart;
        loop->start = (s8 *)chan->wav->data + chan->wav->loopStart;
        loop->startIndex = chan->wav->loopStart;
    }
}

// Reads one source sample `off` positions from the channel's anchor.
//
// A plain wave anchors on a byte pointer; a DPCM wave anchors on a sample index
// into the compressed stream, because that is what DecodeDPCM wants. Reverse
// waves walk backwards, so `off` is signed.
static inline s32 FetchAt(struct SoundChannel *chan, const s8 *base, u32 baseIdx,
                          s32 off, bool dpcm)
{
    if (dpcm)
        return DecodeDPCM(chan, (u32)((s32)baseIdx + off));
    return base[off];
}

// The shared interpolator (m4a_1.s:397-435 for plain waves, :514-554 for DPCM,
// :575-614 and :615-652 for the two reverse variants).
//
// `pos` is a signed offset from the anchor, and the anchor is saved across the
// call so the final currentPointer can be rebuilt from it -- which is what
// lets a reverse wave resume mid-sample on the next frame without a second
// cursor.
static void MixInterpolated(struct SoundChannel *chan, struct LoopInfo *loop, bool dpcm, bool reverse,
                            u32 divFreq, u32 volR, u32 volL, u32 samples,
                            u32 **prBuf, u32 **plBuf)
{
    s8 *base = chan->currentPointer;
    u32 baseIdx = dpcm ? (u32)(uintptr_t)base : 0;
    u32 *rBuf = *prBuf;
    u32 *lBuf = *plBuf;
    u32 step = divFreq * chan->frequency;
    u32 fw = chan->fw;
    s32 pos = 0;
    s32 count = (s32)chan->count;
    s32 dir = reverse ? -1 : 1;
    s32 sample = FetchAt(chan, base, baseIdx, 0, dpcm);
    s32 delta = FetchAt(chan, base, baseIdx, dir, dpcm) - sample;
    bool dead = false;

    while (samples > 0)
    {
        u32 accR = *rBuf;
        u32 accL = *lBuf;
        u32 n = 0;

        while (n < 4 && samples > 0)
        {
            // Arithmetic shift: the ARM does `add lr, r0, lr, asr 23`, and a
            // logical shift here turns every negative delta into a large
            // positive jump rather than a small negative one.
            s32 v = sample + ((s32)(fw * (u32)delta) >> 23);

            accR = MixAcc(accR, v, volR);
            accL = MixAcc(accL, v, volL);
            n++;
            samples--;

            fw += step;
            u32 advance = fw >> 23;

            if (advance != 0)
            {
                // `bic r9, r9, #0x3F800000` is AND-NOT: it drops the whole
                // 9-bit integer part (bits 22..30) and keeps the 22-bit
                // fraction plus the carry bit. ANDing with 0x3F800000 instead
                // would keep the integer part and drop the fraction, so the
                // phase would never accumulate and every sample would
                // interpolate a full step ahead.
                fw &= ~0x3F800000u;
                count -= (s32)advance;

                if (count <= 0)
                {
                    if (loop->size == 0)
                    {
                        dead = true;
                        break;
                    }
                    // Wrap. The overshoot is dropped rather than carried into
                    // the loop: the pitch phase (fw) is continuous, so the only
                    // artefact is the seam itself.
                    count = (s32)loop->size;
                    pos = 0;
                    if (dpcm)
                    {
                        baseIdx = loop->startIndex;
                        base = (s8 *)(uintptr_t)baseIdx;
                    }
                    else if (reverse)
                    {
                        // A reverse wave restarts at the end of its loop.
                        base = (s8 *)chan->wav->data + chan->wav->size;
                    }
                    else
                    {
                        base = loop->start;
                    }
                    sample = FetchAt(chan, base, baseIdx, pos, dpcm);
                    delta = FetchAt(chan, base, baseIdx, pos + dir, dpcm) - sample;
                    continue;
                }

                if (advance == 1)
                {
                    // Stepping exactly one sample still re-reads the next pair:
                    // the ARM's advance==1 case falls through to the same
                    // `ldrsb r1, [r3, #1]!; sub r1, r1, r0` that the multi-step
                    // case uses, so delta tracks the new slope. Without it the
                    // interpolator extrapolates the first delta forever and the
                    // waveform becomes a ramp.
                    pos += dir;
                    sample += delta;
                    delta = FetchAt(chan, base, baseIdx, pos + dir, dpcm) - sample;
                }
                else
                {
                    pos += dir * (s32)advance;
                    sample = FetchAt(chan, base, baseIdx, pos, dpcm);
                    delta = FetchAt(chan, base, baseIdx, pos + dir, dpcm) - sample;
                }
            }
        }

        *rBuf++ = accR;
        *lBuf++ = accL;

        // The wave ran out mid-word. The ARM original stops the channel here
        // rather than carrying on into the next word, which would read past the
        // end of the sample data.
        if (dead)
            break;
    }

    *prBuf = rBuf;
    *plBuf = lBuf;
    chan->count = dead ? 0 : (u32)count;
    chan->currentPointer = dpcm ? (s8 *)(uintptr_t)((s32)baseIdx + pos) : base + pos;
    chan->fw = fw;
    if (dead)
        chan->statusFlags = 0;
}

// TONEDATA_TYPE_FIX: the wave is already at the output rate, so one source byte
// per output sample and no interpolation (m4a_1.s:313-344).
static void MixFixed(struct SoundChannel *chan, struct LoopInfo *loop, u32 volR, u32 volL,
                     u32 samples, u32 **prBuf, u32 **plBuf)
{
    s8 *base = chan->currentPointer;
    u32 *rBuf = *prBuf;
    u32 *lBuf = *plBuf;
    u32 pos = 0;
    s32 count = (s32)chan->count;
    bool dead = false;

    while (samples > 0)
    {
        u32 accR = *rBuf;
        u32 accL = *lBuf;
        u32 n = 0;

        while (n < 4 && count > 0 && samples > 0)
        {
            s32 s = base[pos + n];
            accR = MixAcc(accR, s, volR);
            accL = MixAcc(accL, s, volL);
            n++;
            samples--;
            count--;
        }

        // The ARM tail writes the whole word even when it only filled part of
        // it, which is what the rotate above expects; the samples it did fill
        // sit at offsets 4-n..3 there rather than 0..n-1. Shift them down so
        // they land on the output samples they belong to.
        if (n < 4)
        {
            accR = (accR >> ((4 - n) * 8));
            accL = (accL >> ((4 - n) * 8));
        }

        *rBuf++ = accR;
        *lBuf++ = accL;
        pos += n;

        if (count == 0)
        {
            if (loop->size == 0)
            {
                dead = true;
                break;
            }
            count = (s32)loop->size;
            base = loop->start;
            pos = 0;
        }
    }

    *prBuf = rBuf;
    *plBuf = lBuf;
    chan->count = dead ? 0 : (u32)count;
    chan->currentPointer = base + pos;
    if (dead)
        chan->statusFlags = 0;
}

// ---------------------------------------------------------------------------
// Game Boy APU (channels 1-4)
// ---------------------------------------------------------------------------

// m4a's CgbSound writes final envelope, sweep, duty, length and pitch into the
// NRxx registers every time it touches them. Emulating the registers rather
// than re-deriving the values means this file never has to know what an m4a
// envelope means, only what a Game Boy one does.
//
// The hardware envelope generator is deliberately not emulated: CgbSound
// overwrites NR12/NR32 with the volume it computed, so the high nibble is
// always current. What is emulated is the oscillator, the sweep, the length
// counter and the DAC gates.
struct ApuPulse
{
    u32 phase;  // Q24 within one period
    u32 step;   // Q24 phase advance per output sample
    u16 period;
    u16 sweepShadow;
    u8 sweepPeriod;
    u8 sweepPeriodMax;
    u8 sweepShift;
    bool sweepNeg;
    bool enabled;
    bool sweepOn;
    u8 duty;
    u8 volume;
    u8 length;
    u8 lengthCounter;
};

struct ApuWave
{
    u32 phase;
    u32 step;
    u8 volume;
    u8 ram[16];
    bool enabled;
    u8 sample;
    u8 length;
    u8 lengthCounter;
};

struct ApuNoise
{
    u32 phase;
    u32 step;
    u16 lfsr;
    u8 periodIdx;
    u8 shift;
    bool width7;
    bool enabled;
    u8 length;
    u8 lengthCounter;
    u8 volume;
};

struct Apu
{
    struct ApuPulse ch1, ch2;
    struct ApuWave ch3;
    struct ApuNoise ch4;
    u32 lenDiv;   // fractional 256 Hz tick (the length counters)
    u32 sweepDiv; // fractional 128 Hz tick (channel 1's sweep)
    u32 rate;
    bool started;
};

static struct Apu sApu;

// GB duty patterns, MSB first over the eight eighths of a period.
static const u8 sDutyTable[4] = { 0x01, 0x81, 0x87, 0x7E };

static const u8 sNoiseDivisors[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

// NR32 only has three bits, so m4a quantises its wave envelope through
// gCgb3Vol. Codes 1..4 are the four monotonically increasing steps m4a uses
// (12.5% / 25% / 50% / 100% of full scale); 5..7 are their aliases.
static const u8 sWaveVolume[8] = { 0, 2, 4, 8, 15, 4, 8, 15 };

// CgbSound only writes the wave RAM when the sample bank changes, so it is
// re-read here every frame; 16 bytes is cheaper than tracking the pointer.
static void ApuLoadWaveRam(void)
{
    const u32 *src = (const u32 *)&REG_WAVE_RAM0;
    u8 *dst = sApu.ch3.ram;
    int i;

    for (i = 0; i < 4; i++)
    {
        u32 word = src[i];
        dst[i * 4 + 0] = (u8)(word & 0xFF);
        dst[i * 4 + 1] = (u8)((word >> 8) & 0xFF);
        dst[i * 4 + 2] = (u8)((word >> 16) & 0xFF);
        dst[i * 4 + 3] = (u8)((word >> 24) & 0xFF);
    }
}

static void ApuUpdateRegisters(const struct CgbChannel *cgb, u32 sampleRate)
{
    struct Apu *apu = &sApu;
    // m4a sets statusFlags to exactly SOUND_CHANNEL_SF_ENV_ATTACK when it
    // starts a note and never returns to that value afterwards (the phases run
    // down 3 -> 2 -> 1 -> 0), so it is an exact "a note just began" signal.
    //
    // Detecting a trigger off NRx4's bit 7 instead does not work: CgbSound
    // leaves that bit set after writing it, so the next note's write is not an
    // edge and the oscillator would never restart.
    bool trig1 = cgb != NULL && cgb[0].statusFlags == SOUND_CHANNEL_SF_ENV_ATTACK;
    bool trig2 = cgb != NULL && cgb[1].statusFlags == SOUND_CHANNEL_SF_ENV_ATTACK;
    bool trig3 = cgb != NULL && cgb[2].statusFlags == SOUND_CHANNEL_SF_ENV_ATTACK;
    bool trig4 = cgb != NULL && cgb[3].statusFlags == SOUND_CHANNEL_SF_ENV_ATTACK;

    // The length and sweep tick dividers below subtract `rate` from a running
    // total; a zero rate would spin forever.
    if (sampleRate == 0)
        sampleRate = 1;
    u8 nr10 = REG_NR10, nr11 = REG_NR11, nr12 = REG_NR12, nr13 = REG_NR13, nr14 = REG_NR14;
    u8 nr21 = REG_NR21, nr22 = REG_NR22, nr23 = REG_NR23, nr24 = REG_NR24;
    u8 nr30 = REG_NR30, nr31 = REG_NR31, nr32 = REG_NR32, nr33 = REG_NR33, nr34 = REG_NR34;
    u8 nr41 = REG_NR41, nr42 = REG_NR42, nr43 = REG_NR43, nr44 = REG_NR44;
    u32 period;

    (void)nr44; // triggers come from the CgbChannel, not from NRx4

    if (trig1)
    {
        apu->ch1.phase = 0;
        if (apu->ch1.length)
            apu->ch1.lengthCounter = (u8)(64 - apu->ch1.length);
    }
    apu->ch1.duty = (u8)(nr11 >> 6);
    apu->ch1.volume = (u8)(nr12 >> 4);
    apu->ch1.period = (u16)(2048 - (nr13 | ((nr14 & 7) << 8)));
    apu->ch1.enabled = (nr12 & 0xF8) != 0;
    if (nr11 & 0x40)
        apu->ch1.length = (u8)(64 - (nr11 & 0x3F));
    apu->ch1.sweepPeriodMax = (u8)(nr10 >> 4);
    apu->ch1.sweepShift = (u8)(nr10 & 7);
    apu->ch1.sweepNeg = (nr10 & 8) != 0;
    apu->ch1.sweepShadow = (u16)(nr13 | ((nr10 & 7) << 8));

    if (trig2)
    {
        apu->ch2.phase = 0;
        if (apu->ch2.length)
            apu->ch2.lengthCounter = (u8)(64 - apu->ch2.length);
    }
    apu->ch2.duty = (u8)(nr21 >> 6);
    apu->ch2.volume = (u8)(nr22 >> 4);
    apu->ch2.period = (u16)(2048 - (nr23 | ((nr24 & 7) << 8)));
    apu->ch2.enabled = (nr22 & 0xF8) != 0;
    if (nr21 & 0x40)
        apu->ch2.length = (u8)(64 - (nr21 & 0x3F));

    if (trig3)
    {
        apu->ch3.phase = 0;
        apu->ch3.sample = 0;
        if (apu->ch3.length)
            apu->ch3.lengthCounter = (u8)(256 - apu->ch3.length);
    }
    apu->ch3.enabled = (nr30 & 0x80) != 0;
    apu->ch3.volume = sWaveVolume[(nr32 >> 5) & 7];
    if (nr31 & 0x40)
        apu->ch3.length = (u8)(256 - (nr31 & 0xFF));

    if (trig4)
    {
        apu->ch4.phase = 0;
        if (apu->ch4.length)
            apu->ch4.lengthCounter = (u8)(64 - apu->ch4.length);
    }
    apu->ch4.volume = (u8)(nr42 >> 4);
    apu->ch4.enabled = (nr42 & 0xF8) != 0;
    apu->ch4.periodIdx = (u8)(nr43 & 7);
    apu->ch4.shift = (u8)((nr43 >> 4) & 0xF);
    apu->ch4.width7 = (nr43 & 8) != 0;
    if (nr41 & 0x40)
        apu->ch4.length = (u8)(64 - (nr41 & 0x3F));

    apu->rate = sampleRate;

    if (apu->ch3.enabled)
        ApuLoadWaveRam();

    // Phase increments, in Q24 units of one period per output sample.
    period = apu->ch1.period ? apu->ch1.period : 2048;
    apu->ch1.step = (u32)(((u64)131072 << 24) / ((u64)sampleRate * period));
    period = apu->ch2.period ? apu->ch2.period : 2048;
    apu->ch2.step = (u32)(((u64)131072 << 24) / ((u64)sampleRate * period));

    // The wave channel plays 32 samples per period, so it runs twice as fast
    // as a pulse channel at the same period register value.
    period = 2048 - (nr33 | ((nr34 & 7) << 8));
    if (period == 0)
        period = 2048;
    apu->ch3.step = (u32)(((u64)262144 << 24) / ((u64)sampleRate * period));

    {
        u64 noiseHz = apu->ch4.periodIdx
                          ? ((u64)262144 / ((u64)sNoiseDivisors[apu->ch4.periodIdx] << apu->ch4.shift))
                          : ((u64)524288 >> apu->ch4.shift);
        apu->ch4.step = noiseHz ? (u32)(((u64)sampleRate << 24) / noiseHz) : 0;
    }
}

static void ApuClockLengths(void)
{
    struct Apu *apu = &sApu;

    if (apu->ch1.lengthCounter && --apu->ch1.lengthCounter == 0)
        apu->ch1.enabled = false;
    if (apu->ch2.lengthCounter && --apu->ch2.lengthCounter == 0)
        apu->ch2.enabled = false;
    if (apu->ch3.lengthCounter && --apu->ch3.lengthCounter == 0)
        apu->ch3.enabled = false;
    if (apu->ch4.lengthCounter && --apu->ch4.lengthCounter == 0)
        apu->ch4.enabled = false;
}

// One sweep step. Channel 1's result is written back to its period register;
// channel 2 has a sweep register but no frequency write-back, so its result is
// computed and discarded, exactly as on hardware.
static void ApuClockSweep(struct ApuPulse *p, bool writeBack)
{
    if (p->sweepPeriodMax == 0)
    {
        p->sweepPeriod = 0;
        return;
    }
    if (p->sweepPeriod == 0)
        p->sweepPeriod = p->sweepPeriodMax;

    if (p->sweepShadow > p->period)
    {
        u32 next = p->sweepShift ? (u32)(p->sweepShadow >> p->sweepShift) : p->sweepShadow;

        if (p->sweepNeg)
            next = p->sweepShadow - next;
        if (next > 2047)
            p->enabled = false;
        else if (writeBack)
            p->period = (u16)next;
    }
    if (--p->sweepPeriod == 0)
        p->sweepPeriod = p->sweepPeriodMax;
}

// Advance one output sample, returning the level per side after the NR50 master
// volume and the NR51 panning bits.
static void ApuRender(s32 *outL, s32 *outR)
{
    struct Apu *apu = &sApu;
    u32 nr51 = REG_NR51;
    u32 nr50 = REG_NR50;
    s32 level[4];
    s32 sumL = 0, sumR = 0;
    int i;

    // Length counters run at 256 Hz and the sweep at 128 Hz, independently of
    // the output rate. Getting this wrong makes every gated note last several
    // times too long, because m4a sets NRx1/NR4's length-enable bits and lets
    // the hardware length counter end the note.
    apu->lenDiv += 256;
    while (apu->lenDiv >= apu->rate)
    {
        apu->lenDiv -= apu->rate;
        ApuClockLengths();
    }
    apu->sweepDiv += 128;
    while (apu->sweepDiv >= apu->rate)
    {
        apu->sweepDiv -= apu->rate;
        ApuClockSweep(&apu->ch1, true);
        ApuClockSweep(&apu->ch2, false);
    }

    for (i = 0; i < 2; i++)
    {
        struct ApuPulse *p = (i == 0) ? &apu->ch1 : &apu->ch2;

        p->phase += p->step;
        while (p->phase >= (1u << 24))
            p->phase -= (1u << 24);
        if (p->phase >= (1u << 24))
            p->phase = 0;
        level[i] = (p->enabled && (sDutyTable[p->duty] & (1u << (7 - ((p->phase >> 21) & 7)))))
                       ? p->volume
                       : 0;
    }

    {
        struct ApuWave *w = &apu->ch3;

        w->phase += w->step;
        while (w->phase >= (1u << 24))
        {
            w->phase -= (1u << 24);
            w->sample = (u8)((w->sample + 1) & 31);
        }
        level[2] = w->enabled ? (s32)(((w->ram[w->sample >> 1] >> ((w->sample & 1) ? 0 : 4)) & 0xF)
                                       * w->volume / 15)
                              : 0;
    }

    {
        struct ApuNoise *n = &apu->ch4;

        n->phase += n->step;
        while (n->phase >= (1u << 24))
        {
            u16 bit = (u16)((n->lfsr ^ (n->lfsr >> 1)) & 1);

            n->phase -= (1u << 24);
            n->lfsr = (u16)(n->lfsr >> 1);
            n->lfsr = (u16)(n->lfsr | (bit << 14));
            if (n->width7)
                n->lfsr = (u16)((n->lfsr & ~0x40) | (bit << 6));
        }
        level[3] = (n->enabled && !(n->lfsr & 1)) ? n->volume : 0;
    }

    for (i = 0; i < 4; i++)
    {
        if (nr51 & (1u << i))
            sumR += level[i];
        if (nr51 & (1u << (i + 4)))
            sumL += level[i];
    }

    *outL = sumL * (s32)((nr50 >> 4) & 7);
    *outR = sumR * (s32)(nr50 & 7);
}

// ---------------------------------------------------------------------------
// SoundMainRAM
// ---------------------------------------------------------------------------

// Envelope update, m4a_1.s:171-292. Returns false if the channel went silent
// this call; otherwise `volR`/`volL` come back ready to scale samples with.
//
// The phases live in the low two bits of statusFlags and step *down* on each
// transition (attack 3 -> decay 2 -> sustain 1 -> release 0), which is why
// `flags--` appears where it does. SF_IEC is m4a's pseudo-echo: once the
// release has reached its floor the volume is parked at pseudoEchoVolume and
// counted down rather than cut to silence.
static bool UpdateEnvelope(struct SoundChannel *chan, struct SoundInfo *si,
                           u32 *volR, u32 *volL)
{
    u8 flags = chan->statusFlags;

    if (flags & SOUND_CHANNEL_SF_START)
    {
        if (flags & SOUND_CHANNEL_SF_STOP)
        {
            chan->statusFlags = 0;
            return false;
        }

        // A fresh note: latch the wave start, zero the envelope and the
        // interpolation phase, and honour the wave's loop flag. The ARM
        // original then falls straight through into its attack increment, so
        // the first frame mixes at `attack` rather than at silence.
        flags = SOUND_CHANNEL_SF_ENV_ATTACK;
        chan->currentPointer = (s8 *)chan->wav->data + chan->count;
        chan->count = chan->wav->size - chan->count;
        chan->envelopeVolume = 0;
        chan->fw = 0;
        if (chan->wav->status & WAVE_DATA_FLAG_LOOP)
            flags |= SOUND_CHANNEL_SF_LOOP;
        chan->statusFlags = flags;
    }

    {
        s32 vol = chan->envelopeVolume;

        if (flags & SOUND_CHANNEL_SF_IEC)
        {
            if (--chan->pseudoEchoLength == 0)
            {
                chan->statusFlags = 0;
                return false;
            }
        }
        else if (flags & SOUND_CHANNEL_SF_STOP)
        {
            vol = (s32)(((u32)vol * chan->release) >> 8);
            if (vol <= chan->pseudoEchoVolume)
            {
                if (chan->pseudoEchoVolume == 0)
                {
                    chan->statusFlags = 0;
                    return false;
                }
                flags |= SOUND_CHANNEL_SF_IEC;
                chan->statusFlags = flags;
                vol = chan->pseudoEchoVolume;
            }
        }
        else
        {
            u8 env = flags & SOUND_CHANNEL_SF_ENV;

            if (env == SOUND_CHANNEL_SF_ENV_DECAY)
            {
                vol = (s32)(((u32)vol * chan->decay) >> 8);
                if (vol <= chan->sustain)
                {
                    if (chan->sustain == 0)
                    {
                        if (chan->pseudoEchoVolume == 0)
                        {
                            chan->statusFlags = 0;
                            return false;
                        }
                        flags |= SOUND_CHANNEL_SF_IEC;
                        chan->statusFlags = flags;
                        vol = chan->pseudoEchoVolume;
                    }
                    else
                    {
                        flags--; // decay -> sustain
                        chan->statusFlags = flags;
                        vol = chan->sustain;
                    }
                }
            }
            else if (env == SOUND_CHANNEL_SF_ENV_ATTACK)
            {
                vol += chan->attack;
                if (vol >= 0xFF)
                {
                    vol = 0xFF;
                    flags--; // attack -> decay
                    chan->statusFlags = flags;
                }
            }
            // SOUND_CHANNEL_SF_ENV_SUSTAIN holds the level.
        }

        chan->envelopeVolume = (u8)vol;

        // The ARM original reads soundInfo->masterVolume through an offset that
        // is also SoundChannel::release; the shared master volume is what it
        // means. masterVolume is 15, so this is a no-op that scales to whatever
        // m4aSoundMode last set.
        vol = (s32)(((u32)vol * (si->masterVolume + 1)) >> 4);
        chan->envelopeVolumeRight = (u8)((vol * chan->rightVolume) >> 8);
        chan->envelopeVolumeLeft = (u8)((vol * chan->leftVolume) >> 8);
        *volR = (u32)chan->envelopeVolumeRight << 16;
        *volL = (u32)chan->envelopeVolumeLeft << 16;
    }

    return true;
}

// The reverb pass, m4a_1.s:96-118. Only runs when the song asked for it.
static void ApplyReverb(struct SoundInfo *si, s8 *buf, u32 samples, u32 offset)
{
    s32 reverb = si->reverb;
    // The original points the echo read head one buffer's worth ahead of the
    // write head, except when maxLines says there is no room for a delay.
    s8 *echo = (si->maxLines == 2) ? si->pcmBuffer : (s8 *)buf + offset + samples;
    u32 i;

    for (i = 0; i < samples; i++)
    {
        s32 acc = (s32)buf[offset + i]
                + (s32)buf[offset + PCM_DMA_BUF_SIZE + i]
                + (s32)echo[i]
                + (s32)echo[PCM_DMA_BUF_SIZE + i];
        s32 out = (acc * reverb) >> 9;

        if (out < 0)
            out++; // the original's `addne r0, r0, #1`: round toward zero
        buf[offset + PCM_DMA_BUF_SIZE + i] = (s8)out;
        buf[offset + i] = (s8)out;
    }
}

// Entry point, called once per frame from SoundMain (platform/native/pcmout.c).
void NativeMixFrame(struct SoundInfo *si)
{
    s8 *buf = (s8 *)si->pcmBuffer;
    u32 samples = (u32)si->pcmSamplesPerVBlank;
    u32 offset = 0;
    u32 line;
    u32 lineBudget;
    int maxChans = si->maxChans;
    int i;

    if (!sApu.started)
    {
        memset(&sApu, 0, sizeof(sApu));
        sApu.ch4.lfsr = 0x7FFF;
        sApu.started = true;
    }

    // The write head walks forward through the buffer so the DMA, which is
    // always pcmDmaPeriod VBlanks behind, never overwrites samples the mixer has
    // not read yet. Only `samples` bytes of each half are live; the rest is the
    // read head's backlog.
    if (si->pcmDmaCounter > 1)
        offset = samples * (si->pcmDmaPeriod - (si->pcmDmaCounter - 1));

    // maxLines caps how many channels get mixed, in units of scanlines.
    line = native_vcount();
    if (line < NATIVE_VBLANK_START)
        line += NATIVE_TOTAL_SCANLINES;
    lineBudget = si->maxLines ? si->maxLines + line : 0;

    if (si->reverb != 0)
    {
        ApplyReverb(si, buf, samples, offset);
    }
    else
    {
        // The original clears the buffer with an unrolled 8-byte store loop.
        memset(buf + offset, 0, samples);
        memset(buf + offset + PCM_DMA_BUF_SIZE, 0, samples);
    }

    // SoundInfo declares 12 channels; m4aSoundMode can ask for 15. Clamp so a
    // mode change cannot walk off the end of the native struct.
    if (maxChans > MAX_DIRECTSOUND_CHANNELS)
        maxChans = MAX_DIRECTSOUND_CHANNELS;

    if (!lineBudget || line < lineBudget)
    {
        for (i = 0; i < maxChans; i++)
        {
            struct SoundChannel *chan = &si->chans[i];
            u32 volR = 0, volL = 0;
            u32 *rBuf = (u32 *)(buf + offset);
            u32 *lBuf = (u32 *)(buf + offset + PCM_DMA_BUF_SIZE);
            struct LoopInfo loop;

            if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
                continue;
            if (!UpdateEnvelope(chan, si, &volR, &volL))
                continue;

            GetLoopInfo(chan, &loop);

            if (chan->type & (TONEDATA_TYPE_CMP | TONEDATA_TYPE_REV))
            {
                // Reverse and DPCM waves re-anchor the source once, on the
                // first frame they play. For a DPCM wave currentPointer then
                // becomes a sample index rather than a byte offset, because the
                // decoder indexes by sample.
                if (!(chan->statusFlags & SOUND_CHANNEL_SF_SPECIAL))
                {
                    chan->statusFlags |= SOUND_CHANNEL_SF_SPECIAL;
                    if (chan->type & TONEDATA_TYPE_REV)
                    {
                        // A reverse wave starts `count` samples from the end
                        // and walks back toward the start, so the anchor moves
                        // to wav->data + (size - count).
                        chan->currentPointer =
                            (s8 *)chan->wav->data + (chan->wav->size - chan->count);
                    }
                    if (chan->wav->type != 0)
                    {
                        // DPCM anchors on a sample index rather than a byte
                        // address, which is what DecodeDPCM expects. That is
                        // also how the reverse anchor above becomes an index.
                        chan->currentPointer =
                            (s8 *)(uintptr_t)((u32)(uintptr_t)chan->currentPointer
                                              - (uintptr_t)chan->wav
                                              - offsetof(struct WaveData, data));
                        chan->xpi = 0xFFFF;
                    }
                }
                MixInterpolated(chan, &loop, chan->wav->type != 0,
                                (chan->type & TONEDATA_TYPE_REV) != 0,
                                si->divFreq, volR, volL, samples, &rBuf, &lBuf);
            }
            else if (chan->type & TONEDATA_TYPE_FIX)
            {
                MixFixed(chan, &loop, volR, volL, samples, &rBuf, &lBuf);
            }
            else
            {
                MixInterpolated(chan, &loop, false, false, si->divFreq, volR, volL,
                                samples, &rBuf, &lBuf);
            }
        }
    }

    // The legacy channels are summed into the same buffer, in the same units:
    // four GB channels at full scale reach 15 each against 127 for one
    // direct-sound channel, so the scale factor puts them in the same
    // ballpark without either dominating. pcmBuffer's first half is the right
    // speaker (it is what DMA1/FIFO_A, SOUND_A_RIGHT_OUTPUT, reads).
    if (samples != 0)
    {
        s8 *right = buf + offset;
        s8 *left = buf + offset + PCM_DMA_BUF_SIZE;
        u32 n;

        ApuUpdateRegisters(si->cgbChans, (u32)si->pcmFreq);
        for (n = 0; n < samples; n++)
        {
            s32 l, r;

            ApuRender(&l, &r);
            right[n] = (s8)((s32)right[n] + r * 2);
            left[n] = (s8)((s32)left[n] + l * 2);
        }
    }
}