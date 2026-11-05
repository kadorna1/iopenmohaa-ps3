/*
===========================================================================
PS3 native sound driver.

A small software mixer on top of the PSL1GHT libaudio port API. It replaces the
silent PS3NULL driver behind the engine's S_Driver_* interface:

  - 2 channel float output, 48 kHz, 256 frames per block, 8 blocks in the port.
  - A polling audio thread keeps the port ~6 blocks (32 ms) ahead of the reader.
  - Voices read decoded PCM straight from sfx_t (8/16-bit PCM) or from a private
    buffer decoded from IMA ADPCM when the voice starts.
  - 3D sounds use OpenAL's clamped inverse-distance model and constant-power
    panning against the listener axes; menu/local/listener sounds are 2D.
  - Looping sounds are refreshed every frame by AddLoopingSound and dropped by
    Update() when they stop being refreshed.

Implemented through the decoder-backed stream ring: streamed WAV/MP3/Vorbis/Opus
sounds, soundtrack music, triggered music and RoQ companion audio. Reverb and
the "wait till sound done" callbacks remain unimplemented.

Safety: every libaudio call is logged to the early trace *before* it is made.
Create USRDIR/sound.txt containing 0 to disable the driver, e.g. if the console
refuses to start with it.
===========================================================================
*/

#include "snd_local.h"
#include "snd_codec.h"

#include <audio/audio.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>
#include <sys/thread.h>

#include <malloc.h>
#include <math.h>
#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
#include <spukit/spukit.h>
#include "ps3_spukit_mix.h"
extern const u8 ps3_spukit_mix_bin[];
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../sys/ps3_log.h"
#include "../sys/ps3_platform.h"

#define PS3A_MAX_VOICES 48
#define PS3A_OUT_RATE   48000.0
#define PS3A_BLOCK      AUDIO_BLOCK_SAMPLES /* 256 frames */
#define PS3A_LEAD       6                   /* blocks kept queued ahead of the reader */
#define PS3A_MIX_HEADROOM 0.50f              /* reserve 6 dB for overlapping game sounds */

typedef struct {
    bool           active;
    bool           loop;     /* looping voice */
    bool           isLoopFx; /* created by AddLoopingSound (refreshed per frame) */
    bool           seen;     /* loop voice refreshed since ClearLoopingSounds */
    bool           local;    /* 2D: no distance attenuation, centred */
    bool           follow;   /* origin follows s_entity[entnum] */
    int            sfxHandle;
    const byte    *pcm;      /* first frame (interleaved if chans == 2) */
    byte          *own;      /* private ADPCM PCM buffer, stored as little-endian bytes */
    int            frames;
    int            width;    /* bytes per sample: 1 or 2 */
    int            chans;    /* 1 or 2 */
    double         pos;      /* position in source frames */
    double         step;     /* source frames advanced per output frame */
    float          volume;
    float          minDist;
    float          maxDist;
    vec3_t         origin;
    int            entnum;
    int            entchannel;
    int            startTime;
    float          gL, gR;   /* gains used at the end of the previous block */
    bool           gainsValid;
} ps3a_voice_t;

static ps3a_voice_t g_voice[PS3A_MAX_VOICES];

#define PS3A_STREAM_COUNT 11
#define PS3A_STREAM_FIRST_SFX 3
#define PS3A_SOUND_STREAM_CHANNEL_BASE PS3A_MAX_VOICES
#define PS3A_STREAM_RING_FRAMES 16384
#define PS3A_STREAM_CHUNK_FRAMES 2048

typedef struct {
    bool active, paused, eof, isLoopFx, seen, local, follow;
    snd_stream_t *decoder;
    /* Non-NULL only while this stream references a level-cached speech clip. */
    const short *speechPcm;
    uint32_t speechFrames;
    uint32_t speechReadFrame;
    char filename[MAX_QPATH];
    uint64_t writeFrame;
    double pos;
    int rate, width, chans, loopCount, loopsCompleted;
    int entnum, entchannel, sfxHandle, startTime;
    float step, pitch, volume, minDist, maxDist;
    vec3_t origin;
} ps3a_stream_t;

static ps3a_stream_t g_streams[PS3A_STREAM_COUNT];
static short g_stream_ring[PS3A_STREAM_COUNT][PS3A_STREAM_RING_FRAMES * 2]
    __attribute__((aligned(128)));
static bool g_codecs_inited = false;

/* One bounded, level-scoped pool stores complete speech clips as native
 * signed 16-bit PCM. Entries contain offsets, so shared aliases are deduped. */
#define PS3A_SPEECH_CACHE_MAX_ENTRIES 512
#define PS3A_SPEECH_CACHE_DEFAULT_MB 8
#define PS3A_SPEECH_CACHE_MAX_MB 16
#define PS3A_SPEECH_DECODE_CHUNK_FRAMES 2048

typedef struct {
    bool active;
    bool reserved;
    char filename[MAX_QPATH];
    uint32_t offsetBytes;
    uint32_t frames;
    int rate;
    int channels;
} ps3a_speech_cache_entry_t;

typedef struct {
    short *pcm;
    uint32_t capacityBytes;
    uint32_t usedBytes;
    int entryCount;
    char level[MAX_QPATH];
    bool building;
} ps3a_speech_cache_t;

typedef struct {
    const short *pcm;
    uint32_t frames;
    int rate;
    int channels;
} ps3a_speech_cache_take_t;

static ps3a_speech_cache_entry_t g_speech_entries[PS3A_SPEECH_CACHE_MAX_ENTRIES];
static ps3a_speech_cache_t g_speech_cache;
static cvar_t *s_ps3_speech_cache_mb;

static sys_mutex_t  g_lock;
static bool         g_lockMade   = false;
static bool         g_inited     = false; /* audio port + thread exist (kept for the whole run) */
static bool         g_up         = false; /* driver enabled for the sound system */
static volatile int g_run        = 0;
static sys_ppu_thread_t g_thread;
static u32          g_port       = 0;
static audioPortConfig g_cfg;
static sys_event_queue_t g_audio_queue;
static sys_ipc_key_t g_audio_queue_key;
static bool g_audio_notify_ready = false;
static volatile uint64_t g_audio_blocks = 0;
static volatile uint64_t g_audio_late_blocks = 0;
static int          g_tick       = 0;
/* Counters are updated by the single audio producer thread. Periodic snapshots
 * reveal whether audible output is actually being mixed on SPU or PPU. */
static uint32_t g_mix_blocks_total    = 0;
static uint32_t g_mix_blocks_spu      = 0;
static uint32_t g_mix_blocks_ppu      = 0;
static uint32_t g_mix_blocks_fallback = 0;
static uint32_t g_mix_blocks_testtone = 0;
#ifndef OPENMOHAA_PS3_AUDIO_TEST_TONE
#define OPENMOHAA_PS3_AUDIO_TEST_TONE 0
#endif
static volatile int g_ps3_test_tone = OPENMOHAA_PS3_AUDIO_TEST_TONE ? 1 : 0;
static double g_ps3_test_phase = 0.0;
static double g_audio_diag_sumsq = 0.0;
static float g_audio_diag_peak = 0.0f;
static uint32_t g_audio_diag_samples = 0;


static vec3_t g_lpos  = {0, 0, 0};
static vec3_t g_laxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
static spukit_worker *g_spu_mix_worker;
static uint8_t *g_spu_mix_stage;
static ps3a_spu_voice_t g_spu_mix_voices[PS3A_SPU_MAX_VOICES] __attribute__((aligned(128)));
static int g_spu_mix_warned;
#endif

static void ps3a_lock()
{
    if (g_lockMade) {
        sysMutexLock(g_lock, 0);
    }
}

static void ps3a_unlock()
{
    if (g_lockMade) {
        sysMutexUnlock(g_lock);
    }
}

/* Diagnostic output path: issue "ps3audiotest 1" in the in-game console
 * to replace game audio with a quiet 440 Hz tone; use "ps3audiotest 0" to
 * restore normal mixing. This separates libaudio/ring output from PCM decode. */
static void ps3a_testtone_cmd()
{
    if (Cmd_Argc() > 1) {
        g_ps3_test_tone = atoi(Cmd_Argv(1)) ? 1 : 0;
    }
    Com_Printf("PS3 audio test tone: %s (ps3audiotest 1|0)\n",
               g_ps3_test_tone ? "ON" : "OFF");
}

/* ---------------------------------------------------------------------------
   IMA ADPCM (WAVE format 17), mono, block aligned
--------------------------------------------------------------------------- */

static const short ima_index_adj[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static const short ima_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static inline int ima_clamp_index(int i)
{
    return i < 0 ? 0 : (i > 88 ? 88 : i);
}

/* Store decoded ADPCM as little-endian PCM bytes. The same representation is
 * consumed by both the PPU and SPU mixers, independent of their native endian. */
static inline void ps3a_store_s16_le(byte *dst, int sample)
{
    const uint16_t value = (uint16_t)sample;
    dst[0] = (byte)(value & 0xffu);
    dst[1] = (byte)(value >> 8);
}

/* Decodes into a malloc'd little-endian 16-bit PCM buffer. Returns the frame count. */
static int ps3a_decode_ima_mono(const byte *src, int bytes, int blockAlign, byte **outBuf)
{
    int maxSamples, written = 0;
    byte *out;

    *outBuf = NULL;
    if (blockAlign < 8 || bytes <= 0) {
        return 0;
    }

    maxSamples = ((bytes / blockAlign) + 1) * ((blockAlign - 4) * 2 + 1);
    out        = (byte *)malloc((size_t)maxSamples * sizeof(short));
    if (!out) {
        return 0;
    }

    while (bytes >= 5) {
        const int blk   = bytes < blockAlign ? bytes : blockAlign;
        int       pred  = (short)(src[0] | (src[1] << 8));
        int       index = ima_clamp_index(src[2]);
        int       i;

        ps3a_store_s16_le(out + (size_t)written * sizeof(short), pred);
        written++;
        for (i = 4; i < blk; i++) {
            int b = src[i], n;

            for (n = 0; n < 2; n++) {
                const int code = (n == 0) ? (b & 15) : (b >> 4);
                const int step = ima_step[index];
                int       diff = step >> 3;

                if (code & 1) diff += step >> 2;
                if (code & 2) diff += step >> 1;
                if (code & 4) diff += step;
                if (code & 8) diff = -diff;

                pred += diff;
                if (pred > 32767) pred = 32767;
                if (pred < -32768) pred = -32768;
                index = ima_clamp_index(index + ima_index_adj[code]);

                if (written < maxSamples) {
                    ps3a_store_s16_le(out + (size_t)written * sizeof(short), pred);
                    written++;
                }
            }
        }

        src += blk;
        bytes -= blk;
    }

    *outBuf = out;
    return written;
}

/* ---------------------------------------------------------------------------
   Voice management (callers hold the lock)
--------------------------------------------------------------------------- */

static void ps3a_free_voice(ps3a_voice_t *v)
{
    v->active = false;
    if (v->own) {
        free(v->own);
        v->own = NULL;
    }
    v->pcm = NULL;
}

static bool ps3a_sfx_usable(const sfx_t *sfx)
{
    if (!sfx || !sfx->data) {
        return false;
    }
    if (sfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3 | SFX_FLAG_NULL | SFX_FLAG_NO_OFFSET)) {
        return false;
    }
    return sfx->info.channels >= 1 && sfx->info.channels <= 2 && sfx->info.rate >= 4000.0f
        && sfx->info.datasize > sfx->info.dataofs;
}

static ps3a_voice_t *ps3a_pick_voice(int sfxHandle, int entnum, int entchannel, bool loopFx)
{
    ps3a_voice_t *free_v = NULL, *oldest = NULL, *oldestSame = NULL;
    int           same = 0, maxSame = DEFAULT_SFX_NUMBER_PLAYING, i;

    if (s_knownSfx[sfxHandle].sfx_info_index > 0 && s_knownSfx[sfxHandle].sfx_info_index < MAX_SFX_INFOS) {
        const int mp = sfx_infos[s_knownSfx[sfxHandle].sfx_info_index].max_number_playing;
        if (mp > 0) {
            maxSame = mp;
        }
    }

    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_voice_t *v = &g_voice[i];

        if (!v->active) {
            if (!free_v) {
                free_v = v;
            }
            continue;
        }
        /* an entity channel (other than AUTO) is replaced by the new sound */
        if (!loopFx && !v->isLoopFx && entchannel != CHAN_AUTO && v->entnum == entnum && v->entchannel == entchannel) {
            ps3a_free_voice(v);
            return v;
        }
        if (v->sfxHandle == sfxHandle) {
            same++;
            if (!oldestSame || v->startTime < oldestSame->startTime) {
                oldestSame = v;
            }
        }
        if (!v->isLoopFx && (!oldest || v->startTime < oldest->startTime)) {
            oldest = v;
        }
    }

    if (same >= maxSame && oldestSame && !oldestSame->isLoopFx) {
        ps3a_free_voice(oldestSame);
        return oldestSame;
    }
    if (free_v) {
        return free_v;
    }
    if (oldest) {
        ps3a_free_voice(oldest);
        return oldest;
    }
    return NULL;
}

static bool ps3a_setup_voice(ps3a_voice_t *v, int sfxHandle, float pitch)
{
    sfx_t       *sfx = &s_knownSfx[sfxHandle];
    const byte  *src = sfx->data + sfx->info.dataofs;
    const int    bytes = sfx->info.datasize - sfx->info.dataofs;
    const float  w = sfx->info.width;

    v->own = NULL;
    if (sfx->info.format == 17 || w == 0.5f) {
        const int blockAlign = (sfx->info.dataalign > 1) ? (sfx->info.dataalign - 1) / 2 + 4 : 0;
        byte     *buf;
        const int n = (sfx->info.channels == 1) ? ps3a_decode_ima_mono(src, bytes, blockAlign, &buf) : 0;

        if (n <= 0) {
            return false;
        }
        v->own    = buf;
        v->pcm    = buf;
        v->frames = n;
        v->width  = 2;
        {
            static unsigned int adpcmTraceCount;
            if (adpcmTraceCount < 8) {
                PS3_BootTracef("PS3AUDIO: ADPCM voice decoded name=%s frames=%d pcm_storage=little-endian",
                               sfx->name, n);
                adpcmTraceCount++;
            }
        }
        v->chans  = 1;
        v->pos    = 0.0;
        v->step   = (double)sfx->info.rate * (double)pitch / PS3A_OUT_RATE;
        return true;
    }

    if (sfx->info.format != 1 || (w != 1.0f && w != 2.0f)) {
        return false;
    }
    v->width  = (int)w;
    v->chans  = sfx->info.channels;
    v->frames = bytes / (v->width * v->chans);
    v->pcm    = src;
    if (v->frames <= 1) {
        return false;
    }
    v->pos  = 0.0;
    v->step = (double)sfx->info.rate * (double)pitch / PS3A_OUT_RATE;
    return true;
}

/* ---------------------------------------------------------------------------
   Mixing (audio thread)
--------------------------------------------------------------------------- */

/* Non-streamed WAV PCM remains in the little-endian byte order used on disk.
 * ADPCM is explicitly normalized to the same representation above. */
static inline float ps3a_sample(const ps3a_voice_t *v, int frame, int ch)
{
    const byte *p = v->pcm + ((size_t)frame * v->chans + (ch < v->chans ? ch : 0)) * v->width;

    if (v->width == 2) {
        return (float)(short)(p[0] | ((unsigned int)p[1] << 8)) * (1.0f / 32768.0f);
    }
    return (float)((int)p[0] - 128) * (1.0f / 128.0f);
}

static void ps3a_target_gains(const ps3a_voice_t *v, float master, float *gl, float *gr)
{
    float att = 1.0f, pan = 0.0f;

    if (!v->local) {
        vec3_t d;
        float  dist, cd, sep;

        VectorSubtract(v->origin, g_lpos, d);
        dist = sqrtf(DotProduct(d, d));
        cd   = dist < v->minDist ? v->minDist : (dist > v->maxDist ? v->maxDist : dist);
        att  = (cd > 0.0f) ? v->minDist / cd : 1.0f;

        if (dist > 1.0f) {
            /* axis[1] points left, so a positive projection is a source on the left */
            pan = -DotProduct(d, g_laxis[1]) / dist;
            sep = s_separation ? s_separation->value * 2.0f : 1.0f;
            if (sep > 1.0f) sep = 1.0f;
            if (sep < 0.0f) sep = 0.0f;
            pan *= sep;
        }
    }

    {
        const float a = (pan + 1.0f) * 0.78539816f; /* 0..pi/2 */
        const float g = master * v->volume * att;

        *gl = g * cosf(a);
        *gr = g * sinf(a);
    }
}

static void ps3a_mix_block_ppu(float *out)
{
    float  mixL[PS3A_BLOCK], mixR[PS3A_BLOCK];
    float  master = (s_volume ? s_volume->value : 0.9f) * PS3A_MIX_HEADROOM;
    int    i, k;

    memset(mixL, 0, sizeof(mixL));
    memset(mixR, 0, sizeof(mixR));

    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_voice_t *v = &g_voice[i];
        float         tL, tR, gL, gR, dL, dR;

        if (!v->active) {
            continue;
        }

        ps3a_target_gains(v, master, &tL, &tR);
        if (!v->gainsValid) {
            v->gL = tL;
            v->gR = tR;
            v->gainsValid = true;
        }
        gL = v->gL;
        gR = v->gR;
        dL = (tL - gL) * (1.0f / PS3A_BLOCK);
        dR = (tR - gR) * (1.0f / PS3A_BLOCK);

        for (k = 0; k < PS3A_BLOCK; k++) {
            const int    f0 = (int)v->pos;
            int          f1 = f0 + 1;
            const float  frac = (float)(v->pos - (double)f0);
            float        l, r;

            if (f0 >= v->frames) {
                if (v->loop) {
                    v->pos -= (double)v->frames;
                    if (v->pos < 0.0) v->pos = 0.0;
                    k--;
                    continue;
                }
                v->active = false; /* the main thread frees v->own */
                break;
            }
            if (f1 >= v->frames) {
                f1 = v->loop ? 0 : f0;
            }

            l = ps3a_sample(v, f0, 0);
            l += (ps3a_sample(v, f1, 0) - l) * frac;
            if (v->chans == 2) {
                r = ps3a_sample(v, f0, 1);
                r += (ps3a_sample(v, f1, 1) - r) * frac;
            } else {
                r = l;
            }

            mixL[k] += l * gL;
            mixR[k] += r * gR;
            gL += dL;
            gR += dR;
            v->pos += v->step;
        }

        v->gL = tL;
        v->gR = tR;
    }
    ps3a_unlock();

    /* Do not clip the dry voice mix here: streams are added afterwards.
     * Apply the output limit once, after every source has been summed. */
    for (k = 0; k < PS3A_BLOCK; k++) {
        out[k * 2]     = mixL[k];
        out[k * 2 + 1] = mixR[k];
    }
}


#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
static uint32_t ps3a_align16(uint32_t n) { return (n + 15u) & ~15u; }

/* Locking pins source buffers through the SPE job. This is intentionally
 * experimental because the waiting audio thread may briefly block game calls. */
static bool ps3a_mix_block_spu(float *out)
{
    if (!g_spu_mix_worker || !g_spu_mix_stage || !out || (((uintptr_t)out) & 15u)) return false;
    memset(g_spu_mix_voices, 0, sizeof(g_spu_mix_voices));
    ps3a_lock();

    for (uint32_t i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_voice_t *src = &g_voice[i];
        ps3a_spu_voice_t *dst = &g_spu_mix_voices[i];
        if (!src->active) continue;
        if (!src->pcm || src->frames <= 0 ||
            (src->width != 1 && src->width != 2) ||
            (src->chans != 1 && src->chans != 2) ||
            src->step <= 0.0 || src->step > 4.0) goto ppu_fallback;

        const uint32_t bpf = (uint32_t)src->width * (uint32_t)src->chans;
        uint64_t pos_q16 = (uint64_t)(src->pos * 65536.0 + 0.5);
        const uint64_t step_q16 = (uint64_t)(src->step * 65536.0 + 0.5);
        const uint64_t frame_limit_q16 = (uint64_t)(uint32_t)src->frames << 16;
        if (src->loop && pos_q16 >= frame_limit_q16) pos_q16 -= frame_limit_q16;
        if (pos_q16 >= frame_limit_q16) {
            if (src->loop) goto ppu_fallback;
            dst->frames = (uint32_t)src->frames;
            dst->pos_q16 = pos_q16;
            dst->step_q16 = step_q16;
            dst->active = 0;
            continue;
        }

        const uint32_t start = (uint32_t)(pos_q16 >> 16);
        const uint64_t final_offset_q16 = (pos_q16 & 0xFFFFu) +
            (uint64_t)(PS3A_BLOCK - 1) * step_q16;
        const uint64_t required64 = (final_offset_q16 >> 16) + 2u;
        if (required64 > PS3A_SPU_STAGE_PER_VOICE / bpf) goto ppu_fallback;
        const uint32_t required = (uint32_t)required64;
        uint32_t first = (uint32_t)src->frames - start;
        if (first > required) first = required;
        const uint32_t second = required - first;
        if (second && (!src->loop || second > (uint32_t)src->frames)) goto ppu_fallback;

        const uint32_t base = i * PS3A_SPU_STAGE_PER_VOICE;
        const uint32_t first_bytes = first * bpf;
        const uint32_t first_padded = ps3a_align16(first_bytes);
        const uint32_t second_off = base + first_padded;
        const uint32_t second_bytes = second * bpf;
        const uint32_t second_padded = ps3a_align16(second_bytes);
        if (first_padded + second_padded > PS3A_SPU_STAGE_PER_VOICE) goto ppu_fallback;

        memcpy(g_spu_mix_stage + base, src->pcm + (size_t)start * bpf, first_bytes);
        if (first_padded > first_bytes)
            memset(g_spu_mix_stage + base + first_bytes, 0, first_padded - first_bytes);
        if (second) {
            memcpy(g_spu_mix_stage + second_off, src->pcm, second_bytes);
            if (second_padded > second_bytes)
                memset(g_spu_mix_stage + second_off + second_bytes, 0, second_padded - second_bytes);
        }

        float target_l, target_r;
        ps3a_target_gains(src, (s_volume ? s_volume->value : 0.9f) * PS3A_MIX_HEADROOM,
                          &target_l, &target_r);
        dst->first_off = base;
        dst->second_off = second_off;
        dst->first_start = start;
        dst->first_frames = first;
        dst->second_frames = second;
        dst->frames = (uint32_t)src->frames;
        dst->pos_q16 = pos_q16;
        dst->step_q16 = step_q16;
        dst->gain_l = src->gainsValid ? src->gL : target_l;
        dst->gain_r = src->gainsValid ? src->gR : target_r;
        dst->delta_l = (target_l - dst->gain_l) * (1.0f / PS3A_BLOCK);
        dst->delta_r = (target_r - dst->gain_r) * (1.0f / PS3A_BLOCK);
        dst->target_l = target_l;
        dst->target_r = target_r;
        dst->width = (uint32_t)src->width;
        dst->chans = (uint32_t)src->chans;
        dst->loop = src->loop ? 1u : 0u;
        dst->active = 1u;
    }

    /* Include decoded stream PCM in the SPU job. The stream ring is a native-
     * endian short array on PPU; normalize it to little-endian PCM while staging. */
    for (uint32_t i = 0; i < PS3A_STREAM_COUNT; i++) {
        ps3a_stream_t *stream = &g_streams[i];
        ps3a_spu_voice_t *dst = &g_spu_mix_voices[PS3A_MAX_VOICES + i];
        if (!stream->active || stream->paused) continue;
        if (stream->pos < 0.0 || stream->step <= 0.0f || stream->step > 4.0f)
            goto ppu_fallback;

        const uint64_t start_frame = (uint64_t)stream->pos;
        const uint32_t frac_q16 = (uint32_t)((stream->pos - (double)start_frame) * 65536.0);
        const uint64_t step_q16 = (uint64_t)((double)stream->step * 65536.0 + 0.5);
        if (!step_q16 || stream->writeFrame <= start_frame) {
            if (stream->eof) {
                stream->active = false;
                continue;
            }
            goto ppu_fallback;
        }

        uint64_t required64 = (((uint64_t)frac_q16 +
            (uint64_t)(PS3A_BLOCK - 1) * step_q16) >> 16) + 2u;
        uint64_t available64 = stream->writeFrame - start_frame;
        if (required64 > available64) {
            if (!stream->eof) goto ppu_fallback;
            required64 = available64;
        }
        if (!required64) {
            stream->active = false;
            continue;
        }
        if (required64 > PS3A_SPU_STAGE_PER_VOICE / 4u)
            goto ppu_fallback;

        const uint32_t frames = (uint32_t)required64;
        const uint32_t base = (PS3A_MAX_VOICES + i) * PS3A_SPU_STAGE_PER_VOICE;
        for (uint32_t f = 0; f < frames; f++) {
            const uint32_t ring_frame = (uint32_t)((start_frame + f) % PS3A_STREAM_RING_FRAMES);
            const short left = g_stream_ring[i][ring_frame * 2u];
            const short right = g_stream_ring[i][ring_frame * 2u + 1u];
            ps3a_store_s16_le(g_spu_mix_stage + base + f * 4u, left);
            ps3a_store_s16_le(g_spu_mix_stage + base + f * 4u + 2u, right);
        }

        ps3a_voice_t positional;
        float target_l, target_r;
        memset(&positional, 0, sizeof(positional));
        positional.local = stream->local;
        positional.volume = stream->volume;
        positional.minDist = stream->minDist;
        positional.maxDist = stream->maxDist;
        VectorCopy(stream->origin, positional.origin);
        ps3a_target_gains(&positional,
            (s_volume ? s_volume->value : 0.9f) * PS3A_MIX_HEADROOM,
            &target_l, &target_r);

        dst->first_off = base;
        dst->second_off = base;
        dst->first_start = 0;
        dst->first_frames = frames;
        dst->second_frames = 0;
        dst->frames = frames;
        dst->pos_q16 = frac_q16;
        dst->step_q16 = step_q16;
        dst->gain_l = target_l;
        dst->gain_r = target_r;
        dst->target_l = target_l;
        dst->target_r = target_r;
        dst->width = 2u;
        dst->chans = 2u;
        dst->loop = 0u;
        dst->active = 1u;
    }

    {
        ps3a_spu_mix_job_t job;
        job.voices_ea = (uint64_t)(uintptr_t)g_spu_mix_voices;
        job.stage_ea = (uint64_t)(uintptr_t)g_spu_mix_stage;
        job.output_ea = (uint64_t)(uintptr_t)out;
        job.voice_count = PS3A_SPU_MAX_VOICES;
        job.output_frames = PS3A_BLOCK;
        uint32_t seq = spukit_post(g_spu_mix_worker, PS3A_SPU_JOB_MIX, &job, sizeof(job));
        const spukit_result *res = spukit_wait(g_spu_mix_worker, seq);
        if (!res || res->error != 0) {
            ps3a_unlock();
            if (!g_spu_mix_warned++) PS3_BootTracef("PS3AUDIO: experimental SPU mixer job failed; using PPU mixer");
            return false;
        }
        for (uint32_t i = 0; i < PS3A_MAX_VOICES; i++) {
            ps3a_voice_t *src = &g_voice[i];
            ps3a_spu_voice_t *dst = &g_spu_mix_voices[i];
            if (!src->active) continue;
            src->active = dst->active ? true : false;
            src->pos = (double)dst->pos_q16 / 65536.0;
            src->gL = dst->target_l;
            src->gR = dst->target_r;
            src->gainsValid = true;
        }
    }
    for (uint32_t i = 0; i < PS3A_STREAM_COUNT; i++) {
        ps3a_stream_t *stream = &g_streams[i];
        ps3a_spu_voice_t *dst = &g_spu_mix_voices[PS3A_MAX_VOICES + i];
        if (!stream->active || stream->paused || !dst->frames) continue;
        stream->pos = (double)(uint64_t)stream->pos
                    + (double)dst->pos_q16 / 65536.0;
        if (!dst->active && stream->eof)
            stream->active = false;
    }
    ps3a_unlock();
    return true;

ppu_fallback:
    ps3a_unlock();
    if (!g_spu_mix_warned++) PS3_BootTracef("PS3AUDIO: voice outside experimental SPU mixer limits; using PPU mixer");
    return false;
}
#endif


static snd_stream_t *ps3a_stream_open_decoder(const char *filename, int *rate, int *width,
                                               int *chans, unsigned int offsetFrames)
{
    snd_stream_t *d = S_CodecOpenStream(filename);
    byte discard[4096];
    uint64_t skip, done = 0;
    int frameBytes;
    if (!d) return NULL;
    if (d->info.rate <= 0 || d->info.channels < 1 || d->info.channels > 2
        || (d->info.width != 1.0f && d->info.width != 2.0f)) {
        PS3_BootTracef("PS3AUDIO: unsupported stream format %s rate=%d width=%.1f channels=%d",
                       filename, d->info.rate, d->info.width, d->info.channels);
        S_CodecCloseStream(d);
        return NULL;
    }
    *rate = d->info.rate;
    *width = (int)d->info.width;
    *chans = d->info.channels;
    frameBytes = *width * *chans;
    skip = (uint64_t)offsetFrames * (uint64_t)frameBytes;
    while (done < skip) {
        int req = (int)((skip - done) < sizeof(discard) ? skip - done : sizeof(discard));
        int got = S_CodecReadStream(d, req, discard);
        if (got <= 0) break;
        done += (uint64_t)got;
    }
    return d;
}

static void ps3a_speech_cache_clear_internal(void)
{
    short *oldPool;
    int i;

    /* Stop any stream still pointing into the pool before releasing it. */
    ps3a_lock();
    for (i = 0; i < PS3A_STREAM_COUNT; i++) {
        if (g_streams[i].speechPcm) {
            g_streams[i].active = false;
            g_streams[i].speechPcm = NULL;
            g_streams[i].speechFrames = 0;
            g_streams[i].speechReadFrame = 0;
            g_streams[i].filename[0] = '\0';
            g_streams[i].eof = true;
        }
    }
    oldPool = g_speech_cache.pcm;
    memset(&g_speech_cache, 0, sizeof(g_speech_cache));
    memset(g_speech_entries, 0, sizeof(g_speech_entries));
    ps3a_unlock();

    if (oldPool) {
        free(oldPool);
    }
}

static void ps3a_speech_cache_begin_internal(const char *levelName)
{
    int limitMb;
    uint32_t capacity;
    short *pool;

    ps3a_speech_cache_clear_internal();
    if (!g_up) {
        PS3_BootTracef("PS3AUDIO: speech cache disabled; audio driver is down level=%s",
                       levelName ? levelName : "(unknown)");
        return;
    }

    limitMb = s_ps3_speech_cache_mb ? s_ps3_speech_cache_mb->integer
                                    : PS3A_SPEECH_CACHE_DEFAULT_MB;
    if (limitMb < 0) limitMb = 0;
    if (limitMb > PS3A_SPEECH_CACHE_MAX_MB) limitMb = PS3A_SPEECH_CACHE_MAX_MB;
    if (!limitMb) {
        PS3_BootTracef("PS3AUDIO: speech cache disabled by cvar level=%s",
                       levelName ? levelName : "(unknown)");
        return;
    }

    capacity = (uint32_t)limitMb * 1024u * 1024u;
    pool = (short *)malloc(capacity);
    if (!pool) {
        PS3_BootTracef("PS3AUDIO: speech cache allocation failed level=%s bytes=%u",
                       levelName ? levelName : "(unknown)", (unsigned)capacity);
        return;
    }

    ps3a_lock();
    g_speech_cache.pcm = pool;
    g_speech_cache.capacityBytes = capacity;
    g_speech_cache.building = true;
    Q_strncpyz(g_speech_cache.level, levelName ? levelName : "(unknown)",
               sizeof(g_speech_cache.level));
    ps3a_unlock();

    PS3_BootTracef("PS3AUDIO: speech cache begin level=%s budget=%dMB",
                   levelName ? levelName : "(unknown)", limitMb);
}

static int ps3a_speech_cache_add_internal(const char *filename)
{
    snd_stream_t *decoder;
    byte raw[PS3A_SPEECH_DECODE_CHUNK_FRAMES * 4];
    int rate = 0, width = 0, channels = 0;
    int frameBytes, slot = -1, i;
    uint32_t startOffset, availableBytes, totalFrames = 0, writtenBytes = 0;
    bool overflow = false;
    short *pool;

    if (!g_up || !filename || !filename[0]) return -1;
    if (strlen(filename) >= MAX_QPATH) return 0;

    ps3a_lock();
    if (!g_speech_cache.building || !g_speech_cache.pcm) {
        ps3a_unlock();
        return -1;
    }

    for (i = 0; i < PS3A_SPEECH_CACHE_MAX_ENTRIES; i++) {
        if ((g_speech_entries[i].active || g_speech_entries[i].reserved)
            && !Q_stricmp(g_speech_entries[i].filename, filename)) {
            ps3a_unlock();
            return 1;
        }
        if (slot < 0 && !g_speech_entries[i].active && !g_speech_entries[i].reserved) {
            slot = i;
        }
    }
    if (slot < 0) {
        ps3a_unlock();
        return -1;
    }

    startOffset = g_speech_cache.usedBytes;
    availableBytes = g_speech_cache.capacityBytes - startOffset;
    pool = g_speech_cache.pcm;
    g_speech_entries[slot].reserved = true;
    Q_strncpyz(g_speech_entries[slot].filename, filename,
               sizeof(g_speech_entries[slot].filename));
    ps3a_unlock();

    decoder = ps3a_stream_open_decoder(filename, &rate, &width, &channels, 0);
    if (!decoder) {
        ps3a_lock();
        memset(&g_speech_entries[slot], 0, sizeof(g_speech_entries[slot]));
        ps3a_unlock();
        return 0;
    }

    frameBytes = width * channels;
    while (frameBytes > 0) {
        int got = S_CodecReadStream(decoder,
            PS3A_SPEECH_DECODE_CHUNK_FRAMES * frameBytes, raw);
        int frames, n;
        uint32_t outFrameBytes = (uint32_t)channels * (uint32_t)sizeof(short);
        short *dst;

        if (got <= 0) break;
        frames = got / frameBytes;
        if (frames <= 0) break;
        if ((uint64_t)writtenBytes + (uint64_t)frames * outFrameBytes > availableBytes) {
            overflow = true;
            break;
        }

        dst = (short *)((byte *)pool + startOffset + writtenBytes);
        for (n = 0; n < frames; n++) {
            int left, right;
            if (width == 2) {
                short sample;
                const int inputOffset = n * frameBytes;
                memcpy(&sample, raw + inputOffset, sizeof(sample));
                left = (int)sample;
                if (channels == 2) {
                    memcpy(&sample, raw + inputOffset + 2, sizeof(sample));
                    right = (int)sample;
                } else {
                    right = left;
                }
            } else {
                const int inputOffset = n * channels;
                left = ((int)raw[inputOffset] - 128) << 8;
                right = channels == 2 ? (((int)raw[inputOffset + 1] - 128) << 8) : left;
            }

            dst[n * channels] = (short)left;
            if (channels == 2) dst[n * 2 + 1] = (short)right;
        }

        totalFrames += (uint32_t)frames;
        writtenBytes += (uint32_t)frames * outFrameBytes;
    }

    S_CodecCloseStream(decoder);

    if (overflow || !totalFrames) {
        ps3a_lock();
        memset(&g_speech_entries[slot], 0, sizeof(g_speech_entries[slot]));
        ps3a_unlock();
        return 0; /* Partial clips are never exposed; normal streaming remains available. */
    }

    ps3a_lock();
    if (g_speech_cache.building && g_speech_cache.pcm == pool
        && g_speech_entries[slot].reserved
        && !Q_stricmp(g_speech_entries[slot].filename, filename)
        && g_speech_cache.usedBytes == startOffset) {
        g_speech_entries[slot].offsetBytes = startOffset;
        g_speech_entries[slot].frames = totalFrames;
        g_speech_entries[slot].rate = rate;
        g_speech_entries[slot].channels = channels;
        g_speech_entries[slot].active = true;
        g_speech_entries[slot].reserved = false;
        g_speech_cache.usedBytes += writtenBytes;
        g_speech_cache.entryCount++;
        ps3a_unlock();

        PS3_BootTracef("PS3AUDIO: speech cached level=%s file=%s frames=%u rate=%d ch=%d bytes=%u used=%u/%u",
                       g_speech_cache.level, filename, (unsigned)totalFrames, rate, channels,
                       (unsigned)writtenBytes, (unsigned)(startOffset + writtenBytes),
                       (unsigned)g_speech_cache.capacityBytes);
        return 2;
    }

    memset(&g_speech_entries[slot], 0, sizeof(g_speech_entries[slot]));
    ps3a_unlock();
    return 0;
}

static void ps3a_speech_cache_end_internal(void)
{
    char level[MAX_QPATH];
    int entries;
    uint32_t used, capacity;

    ps3a_lock();
    g_speech_cache.building = false;
    Q_strncpyz(level, g_speech_cache.level, sizeof(level));
    entries = g_speech_cache.entryCount;
    used = g_speech_cache.usedBytes;
    capacity = g_speech_cache.capacityBytes;
    ps3a_unlock();

    PS3_BootTracef("PS3AUDIO: speech cache ready level=%s clips=%d used=%u budget=%u",
                   level[0] ? level : "(disabled)", entries,
                   (unsigned)used, (unsigned)capacity);
}

static bool ps3a_speech_cache_take(const char *filename, ps3a_speech_cache_take_t *taken)
{
    int i;
    bool found = false;

    if (!filename || !filename[0] || !taken) return false;
    memset(taken, 0, sizeof(*taken));

    ps3a_lock();
    for (i = 0; i < PS3A_SPEECH_CACHE_MAX_ENTRIES; i++) {
        const ps3a_speech_cache_entry_t *entry = &g_speech_entries[i];
        if (!entry->active || Q_stricmp(entry->filename, filename)) continue;
        taken->pcm = (const short *)((const byte *)g_speech_cache.pcm + entry->offsetBytes);
        taken->frames = entry->frames;
        taken->rate = entry->rate;
        taken->channels = entry->channels;
        found = true;
        break;
    }
    ps3a_unlock();
    return found;
}

static void ps3a_stream_stop(int slot)
{
    snd_stream_t *d;
    if (slot < 0 || slot >= PS3A_STREAM_COUNT) return;
    ps3a_lock();
    d = g_streams[slot].decoder;
    g_streams[slot].decoder = NULL;
    g_streams[slot].speechPcm = NULL;
    g_streams[slot].speechFrames = 0;
    g_streams[slot].speechReadFrame = 0;
    g_streams[slot].active = false;
    g_streams[slot].paused = false;
    g_streams[slot].eof = true;
    g_streams[slot].isLoopFx = false;
    g_streams[slot].seen = false;
    g_streams[slot].filename[0] = '\0';
    ps3a_unlock();
    if (d) S_CodecCloseStream(d);
}

static void ps3a_stream_refill(int slot)
{
    ps3a_stream_t *s;
    byte raw[PS3A_STREAM_CHUNK_FRAMES * 4];
    short pcm[PS3A_STREAM_CHUNK_FRAMES * 2];
    int attempts = 0;
    if (slot < 0 || slot >= PS3A_STREAM_COUNT) return;
    s = &g_streams[slot];

    while (attempts++ < 6) {
        snd_stream_t *d;
        const short *speechPcm;
        uint64_t readFrame, available, capacity, speechReadFrame, speechFrames;
        int width, chans, request, got, frames, i;
        ps3a_lock();
        d = s->decoder;
        speechPcm = s->speechPcm;
        if ((!d && !speechPcm) || s->paused || s->eof) { ps3a_unlock(); return; }
        readFrame = (uint64_t)s->pos;
        available = s->writeFrame > readFrame ? s->writeFrame - readFrame : 0;
        capacity = PS3A_STREAM_RING_FRAMES - available - 1;
        width = s->width;
        chans = s->chans;
        speechReadFrame = s->speechReadFrame;
        speechFrames = s->speechFrames;
        ps3a_unlock();
        if (available >= PS3A_STREAM_RING_FRAMES / 2 || !capacity) return;
        if (capacity > PS3A_STREAM_CHUNK_FRAMES) capacity = PS3A_STREAM_CHUNK_FRAMES;

        if (speechPcm) {
            if (speechReadFrame >= speechFrames) {
                bool again;
                ps3a_lock();
                if (s->speechPcm != speechPcm || s->eof) {
                    ps3a_unlock();
                    return;
                }
                s->loopsCompleted++;
                again = (s->loopCount == 0 || s->loopsCompleted < s->loopCount);
                if (again) s->speechReadFrame = 0;
                else s->eof = true;
                ps3a_unlock();
                if (!again) return;
                continue;
            }

            frames = (int)(speechFrames - speechReadFrame);
            if ((uint64_t)frames > capacity) frames = (int)capacity;
            ps3a_lock();
            if (s->speechPcm != speechPcm || s->eof) {
                ps3a_unlock();
                return;
            }
            for (i = 0; i < frames; i++) {
                const uint64_t sourceFrame = speechReadFrame + (uint64_t)i;
                const uint32_t dstFrame =
                    (uint32_t)((s->writeFrame + (uint64_t)i) % PS3A_STREAM_RING_FRAMES);
                const short left = speechPcm[sourceFrame * (uint64_t)chans];
                const short right = chans == 2 ? speechPcm[sourceFrame * 2u + 1u] : left;
                g_stream_ring[slot][dstFrame * 2u] = left;
                g_stream_ring[slot][dstFrame * 2u + 1u] = right;
            }
            s->speechReadFrame += (uint32_t)frames;
            s->writeFrame += (uint64_t)frames;
            ps3a_unlock();
            continue;
        }

        request = (int)(capacity * (uint64_t)width * (uint64_t)chans);
        got = S_CodecReadStream(d, request, raw);
        if (got <= 0) {
            snd_stream_t *old = NULL;
            bool again = false;
            ps3a_lock();
            if (s->decoder == d) {
                old = s->decoder;
                s->decoder = NULL;
                s->loopsCompleted++;
                again = (s->loopCount == 0 || s->loopsCompleted < s->loopCount);
                if (!again) s->eof = true;
            }
            ps3a_unlock();
            if (old) S_CodecCloseStream(old);
            if (!again) return;
            {
                int nr=0,nw=0,nc=0;
                snd_stream_t *nd=ps3a_stream_open_decoder(s->filename,&nr,&nw,&nc,0);
                if (!nd) { ps3a_lock(); s->eof=true; ps3a_unlock(); return; }
                ps3a_lock();
                if (!s->eof && !s->decoder) {
                    s->decoder=nd; s->rate=nr; s->width=nw; s->chans=nc;
                    s->step=(float)nr*s->pitch/(float)PS3A_OUT_RATE;
                    ps3a_unlock();
                } else { ps3a_unlock(); S_CodecCloseStream(nd); return; }
            }
            continue;
        }
        frames = got / (width * chans);
        if (frames <= 0) return;
        if (frames > (int)capacity) frames = (int)capacity;
        for (i=0;i<frames;i++) {
            int l,r;
            if(width==2) {
                int o=i*chans*2;
                short leftSample, rightSample;
                /* Codec read functions return host-native 16-bit PCM:
                 * WAV swaps on big-endian, MP3/Vorbis request big-endian,
                 * and Opus writes native opus_int16 samples. Copy rather than
                 * assembling little-endian bytes before storing in the short ring. */
                memcpy(&leftSample, raw + o, sizeof(leftSample));
                l = (int)leftSample;
                if(chans==2) {
                    memcpy(&rightSample, raw + o + 2, sizeof(rightSample));
                    r = (int)rightSample;
                } else {
                    r = l;
                }
            } else {
                int o=i*chans;
                l=((int)raw[o]-128)<<8;
                r=chans==2?(((int)raw[o+1]-128)<<8):l;
            }
            pcm[i*2]=(short)l; pcm[i*2+1]=(short)r;
        }
        ps3a_lock();
        if(s->decoder!=d || s->eof) { ps3a_unlock(); return; }
        for(i=0;i<frames;i++) {
            uint32_t dst=(uint32_t)((s->writeFrame+(uint64_t)i)%PS3A_STREAM_RING_FRAMES);
            g_stream_ring[slot][dst*2]=pcm[i*2];
            g_stream_ring[slot][dst*2+1]=pcm[i*2+1];
        }
        s->writeFrame+=(uint64_t)frames;
        ps3a_unlock();
    }
}

static void ps3a_mix_streams(float *out)
{
    float master=(s_volume?s_volume->value:0.9f)*PS3A_MIX_HEADROOM;
    int slot,k;
    ps3a_lock();
    for(slot=0;slot<PS3A_STREAM_COUNT;slot++) {
        ps3a_stream_t *s=&g_streams[slot];
        ps3a_voice_t v;
        float gl,gr;
        if(!s->active || s->paused) continue;
        memset(&v,0,sizeof(v));
        v.local=s->local; v.volume=s->volume; v.minDist=s->minDist; v.maxDist=s->maxDist;
        VectorCopy(s->origin,v.origin);
        ps3a_target_gains(&v,master,&gl,&gr);
        for(k=0;k<PS3A_BLOCK;k++) {
            uint64_t f0=(uint64_t)s->pos, f1;
            uint32_t i0,i1;
            float frac,l0,r0,l1,r1;
            if(f0>=s->writeFrame) {
                if(s->eof) s->active=false;
                break;
            }
            f1=(f0+1<s->writeFrame)?f0+1:f0;
            i0=(uint32_t)(f0%PS3A_STREAM_RING_FRAMES);
            i1=(uint32_t)(f1%PS3A_STREAM_RING_FRAMES);
            frac=(float)(s->pos-(double)f0);
            l0=(float)g_stream_ring[slot][i0*2]*(1.0f/32768.0f);
            r0=(float)g_stream_ring[slot][i0*2+1]*(1.0f/32768.0f);
            l1=(float)g_stream_ring[slot][i1*2]*(1.0f/32768.0f);
            r1=(float)g_stream_ring[slot][i1*2+1]*(1.0f/32768.0f);
            out[k*2]+=(l0+(l1-l0)*frac)*gl;
            out[k*2+1]+=(r0+(r1-r0)*frac)*gr;
            s->pos+=(double)s->step;
        }
    }
    ps3a_unlock();
    for(k=0;k<PS3A_BLOCK*2;k++) {
        if(out[k]>1.0f) out[k]=1.0f;
        else if(out[k]<-1.0f) out[k]=-1.0f;
    }
}

qboolean S_PS3AUDIO_StartStream(int slot,const char *filename,int loopCount,
    unsigned int offsetFrames,float volume,float pitch,qboolean local,
    const vec3_t origin,int entnum,int entchannel,float minDist,float maxDist)
{
    ps3a_stream_t *s;
    snd_stream_t *d = NULL;
    ps3a_speech_cache_take_t speech;
    bool fromSpeechCache = false;
    int rate=0,width=0,chans=0;
    if(!g_up || slot<0 || slot>=PS3A_STREAM_COUNT || !filename || !filename[0]) return qfalse;
    ps3a_stream_stop(slot);
    memset(&speech, 0, sizeof(speech));
    if (offsetFrames == 0) {
        fromSpeechCache = ps3a_speech_cache_take(filename, &speech);
    }
    if (fromSpeechCache) {
        rate = speech.rate;
        width = 2;
        chans = speech.channels;
    } else {
        d=ps3a_stream_open_decoder(filename,&rate,&width,&chans,offsetFrames);
    }
    if(!d && !fromSpeechCache) {
        PS3_BootTracef("PS3AUDIO: could not open stream slot=%d file=%s",slot,filename);
        return qfalse;
    }
    s=&g_streams[slot];
    ps3a_lock();
    memset(s,0,sizeof(*s));
    s->decoder=d;
    s->speechPcm = fromSpeechCache ? speech.pcm : NULL;
    s->speechFrames = fromSpeechCache ? speech.frames : 0;
    s->speechReadFrame = 0;
    Q_strncpyz(s->filename,filename,sizeof(s->filename));
    s->rate=rate; s->width=width; s->chans=chans;
    s->loopCount=loopCount;
    s->pitch=(pitch>0.0f?pitch:1.0f);
    s->step=(float)rate*s->pitch/(float)PS3A_OUT_RATE;
    s->volume=volume<0.0f?1.0f:volume;
    s->local=local?qtrue:qfalse;
    s->entnum=entnum; s->entchannel=entchannel;
    s->minDist=minDist>1.0f?minDist:200.0f;
    s->maxDist=maxDist>=s->minDist?maxDist:12800.0f;
    s->startTime=++g_tick;
    if(origin) VectorCopy(origin,s->origin); else VectorClear(s->origin);
    ps3a_unlock();
    ps3a_stream_refill(slot);
    if (fromSpeechCache) {
        PS3_BootTracef("PS3AUDIO: speech stream cache hit file=%s frames=%u rate=%d channels=%d",
                       filename, (unsigned)speech.frames, rate, chans);
    } else {
        PS3_BootTracef("PS3AUDIO: stream PCM decoder=%s rate=%d width=%d channels=%d input_order=host-native ring=host-native",
                       filename, rate, width, chans);
    }
    ps3a_lock();
    s->active=s->writeFrame>0;
    ps3a_unlock();
    return s->active?qtrue:qfalse;
}

void S_PS3AUDIO_StopStream(int slot) { ps3a_stream_stop(slot); }
void S_PS3AUDIO_PauseStream(int slot,qboolean pause)
{
    if(slot<0 || slot>=PS3A_STREAM_COUNT) return;
    ps3a_lock(); g_streams[slot].paused=pause?qtrue:qfalse; ps3a_unlock();
}
void S_PS3AUDIO_SetStreamVolume(int slot,float volume)
{
    if(slot<0 || slot>=PS3A_STREAM_COUNT) return;
    ps3a_lock(); g_streams[slot].volume=volume<0.0f?0.0f:volume; ps3a_unlock();
}
unsigned int S_PS3AUDIO_GetStreamPosition(int slot)
{
    unsigned int ms=0;
    if(slot<0 || slot>=PS3A_STREAM_COUNT) return 0;
    ps3a_lock();
    if(g_streams[slot].rate>0) ms=(unsigned int)(g_streams[slot].pos*1000.0/(double)g_streams[slot].rate);
    ps3a_unlock();
    return ms;
}
qboolean S_PS3AUDIO_IsStreamActive(int slot)
{
    qboolean active;
    if(slot<0 || slot>=PS3A_STREAM_COUNT) return qfalse;
    ps3a_lock(); active=g_streams[slot].active?qtrue:qfalse; ps3a_unlock();
    return active;
}

qboolean S_PS3AUDIO_IsSoundPlaying(int channel_number, const char *sfxName)
{
    qboolean playing = qfalse;

    if (!g_up || !sfxName || !sfxName[0]) {
        return qfalse;
    }

    ps3a_lock();
    if (channel_number >= 0 && channel_number < PS3A_MAX_VOICES) {
        const ps3a_voice_t *v = &g_voice[channel_number];
        if (v->active && v->sfxHandle > 0 && v->sfxHandle < s_numSfx
            && !Q_stricmp(s_knownSfx[v->sfxHandle].name, sfxName)) {
            playing = qtrue;
        }
    } else {
        const int slot = channel_number - PS3A_SOUND_STREAM_CHANNEL_BASE;
        if (slot >= PS3A_STREAM_FIRST_SFX && slot < PS3A_STREAM_COUNT) {
            const ps3a_stream_t *s = &g_streams[slot];
            if (s->active && s->sfxHandle > 0 && s->sfxHandle < s_numSfx
                && !Q_stricmp(s_knownSfx[s->sfxHandle].name, sfxName)) {
                playing = qtrue;
            }
        }
    }
    ps3a_unlock();
    return playing;
}

static void ps3a_mix_block(float *out)
{
    int k;

    if (g_ps3_test_tone) {
        const double phaseStep = 6.28318530717958647692 * 440.0 / PS3A_OUT_RATE;
        for (k = 0; k < PS3A_BLOCK; k++) {
            const float sample = (float)(sinf((float)g_ps3_test_phase) * 0.20f);
            out[k * 2] = sample;
            out[k * 2 + 1] = sample;
            g_ps3_test_phase += phaseStep;
            if (g_ps3_test_phase >= 6.28318530717958647692) {
                g_ps3_test_phase -= 6.28318530717958647692;
            }
        }
        g_mix_blocks_total++;
        g_mix_blocks_testtone++;
    } else {
#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
        if (ps3a_mix_block_spu(out)) {
            g_mix_blocks_total++;
            g_mix_blocks_spu++;
        } else {
            g_mix_blocks_fallback++;
            ps3a_mix_block_ppu(out);
            ps3a_mix_streams(out);
            g_mix_blocks_total++;
            g_mix_blocks_ppu++;
        }
#else
        ps3a_mix_block_ppu(out);
        ps3a_mix_streams(out);
        g_mix_blocks_total++;
        g_mix_blocks_ppu++;
#endif
    }

    /* Measure the actual float samples written into libaudio's ring. */
    for (k = 0; k < PS3A_BLOCK * 2; k++) {
        const float sample = out[k];
        const float magnitude = fabsf(sample);
        if (magnitude > g_audio_diag_peak) g_audio_diag_peak = magnitude;
        g_audio_diag_sumsq += (double)sample * (double)sample;
    }
    g_audio_diag_samples += PS3A_BLOCK * 2;

    /* One diagnostic every 1024 output blocks (~5.5 s at 48 kHz). */
    if ((g_mix_blocks_total & 1023u) == 0) {
        const double rms = g_audio_diag_samples
            ? sqrt(g_audio_diag_sumsq / (double)g_audio_diag_samples) : 0.0;
        PS3_BootTraceFastf(
            "PS3AUDIO: mix blocks total=%u spu=%u ppu=%u fallback=%u tone=%u peak=%.4f rms=%.4f ring=%llu late=%llu",
            (unsigned)g_mix_blocks_total,
            (unsigned)g_mix_blocks_spu,
            (unsigned)g_mix_blocks_ppu,
            (unsigned)g_mix_blocks_fallback,
            (unsigned)g_mix_blocks_testtone,
            (double)g_audio_diag_peak, rms,
            (unsigned long long)g_audio_blocks,
            (unsigned long long)g_audio_late_blocks);
        g_audio_diag_peak = 0.0f;
        g_audio_diag_sumsq = 0.0;
        g_audio_diag_samples = 0;
    }
}

static void ps3a_thread(void *arg)
{
    volatile u64 *readPtr = (volatile u64 *)(uintptr_t)g_cfg.readIndex;
    float        *ring    = (float *)(uintptr_t)g_cfg.audioDataStart;
    const u32     nb      = (u32)g_cfg.numBlocks;
    const u32     blockFloats = PS3A_BLOCK * 2;
    uint64_t      readAbs = (uint64_t)*readPtr;
    uint64_t      nextAbs = readAbs + 1;
    u32           lastRd  = (u32)(readAbs % nb);

    (void)arg;

    while (g_run) {
        uint64_t nowAbs;
        u32 nowMod;

        /* libaudio notifies this queue when it consumes a block. This avoids
         * the old 1.5 ms polling loop, which could be starved by the renderer. */
        if (g_audio_notify_ready) {
            sys_event_t event;
            (void)sysEventQueueReceive(g_audio_queue, &event, 100000);
        } else {
            usleep(1000);
        }

        nowAbs = (uint64_t)*readPtr;
        nowMod = (u32)(nowAbs % nb);
        readAbs += (nowMod + nb - lastRd) % nb;
        lastRd = nowMod;

        if (nextAbs <= readAbs) {
            /* The reader overtook the producer. Skip consumed blocks, count
             * the miss, and refill only future blocks (never overwrite audio
             * currently being scanned out by libaudio). */
            g_audio_late_blocks++;
            nextAbs = readAbs + 1;
        }

        while (nextAbs <= readAbs + PS3A_LEAD) {
            const u32 block = (u32)(nextAbs % nb);
            ps3a_mix_block(ring + (size_t)block * blockFloats);
            nextAbs++;
            g_audio_blocks++;
        }
    }

    sysThreadExit(0);
}

/* ---------------------------------------------------------------------------
   Driver interface
--------------------------------------------------------------------------- */

extern "C" {

void PS3_CompatMusicUpdate(void);
void PS3_CompatAudioInit(void);

void S_PS3AUDIO_BeginSpeechCache(const char *levelName)
{
    ps3a_speech_cache_begin_internal(levelName);
}

int S_PS3AUDIO_PrecacheSpeech(const char *filename)
{
    return ps3a_speech_cache_add_internal(filename);
}

void S_PS3AUDIO_EndSpeechCache(void)
{
    ps3a_speech_cache_end_internal();
}

void S_PS3AUDIO_ClearSpeechCache(void)
{
    ps3a_speech_cache_clear_internal();
}

qboolean S_PS3AUDIO_Init()
{
    FILE *f;
    s32   rc;

    f = fopen(PS3_USRDIR "/sound.txt", "rb");
    if (f) {
        const int c = fgetc(f);
        fclose(f);
        if (c == '0') {
            PS3_BootTracef("PS3AUDIO: disabled by USRDIR/sound.txt");
            return qfalse;
        }
    }

    if (!s_ps3_speech_cache_mb) {
        s_ps3_speech_cache_mb = Cvar_Get("s_ps3_speech_cache_mb", "8", CVAR_ARCHIVE);
    }

    if (!g_codecs_inited) {
        S_CodecInit();
        g_codecs_inited = true;
        PS3_BootTracef("PS3AUDIO: registered WAV/MP3/Vorbis/Opus stream decoders");
    }

    if (g_inited) {
        g_up = true; /* the port and the thread live for the whole run */
        PS3_CompatAudioInit();
        return qtrue;
    }

    memset(g_voice, 0, sizeof(g_voice));

    if (!g_lockMade) {
        sys_mutex_attr_t attr;
        sysMutexAttrInitialize(attr);
        PS3_BootTracef("PS3AUDIO: about to call sysMutexCreate");
        if (sysMutexCreate(&g_lock, &attr) != 0) {
            PS3_BootTracef("PS3AUDIO: sysMutexCreate failed");
            return qfalse;
        }
        g_lockMade = true;
    }

    PS3_BootTracef("PS3AUDIO: about to call audioInit");
    rc = audioInit();
    PS3_BootTracef("PS3AUDIO: audioInit rc=0x%x", (unsigned)rc);
    if (rc != 0) {
        return qfalse;
    }

    {
        audioPortParam param;

        param.numChannels = AUDIO_PORT_2CH;
        param.numBlocks   = AUDIO_BLOCK_16;
        param.attrib      = AUDIO_PORT_INITLEVEL;
        param.level       = 1.0f;

        PS3_BootTracef("PS3AUDIO: about to call audioPortOpen");
        rc = audioPortOpen(&param, &g_port);
        PS3_BootTracef("PS3AUDIO: audioPortOpen rc=0x%x port=%u", (unsigned)rc, (unsigned)g_port);
        if (rc != 0) {
            audioQuit();
            return qfalse;
        }
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
    PS3_BootTracef("PS3AUDIO: about to call audioGetPortConfig");
    rc = audioGetPortConfig(g_port, &g_cfg);
    PS3_BootTracef("PS3AUDIO: audioGetPortConfig rc=0x%x channels=%u blocks=%u data=0x%x readIndex=0x%x",
                   (unsigned)rc, (unsigned)g_cfg.channelCount, (unsigned)g_cfg.numBlocks,
                   (unsigned)g_cfg.audioDataStart, (unsigned)g_cfg.readIndex);
    if (rc != 0 || g_cfg.channelCount != 2 || g_cfg.numBlocks < 4 || !g_cfg.audioDataStart || !g_cfg.readIndex) {
        audioPortClose(g_port);
        audioQuit();
        return qfalse;
    }

    /* Register the libaudio block notification queue before starting the port.
     * The producer thread uses this event instead of timer polling. */
    PS3_BootTracef("PS3AUDIO: about to create block notification queue");
    rc = audioCreateNotifyEventQueue(&g_audio_queue, &g_audio_queue_key);
    PS3_BootTracef("PS3AUDIO: audioCreateNotifyEventQueue rc=0x%x", (unsigned)rc);
    if (rc != 0) {
        audioPortClose(g_port);
        audioQuit();
        return qfalse;
    }
    rc = audioSetNotifyEventQueue(g_audio_queue_key);
    PS3_BootTracef("PS3AUDIO: audioSetNotifyEventQueue rc=0x%x", (unsigned)rc);
    if (rc != 0) {
        sysEventQueueDestroy(g_audio_queue, 0);
        audioPortClose(g_port);
        audioQuit();
        return qfalse;
    }
    (void)sysEventQueueDrain(g_audio_queue);
    g_audio_notify_ready = true;

    /* start from silence */
    memset((void *)(uintptr_t)g_cfg.audioDataStart, 0,
           (size_t)g_cfg.numBlocks * PS3A_BLOCK * 2 * sizeof(float));

    PS3_BootTracef("PS3AUDIO: about to call audioPortStart");
    rc = audioPortStart(g_port);
    PS3_BootTracef("PS3AUDIO: audioPortStart rc=0x%x", (unsigned)rc);
    if (rc != 0) {
        (void)audioRemoveNotifyEventQueue(g_audio_queue_key);
        (void)sysEventQueueDestroy(g_audio_queue, 0);
        g_audio_notify_ready = false;
        audioPortClose(g_port);
        audioQuit();
        return qfalse;
    }

    g_run = 1;
    PS3_BootTracef("PS3AUDIO: about to create the audio thread");

#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
    if (!g_spu_mix_stage) {
        g_spu_mix_stage = (uint8_t *)memalign(128, PS3A_SPU_MAX_VOICES * PS3A_SPU_STAGE_PER_VOICE);
    }
    if (g_spu_mix_stage) {
        (void)spukit_init(1);
        g_spu_mix_worker = spukit_worker_start(ps3_spukit_mix_bin, "mohaa-audio", 0);
    }
    PS3_BootTracef("PS3AUDIO: experimental SPU mixer %s",
                   g_spu_mix_worker ? "available" : "unavailable; PPU fallback");
#else
    PS3_BootTracef("PS3AUDIO: using PPU mixer (SPU path disabled for output validation)");
#endif

    rc = sysThreadCreate(&g_thread, ps3a_thread, NULL, 100, 0x10000, 0, (char *)"ps3audio");
    PS3_BootTracef("PS3AUDIO: sysThreadCreate rc=0x%x", (unsigned)rc);
    if (rc != 0) {
        g_run = 0;
        audioPortStop(g_port);
        (void)audioRemoveNotifyEventQueue(g_audio_queue_key);
        (void)sysEventQueueDestroy(g_audio_queue, 0);
        g_audio_notify_ready = false;
        audioPortClose(g_port);
        audioQuit();
        return qfalse;
    }

    g_inited = true;
    g_up     = true;
    PS3_BootTracef("PS3AUDIO: up (48 kHz stereo, %u blocks)", (unsigned)g_cfg.numBlocks);
    PS3_BootTracef("PS3AUDIO: startup mode=%s", g_ps3_test_tone ? "440Hz test tone" : "normal game mix");
    PS3_BootTracef("PS3AUDIO: normal mixer headroom=%.2f (%.1f dB)",
                   (double)PS3A_MIX_HEADROOM, 20.0 * log10((double)PS3A_MIX_HEADROOM));
    PS3_CompatAudioInit();
    Cmd_AddCommand("ps3audiotest", ps3a_testtone_cmd);
    return qtrue;
}

void S_PS3AUDIO_Shutdown()
{
    int i;
    /* Keep libaudio alive over map changes, but release every decoded stream. */
    for (i = 0; i < PS3A_STREAM_COUNT; i++) ps3a_stream_stop(i);
    S_PS3AUDIO_ClearSpeechCache();
    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) ps3a_free_voice(&g_voice[i]);
    ps3a_unlock();
    g_up = false;
}

void S_PS3AUDIO_StartSound(
    const vec3_t origin,
    int          entnum,
    int          entchannel,
    sfxHandle_t  sfxHandle,
    float        volume,
    float        min_dist,
    float        pitch,
    float        max_dist,
    qboolean     stream
)
{
    ps3a_voice_t *v;
    sfx_t        *sfx;
    qboolean      doCallback = (entnum & S_FLAG_DO_CALLBACK) ? qtrue : qfalse;
    int           callbackChannel = -1;

    entnum &= ~S_FLAG_DO_CALLBACK;
    if (sfxHandle <= 0 || sfxHandle >= s_numSfx) {
        return;
    }
    sfx = &s_knownSfx[sfxHandle];

    if (!g_up) {
        if (doCallback) {
            PS3_CompatSoundCallback(entnum, -1, sfx->name);
        }
        return;
    }

    if (stream || (sfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3))) {
        int slot = -1, i;
        vec3_t streamOrigin;
        bool local;

        if (entnum < 0 || entnum >= MAX_GENTITIES) entnum = 0;
        local = (entchannel == CHAN_MENU || entchannel == CHAN_LOCAL
                 || (entnum == s_iListenerNumber && entnum != 0)
                 || (!origin && entnum == 0));
        if (origin) VectorCopy(origin, streamOrigin);
        else VectorCopy(s_entity[entnum].position, streamOrigin);

        ps3a_lock();
        for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
            if (g_streams[i].active && entchannel != CHAN_AUTO
                && g_streams[i].entnum == entnum && g_streams[i].entchannel == entchannel) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
                if (!g_streams[i].active) { slot = i; break; }
            }
        }
        if (slot < 0) {
            int oldest = 0x7fffffff;
            for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
                if (!g_streams[i].isLoopFx && g_streams[i].startTime < oldest) {
                    oldest = g_streams[i].startTime;
                    slot = i;
                }
            }
        }
        ps3a_unlock();

        if (slot >= 0) {
            float md = min_dist < 0.0f ? 200.0f : min_dist;
            float xd = min_dist < 0.0f ? 12800.0f : max_dist;
            if (volume < 0.0f) volume = 1.0f;
            if (pitch <= 0.0f) pitch = 1.0f;

            if (S_PS3AUDIO_StartStream(slot, sfx->name, 1, 0, volume, pitch,
                                       local ? qtrue : qfalse, streamOrigin,
                                       entnum, entchannel, md, xd)) {
                ps3a_lock();
                g_streams[slot].sfxHandle = sfxHandle;
                g_streams[slot].follow = (!origin && entnum != 0) ? true : false;
                ps3a_unlock();
            }
            callbackChannel = PS3A_SOUND_STREAM_CHANNEL_BASE + slot;
        }

        /* Like OpenAL, notify the game when a callback-enabled sound is
         * assigned a channel. If decoding failed, the query below returns
         * false and the script's sounddone wait completes instead of hanging. */
        if (doCallback) {
            PS3_CompatSoundCallback(entnum, callbackChannel, sfx->name);
        }
        return;
    }

    if (!ps3a_sfx_usable(sfx)) {
        if (doCallback) {
            PS3_CompatSoundCallback(entnum, -1, sfx->name);
        }
        return;
    }

    if (entnum < 0 || entnum >= MAX_GENTITIES) {
        entnum = 0;
    }
    if (min_dist < 0.0f) {
        min_dist = 200.0f;
        max_dist = 12800.0f;
    }
    if (max_dist < min_dist) {
        max_dist = min_dist;
    }
    if (volume < 0.0f) {
        volume = 1.0f;
    }
    if (pitch <= 0.0f) {
        pitch = 1.0f;
    }

    ps3a_lock();
    v = ps3a_pick_voice(sfxHandle, entnum, entchannel, false);
    if (v) {
        callbackChannel = (int)(v - g_voice);
        memset(v, 0, sizeof(*v));
        if (ps3a_setup_voice(v, sfxHandle, pitch)) {
            v->sfxHandle  = sfxHandle;
            v->entnum     = entnum;
            v->entchannel = entchannel;
            v->volume     = volume;
            v->minDist    = min_dist > 1.0f ? min_dist : 1.0f;
            v->maxDist    = max_dist;
            v->startTime  = ++g_tick;
            v->local      = (entchannel == CHAN_MENU || entchannel == CHAN_LOCAL
                             || (entnum == s_iListenerNumber && entnum != 0));
            if (origin) {
                VectorCopy(origin, v->origin);
            } else {
                VectorCopy(s_entity[entnum].position, v->origin);
                v->follow = true;
            }
            v->active = true;
        } else {
            ps3a_free_voice(v);
        }
    }
    ps3a_unlock();

    if (doCallback) {
        PS3_CompatSoundCallback(entnum, callbackChannel, sfx->name);
    }
}

void S_PS3AUDIO_AddLoopingSound(
    const vec3_t origin,
    const vec3_t velocity,
    sfxHandle_t  sfxHandle,
    float        volume,
    float        min_dist,
    float        max_dist,
    float        pitch,
    int          flags
)
{
    ps3a_voice_t *v = NULL;
    sfx_t        *sfx;
    int           i;

    (void)velocity;
    if (!g_up || sfxHandle <= 0 || sfxHandle >= s_numSfx) {
        return;
    }
    sfx = &s_knownSfx[sfxHandle];
    if (sfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3)) {
        int slot = -1, i;
        ps3a_lock();
        for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
            if (g_streams[i].active && g_streams[i].isLoopFx
                && g_streams[i].sfxHandle == sfxHandle) {
                vec3_t delta;
                VectorSubtract(g_streams[i].origin, origin, delta);
                if (DotProduct(delta, delta) < 4.0f) {
                    slot = i;
                    g_streams[i].seen = true;
                    g_streams[i].volume = volume;
                    break;
                }
            }
        }
        if (slot < 0) {
            for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
                if (!g_streams[i].active) { slot = i; break; }
            }
        }
        ps3a_unlock();
        if (slot >= 0 && !S_PS3AUDIO_IsStreamActive(slot)) {
            if (min_dist < 0.0f) { min_dist = 200.0f; max_dist = 12800.0f; }
            if (volume < 0.0f) volume = 1.0f;
            if (pitch <= 0.0f) pitch = 1.0f;
            if (S_PS3AUDIO_StartStream(slot, sfx->name, 0, 0, volume,
                    pitch, qfalse, origin, 0, CHAN_AUTO,
                    min_dist > 1.0f ? min_dist : 200.0f,
                    max_dist >= min_dist ? max_dist : 12800.0f)) {
                ps3a_lock();
                g_streams[slot].isLoopFx = true;
                g_streams[slot].seen = true;
                g_streams[slot].sfxHandle = sfxHandle;
                ps3a_unlock();
            }
        }
        return;
    }
    if (!ps3a_sfx_usable(sfx)) {
        return;
    }
    if (min_dist < 0.0f) {
        min_dist = 200.0f;
        max_dist = 12800.0f;
    }
    if (volume < 0.0f) {
        volume = 1.0f;
    }
    if (pitch <= 0.0f) {
        pitch = 1.0f;
    }
    volume *= (s_ambientVolume ? s_ambientVolume->value : 1.0f);

    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_voice_t *c = &g_voice[i];
        vec3_t        d;

        if (c->active && c->isLoopFx && c->sfxHandle == sfxHandle) {
            VectorSubtract(c->origin, origin, d);
            if (DotProduct(d, d) < 4.0f) {
                v = c;
                break;
            }
        }
    }

    if (v) {
        v->seen   = true;
        v->volume = volume;
    } else {
        v = ps3a_pick_voice(sfxHandle, 0, CHAN_AUTO, true);
        if (v) {
            memset(v, 0, sizeof(*v));
            if (ps3a_setup_voice(v, sfxHandle, pitch)) {
                v->sfxHandle  = sfxHandle;
                v->loop       = true;
                v->isLoopFx   = true;
                v->seen       = true;
                v->volume     = volume;
                v->minDist    = min_dist > 1.0f ? min_dist : 1.0f;
                v->maxDist    = max_dist < min_dist ? min_dist : max_dist;
                v->startTime  = ++g_tick;
                v->local      = false; /* loops stay positional (flags such as NO_PAN are ignored) */
                (void)flags;
                VectorCopy(origin, v->origin);
                v->active = true;
            } else {
                ps3a_free_voice(v);
            }
        }
    }
    ps3a_unlock();
}

void S_PS3AUDIO_ClearLoopingSounds()
{
    int i;

    if (!g_up) {
        return;
    }
    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        if (g_voice[i].active && g_voice[i].isLoopFx) g_voice[i].seen = false;
    }
    for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
        if (g_streams[i].active && g_streams[i].isLoopFx) g_streams[i].seen = false;
    }
    ps3a_unlock();
}

void S_PS3AUDIO_StopSound(int entnum, int channel)
{
    int i;

    if (!g_up) {
        return;
    }
    entnum &= ~S_FLAG_DO_CALLBACK;
    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_voice_t *v = &g_voice[i];
        if (v->active && !v->isLoopFx && v->entnum == entnum && (channel == CHAN_AUTO || v->entchannel == channel))
            ps3a_free_voice(v);
    }
    for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
        if (g_streams[i].active && !g_streams[i].isLoopFx && g_streams[i].entnum == entnum
            && (channel == CHAN_AUTO || g_streams[i].entchannel == channel))
            g_streams[i].active = false;
    }
    ps3a_unlock();
}

void S_PS3AUDIO_StopAllSounds(qboolean stop_music)
{
    int i;
    for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) ps3a_stream_stop(i);
    ps3a_stream_stop(2);
    if (stop_music) {
        ps3a_stream_stop(0);
        ps3a_stream_stop(1);
    }
    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) ps3a_free_voice(&g_voice[i]);
    ps3a_unlock();
}

void S_PS3AUDIO_Respatialize(int entityNum, const vec3_t head, const vec3_t axis[3])
{
    (void)entityNum;
    if (!g_up) {
        return;
    }
    ps3a_lock();
    VectorCopy(head, g_lpos);
    VectorCopy(axis[0], g_laxis[0]);
    VectorCopy(axis[1], g_laxis[1]);
    VectorCopy(axis[2], g_laxis[2]);
    ps3a_unlock();
}

void S_PS3AUDIO_SetReverb(int, float) {}

void S_PS3AUDIO_Update()
{
    int i;
    if (!g_up) return;

    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_voice_t *v = &g_voice[i];
        if (v->active) {
            if (v->isLoopFx && !v->seen) { ps3a_free_voice(v); continue; }
            if (v->follow && v->entnum >= 0 && v->entnum < MAX_GENTITIES)
                VectorCopy(s_entity[v->entnum].position, v->origin);
        } else if (v->own) {
            ps3a_free_voice(v);
        }
    }
    for (i = PS3A_STREAM_FIRST_SFX; i < PS3A_STREAM_COUNT; i++) {
        if (g_streams[i].active && g_streams[i].isLoopFx && !g_streams[i].seen)
            g_streams[i].active = false;
        if (g_streams[i].active && g_streams[i].follow
            && g_streams[i].entnum >= 0 && g_streams[i].entnum < MAX_GENTITIES)
            VectorCopy(s_entity[g_streams[i].entnum].position, g_streams[i].origin);
    }
    ps3a_unlock();

    for (i = 0; i < PS3A_STREAM_COUNT; i++) {
        snd_stream_t *finished = NULL;
        ps3a_stream_refill(i);
        ps3a_lock();
        if (!g_streams[i].active
            && (g_streams[i].decoder || g_streams[i].speechPcm)) {
            finished = g_streams[i].decoder;
            g_streams[i].decoder = NULL;
            g_streams[i].speechPcm = NULL;
            g_streams[i].speechFrames = 0;
            g_streams[i].speechReadFrame = 0;
            g_streams[i].filename[0] = '\0';
            g_streams[i].eof = true;
        }
        ps3a_unlock();
        if (finished) S_CodecCloseStream(finished);
    }
    PS3_CompatMusicUpdate();
}

const char *S_PS3AUDIO_GetMusicFilename()
{
    return g_streams[0].filename;
}

int S_PS3AUDIO_GetMusicLoopCount()
{
    return g_streams[0].loopCount;
}

unsigned int S_PS3AUDIO_GetMusicOffset()
{
    unsigned int offset;
    ps3a_lock();
    offset = (unsigned int)g_streams[0].pos;
    ps3a_unlock();
    return offset;
}

} /* extern "C" */
