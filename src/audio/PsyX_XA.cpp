/* XA-ADPCM streaming source.
 *
 * Spyro 1's music is XA-ADPCM (file=1, chan=track%8, real-time mode).
 * The PS1 hardware would auto-decode matching Mode 2 Form 2 sectors;
 * PsyCross has no equivalent path (its "XA" branch in CdSync just
 * advances a fake sector counter and discards bytes). This file
 * provides the missing decode + an OpenAL streaming source.
 *
 * Pump model: caller (typically psyq_CdSync) invokes PsyX_XA_Pump()
 * once per frame; we read N sectors from g_imageFile starting at the
 * current playhead, decode matching audio sectors, and queue PCM into
 * a small ring of OpenAL buffers. Sectors that don't match the filter
 * (or aren't audio at all) just advance the playhead.
 */

#include "PsyX_XA.h"
#include "../PsyX_main.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* PsyCross internals. LIBCD.C is built as C++ (psyq surface), so
 * vfread/vfseek/vftell have C++ linkage — declare them in the global
 * namespace, not extern "C", so the mangled names match. */
typedef struct _VFILE {
    FILE*           fp;
    unsigned char*  basePtr;
    unsigned char*  curPtr;
    size_t          size;
} VFILE;
extern VFILE g_imageFile;
size_t vfread(void* buf, size_t es, size_t ec, VFILE* s);
int    vfseek(VFILE* s, int off, int origin);
int    vftell(VFILE* s);
extern "C" int g_cdSectorSize;

namespace {

constexpr int kSectorSize    = 2352;
constexpr int kRingBuffers   = 6;
constexpr int kPumpBatch     = 8;     /* sectors per pump call default */

/* XA-ADPCM filter coefficients, scaled by 64. */
const int kFilterK0[5] = { 0,   60,  115,  98, 122 };
const int kFilterK1[5] = { 0,    0,  -52, -55, -60 };

struct XAState {
    bool     initialized = false;
    bool     active      = false;
    ALuint   source      = 0;
    ALuint   buffers[kRingBuffers] = {};
    bool     buf_in_use[kRingBuffers] = {};

    int      playhead    = 0;     /* absolute LBA */
    int      filter_file = -1;
    int      filter_chan = -1;

    /* ADPCM history per stereo channel (left=0, right=1). */
    int32_t  hist1[2] = { 0, 0 };
    int32_t  hist2[2] = { 0, 0 };

    /* Most recent codinginfo sample rate / channel count, used for
     * the current sector's queued buffer. */
    int      sample_rate = 37800;
    int      channels    = 2;
    bool     bits_8      = false;

    float    gain         = 1.0f;
    float    forced_gain  = -1.0f;   /* >=0 → overrides `gain` */
} g_xa;

inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Decode 28 ADPCM samples from a pre-extracted nibble stream into PCM.
 * Independent of byte/nibble layout, which is handled by the caller. */
void decode_block(uint8_t sound_param,
                  const uint8_t* nibbles,
                  int chan,
                  int16_t* out)
{
    int shift  = sound_param & 0x0F;
    int filter = (sound_param >> 4) & 0x03;   /* only bits 4..5 used */
    if (shift  > 12) shift = 9;

    int K0 = kFilterK0[filter];
    int K1 = kFilterK1[filter];

    int32_t h1 = g_xa.hist1[chan];
    int32_t h2 = g_xa.hist2[chan];

    for (int i = 0; i < 28; ++i) {
        /* Sign-extend 4-bit nibble into 16-bit (place in top 4 bits). */
        int32_t s = (int16_t)(nibbles[i] << 12);
        s >>= shift;
        s += (h1 * K0 + h2 * K1 + 32) >> 6;
        int16_t clamped = clamp16(s);
        out[i] = clamped;
        h2 = h1;
        h1 = clamped;
    }

    g_xa.hist1[chan] = h1;
    g_xa.hist2[chan] = h2;
}

/* Extract 28 4-bit nibbles for one ADPCM block from a sound group's
 * 112-byte data area.
 *
 * The 8 blocks per group are processed in 4 stereo pairs (i = 0..3 in
 * Sony's "headtable" layout). For pair i, both L and R blocks share
 * the same 28 byte offsets within the data area:
 *
 *   byte_offset(sample s) = i + 16*(s/4) + 4*(s%4)
 *
 * Each byte holds two nibbles — low = L sample, high = R sample of the
 * same pair. The 4 i-values partition the 112 bytes into 4 disjoint
 * sets of 28, each set producing one stereo pair of blocks. */
void extract_nibbles(const uint8_t* data, int pair_i,
                     int high_nibble, uint8_t* nibbles_out)
{
    for (int s = 0; s < 28; ++s) {
        int off = pair_i + 16 * (s / 4) + 4 * (s % 4);
        uint8_t b = data[off];
        nibbles_out[s] = high_nibble ? (b >> 4) : (b & 0x0F);
    }
}

/* SP byte indices for the 8 blocks (4-bit mode), in pair order:
 *   pair 0: bytes  0 (L),  1 (R)
 *   pair 1: bytes  2 (L),  3 (R)
 *   pair 2: bytes  8 (L),  9 (R)
 *   pair 3: bytes 10 (L), 11 (R)
 * Bytes 4..7 and 12..15 are redundant copies of the SP halves above. */
constexpr int kHeadTable[4] = { 0, 2, 8, 10 };

/* Decode one XA audio sector's 18 sound groups -> stereo PCM samples.
 * Returns number of int16 samples written to `out` (interleaved if stereo).
 *
 * Layout (within the 2336-byte audio data of a Mode 2 Form 2 sector):
 *   bytes  0..7   : subheader (file, chan, submode, codinginfo, +copy)
 *   bytes  8..2311: 18 sound groups of 128 bytes each
 *   bytes 2312..  : padding + EDC (ignored)
 *
 * Each sound group:
 *   bytes  0..3 : SP[0..3] (4-bit mode: blocks 0..3)  / 8-bit: blocks 0..3
 *   bytes  4..7 : SP[4..7] (4-bit) / SP[0..3] copy (8-bit)
 *   bytes  8..11: SP copies
 *   bytes 12..15: SP copies
 *   bytes 16..127: 112 bytes ADPCM data
 */
int decode_xa_sector(const uint8_t* sec_user_data,
                     int /*coding_info*/,
                     bool stereo,
                     int16_t* out)
{
    const uint8_t* groups = sec_user_data + 8;
    int out_idx = 0;

    uint8_t nibbles[28];
    int16_t L[28], R[28];

    for (int g = 0; g < 18; ++g) {
        const uint8_t* group = groups + g * 128;
        const uint8_t* data  = group + 16;

        for (int pair = 0; pair < 4; ++pair) {
            int sp_idx = kHeadTable[pair];

            if (stereo) {
                extract_nibbles(data, pair, /*high=*/0, nibbles);
                decode_block(group[sp_idx + 0], nibbles, /*chan=*/0, L);
                extract_nibbles(data, pair, /*high=*/1, nibbles);
                decode_block(group[sp_idx + 1], nibbles, /*chan=*/1, R);
                for (int i = 0; i < 28; ++i) {
                    out[out_idx++] = L[i];
                    out[out_idx++] = R[i];
                }
            } else {
                /* Mono: the 8 blocks all feed channel 0. Two blocks per
                 * pair via low/high nibbles, in sequence. */
                extract_nibbles(data, pair, /*high=*/0, nibbles);
                decode_block(group[sp_idx + 0], nibbles, /*chan=*/0, L);
                for (int i = 0; i < 28; ++i) out[out_idx++] = L[i];
                extract_nibbles(data, pair, /*high=*/1, nibbles);
                decode_block(group[sp_idx + 1], nibbles, /*chan=*/0, L);
                for (int i = 0; i < 28; ++i) out[out_idx++] = L[i];
            }
        }
    }
    return out_idx;
}

/* Reclaim any OpenAL buffers that the source has finished playing. */
void reclaim_processed_buffers()
{
    if (g_xa.source == 0) return;
    ALint processed = 0;
    alGetSourcei(g_xa.source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint b = 0;
        alSourceUnqueueBuffers(g_xa.source, 1, &b);
        for (int i = 0; i < kRingBuffers; ++i) {
            if (g_xa.buffers[i] == b) { g_xa.buf_in_use[i] = false; break; }
        }
    }
}

int find_free_buffer()
{
    for (int i = 0; i < kRingBuffers; ++i) {
        if (!g_xa.buf_in_use[i]) return i;
    }
    return -1;
}

} // namespace

extern "C" void PsyX_XA_Init(void)
{
    if (g_xa.initialized) return;
    alGenSources(1, &g_xa.source);
    alGenBuffers(kRingBuffers, g_xa.buffers);
    alSourcei(g_xa.source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(g_xa.source, AL_GAIN,
              g_xa.forced_gain >= 0.0f ? g_xa.forced_gain : g_xa.gain);
    alSourcef(g_xa.source, AL_PITCH, 1.0f);
    g_xa.initialized = true;
}

extern "C" void PsyX_XA_Shutdown(void)
{
    if (!g_xa.initialized) return;
    PsyX_XA_Stop();
    alDeleteSources(1, &g_xa.source);
    alDeleteBuffers(kRingBuffers, g_xa.buffers);
    g_xa.source = 0;
    memset(g_xa.buffers, 0, sizeof(g_xa.buffers));
    g_xa.initialized = false;
}

extern "C" void PsyX_XA_Start(int sector, int file, int chan)
{
    if (!g_xa.initialized) PsyX_XA_Init();
    PsyX_XA_Stop();
    g_xa.playhead    = sector;
    g_xa.filter_file = file;
    g_xa.filter_chan = chan;
    g_xa.hist1[0] = g_xa.hist1[1] = 0;
    g_xa.hist2[0] = g_xa.hist2[1] = 0;
    g_xa.active   = true;
    /* Source is started after the first buffer is queued. */
}

extern "C" void PsyX_XA_Stop(void)
{
    if (!g_xa.initialized) return;
    g_xa.active = false;
    if (g_xa.source) {
        alSourceStop(g_xa.source);
        ALint queued = 0;
        alGetSourcei(g_xa.source, AL_BUFFERS_QUEUED, &queued);
        while (queued-- > 0) {
            ALuint b = 0;
            alSourceUnqueueBuffers(g_xa.source, 1, &b);
        }
    }
    for (int i = 0; i < kRingBuffers; ++i) g_xa.buf_in_use[i] = false;
}

extern "C" int PsyX_XA_IsActive(void) { return g_xa.active ? 1 : 0; }
extern "C" int PsyX_XA_GetCurrentSector(void) { return g_xa.active ? g_xa.playhead : 0; }

extern "C" void PsyX_XA_SetVolume(float linear_gain)
{
    g_xa.gain = linear_gain;
    if (g_xa.source && g_xa.forced_gain < 0.0f) {
        alSourcef(g_xa.source, AL_GAIN, linear_gain);
    }
}

extern "C" void PsyX_XA_ForceGain(float linear_gain)
{
    g_xa.forced_gain = linear_gain;
    if (g_xa.source) {
        float g = (linear_gain >= 0.0f) ? linear_gain : g_xa.gain;
        alSourcef(g_xa.source, AL_GAIN, g);
    }
}

extern "C" void PsyX_XA_Pump(int maxSectors)
{
    if (!g_xa.initialized || !g_xa.active) return;
    if (g_imageFile.fp == NULL && g_imageFile.basePtr == NULL) return;
    if (maxSectors <= 0) maxSectors = kPumpBatch;

    reclaim_processed_buffers();

    /* Save the cd image's current cursor; we restore on the way out so
     * concurrent CdRead calls see no disturbance. */
    int saved_pos = vftell(&g_imageFile);

    uint8_t sector[kSectorSize];
    /* Worst case stereo: 18 groups * 4 stereo pairs * 28 frames = 2016
     * stereo frames = 4032 int16. Round up. */
    int16_t pcm_accum[8192 * 2];
    int pcm_used = 0;
    int sample_rate = g_xa.sample_rate;
    int channels    = g_xa.channels;

    for (int n = 0; n < maxSectors; ++n) {
        int free_slot = find_free_buffer();
        if (free_slot < 0) break;     /* ring full — wait for next pump */

        if (vfseek(&g_imageFile, g_xa.playhead * kSectorSize, SEEK_SET) != 0)
            break;
        if (vfread(sector, 1, kSectorSize, &g_imageFile) != (size_t)kSectorSize)
            break;

        /* AudioSector layout: 12 sync + 3 addr + 1 mode + 2336 data. */
        const uint8_t* user = sector + 16;
        uint8_t sub_file = user[0];
        uint8_t sub_chan = user[1];
        uint8_t sub_smode= user[2];
        uint8_t sub_coding = user[3];

        bool is_audio = (sub_smode & 0x04) != 0;   /* submode bit 2: audio */
        bool match    = is_audio
                     && (g_xa.filter_file < 0 || (int)sub_file == g_xa.filter_file)
                     && (g_xa.filter_chan < 0 || (int)sub_chan == g_xa.filter_chan);

        g_xa.playhead++;
        if (!match) continue;

        bool stereo  = (sub_coding & 0x03) == 1;
        bool half_sr = (sub_coding & 0x0C) == 0x04;
        bool bits8   = (sub_coding & 0x30) == 0x10;
        sample_rate  = half_sr ? 18900 : 37800;
        channels     = stereo  ? 2 : 1;


        if (bits8) {
            /* Not encountered in Spyro to date; skip until we have a sample. */
            continue;
        }

        /* Decode into accum, flush to a single AL buffer per pump if
         * we already accumulated for a different format — keeps things
         * simple for Spyro (one format per track). */
        int n_samples = decode_xa_sector(user, sub_coding, stereo,
                                          pcm_accum + pcm_used);
        pcm_used += n_samples;

        /* Flush every sector to keep latency low and avoid mixing rates
         * across sample-rate changes (rare). */
        ALenum fmt = (channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        ALuint b = g_xa.buffers[free_slot];
        alBufferData(b, fmt, pcm_accum, pcm_used * sizeof(int16_t), sample_rate);
        alSourceQueueBuffers(g_xa.source, 1, &b);
        g_xa.buf_in_use[free_slot] = true;
        pcm_used = 0;

        g_xa.sample_rate = sample_rate;
        g_xa.channels    = channels;
    }

    /* Restore cursor. */
    vfseek(&g_imageFile, saved_pos, SEEK_SET);

    /* Make sure source is playing if we have any queued data. */
    ALint queued = 0;
    alGetSourcei(g_xa.source, AL_BUFFERS_QUEUED, &queued);
    if (queued > 0) {
        ALint state = 0;
        alGetSourcei(g_xa.source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(g_xa.source);
    }
}
