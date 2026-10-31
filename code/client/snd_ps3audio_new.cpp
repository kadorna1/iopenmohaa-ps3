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

Not implemented (silent): streamed sounds, MP3, music, movie audio, reverb and
the "wait till sound done" callbacks.

Safety: every libaudio call is logged to the early trace *before* it is made.
Create USRDIR/sound.txt containing 0 to disable the driver, e.g. if the console
refuses to start with it.
===========================================================================
*/

#include "snd_local.h"

#include <audio/audio.h>
#include <sys/mutex.h>
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

typedef struct {
    bool           active;
    bool           loop;     /* looping voice */
    bool           isLoopFx; /* created by AddLoopingSound (refreshed per frame) */
    bool           seen;     /* loop voice refreshed since ClearLoopingSounds */
    bool           local;    /* 2D: no distance attenuation, centred */
    bool           follow;   /* origin follows s_entity[entnum] */
    int            sfxHandle;
    const byte    *pcm;      /* first frame (interleaved if chans == 2) */
    short         *own;      /* private decoded buffer (ADPCM) or NULL */
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
static sys_mutex_t  g_lock;
static bool         g_lockMade   = false;
static bool         g_inited     = false; /* audio port + thread exist (kept for the whole run) */
static bool         g_up         = false; /* driver enabled for the sound system */
static volatile int g_run        = 0;
static sys_ppu_thread_t g_thread;
static u32          g_port       = 0;
static audioPortConfig g_cfg;
static int          g_tick       = 0;
/* Counters are updated by the single audio producer thread. Periodic snapshots
 * reveal whether audible output is actually being mixed on SPU or PPU. */
static uint32_t g_mix_blocks_total    = 0;
static uint32_t g_mix_blocks_spu      = 0;
static uint32_t g_mix_blocks_ppu      = 0;
static uint32_t g_mix_blocks_fallback = 0;


static vec3_t g_lpos  = {0, 0, 0};
static vec3_t g_laxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
static spukit_worker *g_spu_mix_worker;
static uint8_t *g_spu_mix_stage;
static ps3a_spu_voice_t g_spu_mix_voices[PS3A_MAX_VOICES] __attribute__((aligned(128)));
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

/* Decodes into a malloc'd 16-bit buffer. Returns the frame count (0 on failure). */
static int ps3a_decode_ima_mono(const byte *src, int bytes, int blockAlign, short **outBuf)
{
    int maxSamples, written = 0;
    short *out;

    *outBuf = NULL;
    if (blockAlign < 8 || bytes <= 0) {
        return 0;
    }

    maxSamples = ((bytes / blockAlign) + 1) * ((blockAlign - 4) * 2 + 1);
    out        = (short *)malloc((size_t)maxSamples * sizeof(short));
    if (!out) {
        return 0;
    }

    while (bytes >= 5) {
        const int blk   = bytes < blockAlign ? bytes : blockAlign;
        int       pred  = (short)(src[0] | (src[1] << 8));
        int       index = ima_clamp_index(src[2]);
        int       i;

        out[written++] = (short)pred;
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
                    out[written++] = (short)pred;
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
        short    *buf;
        const int n = (sfx->info.channels == 1) ? ps3a_decode_ima_mono(src, bytes, blockAlign, &buf) : 0;

        if (n <= 0) {
            return false;
        }
        v->own    = buf;
        v->pcm    = (const byte *)buf;
        v->frames = n;
        v->width  = 2;
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

static inline float ps3a_sample(const ps3a_voice_t *v, int frame, int ch)
{
    const byte *p = v->pcm + ((size_t)frame * v->chans + (ch < v->chans ? ch : 0)) * v->width;

    if (v->width == 2) {
        return (float)(short)(p[0] | (p[1] << 8)) * (1.0f / 32768.0f);
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
    float  master = (s_volume ? s_volume->value : 0.9f);
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

    for (k = 0; k < PS3A_BLOCK; k++) {
        float l = mixL[k], r = mixR[k];

        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
        out[k * 2]     = l;
        out[k * 2 + 1] = r;
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
        ps3a_target_gains(src, s_volume ? s_volume->value : 0.9f, &target_l, &target_r);
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

    {
        ps3a_spu_mix_job_t job;
        job.voices_ea = (uint64_t)(uintptr_t)g_spu_mix_voices;
        job.stage_ea = (uint64_t)(uintptr_t)g_spu_mix_stage;
        job.output_ea = (uint64_t)(uintptr_t)out;
        job.voice_count = PS3A_MAX_VOICES;
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
    ps3a_unlock();
    return true;

ppu_fallback:
    ps3a_unlock();
    if (!g_spu_mix_warned++) PS3_BootTracef("PS3AUDIO: voice outside experimental SPU mixer limits; using PPU mixer");
    return false;
}
#endif

static void ps3a_mix_block(float *out)
{
#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
    if (ps3a_mix_block_spu(out)) {
        g_mix_blocks_total++;
        g_mix_blocks_spu++;
    } else {
        g_mix_blocks_fallback++;
        ps3a_mix_block_ppu(out);
        g_mix_blocks_total++;
        g_mix_blocks_ppu++;
    }
#else
    ps3a_mix_block_ppu(out);
    g_mix_blocks_total++;
    g_mix_blocks_ppu++;
#endif

    /* One diagnostic every 1024 output blocks (~5.5 s at 48 kHz). */
    if ((g_mix_blocks_total & 1023u) == 0) {
        PS3_BootTraceFastf(
            "PS3AUDIO: mix blocks total=%u spu=%u ppu=%u fallback=%u",
            (unsigned)g_mix_blocks_total,
            (unsigned)g_mix_blocks_spu,
            (unsigned)g_mix_blocks_ppu,
            (unsigned)g_mix_blocks_fallback);
    }
}

static void ps3a_thread(void *arg)
{
    volatile u64 *readPtr = (volatile u64 *)(uintptr_t)g_cfg.readIndex;
    float        *ring    = (float *)(uintptr_t)g_cfg.audioDataStart;
    const u32     nb      = (u32)g_cfg.numBlocks;
    const u32     blockFloats = PS3A_BLOCK * 2;
    u32           lastRd  = (u32)*readPtr % nb;
    u32           wp      = (lastRd + PS3A_LEAD) % nb;
    int           lead    = PS3A_LEAD;

    (void)arg;

    while (g_run) {
        const u32 rd  = (u32)*readPtr % nb;
        const int adv = (int)((rd + nb - lastRd) % nb);

        lastRd = rd;
        lead -= adv;
        if (lead < 0) {
            /* underrun: the reader overtook us; resynchronise just ahead of it */
            wp   = (rd + 1) % nb;
            lead = 1;
        }
        while (lead < PS3A_LEAD) {
            ps3a_mix_block(ring + (size_t)wp * blockFloats);
            wp = (wp + 1) % nb;
            lead++;
        }
        usleep(1500);
    }

    sysThreadExit(0);
}

/* ---------------------------------------------------------------------------
   Driver interface
--------------------------------------------------------------------------- */

extern "C" {

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

    if (g_inited) {
        g_up = true; /* the port and the thread live for the whole run */
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

        memset(&param, 0, sizeof(param));
        param.numChannels = AUDIO_PORT_2CH;
        param.numBlocks   = AUDIO_BLOCK_8;
        param.attrib      = 0;
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

    /* start from silence */
    memset((void *)(uintptr_t)g_cfg.audioDataStart, 0,
           (size_t)g_cfg.numBlocks * PS3A_BLOCK * 2 * sizeof(float));

    PS3_BootTracef("PS3AUDIO: about to call audioPortStart");
    rc = audioPortStart(g_port);
    PS3_BootTracef("PS3AUDIO: audioPortStart rc=0x%x", (unsigned)rc);
    if (rc != 0) {
        audioPortClose(g_port);
        audioQuit();
        return qfalse;
    }

    g_run = 1;
    PS3_BootTracef("PS3AUDIO: about to create the audio thread");

#ifdef OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO
    if (!g_spu_mix_stage) {
        g_spu_mix_stage = (uint8_t *)memalign(128, PS3A_MAX_VOICES * PS3A_SPU_STAGE_PER_VOICE);
    }
    if (g_spu_mix_stage) {
        /* worker_start is the actual capability check; failure is non-fatal. */
        (void)spukit_init(1);
        g_spu_mix_worker = spukit_worker_start(ps3_spukit_mix_bin, "mohaa-audio", 0);
    }
    PS3_BootTracef("PS3AUDIO: experimental SPU mixer %s",
                   g_spu_mix_worker ? "available" : "unavailable; PPU fallback");
#endif

    rc = sysThreadCreate(&g_thread, ps3a_thread, NULL, 500, 0x10000, 0, (char *)"ps3audio");
    PS3_BootTracef("PS3AUDIO: sysThreadCreate rc=0x%x", (unsigned)rc);
    if (rc != 0) {
        g_run = 0;
        audioPortStop(g_port);
        audioPortClose(g_port);
        audioQuit();
        return qfalse;
    }

    g_inited = true;
    g_up     = true;
    PS3_BootTracef("PS3AUDIO: up (48 kHz stereo, %u blocks)", (unsigned)g_cfg.numBlocks);
    return qtrue;
}

void S_PS3AUDIO_Shutdown()
{
    int i;

    /* Keep the port and the thread: they cost nothing while no voice is active, and
     * tearing libaudio down and up again on every map load is needless risk. */
    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_free_voice(&g_voice[i]);
    }
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

    (void)stream;
    if (!g_up || sfxHandle <= 0 || sfxHandle >= s_numSfx) {
        return;
    }
    sfx = &s_knownSfx[sfxHandle];
    if (!ps3a_sfx_usable(sfx)) {
        return;
    }

    entnum &= ~S_FLAG_DO_CALLBACK;
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
        if (g_voice[i].active && g_voice[i].isLoopFx) {
            g_voice[i].seen = false;
        }
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

        if (v->active && !v->isLoopFx && v->entnum == entnum && (channel == CHAN_AUTO || v->entchannel == channel)) {
            ps3a_free_voice(v);
        }
    }
    ps3a_unlock();
}

void S_PS3AUDIO_StopAllSounds(qboolean stop_music)
{
    int i;

    (void)stop_music;
    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_free_voice(&g_voice[i]);
    }
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

    if (!g_up) {
        return;
    }
    ps3a_lock();
    for (i = 0; i < PS3A_MAX_VOICES; i++) {
        ps3a_voice_t *v = &g_voice[i];

        if (v->active) {
            if (v->isLoopFx && !v->seen) {
                ps3a_free_voice(v);
                continue;
            }
            if (v->follow && v->entnum >= 0 && v->entnum < MAX_GENTITIES) {
                VectorCopy(s_entity[v->entnum].position, v->origin);
            }
        } else if (v->own) {
            /* the mixer finished this voice: release its decoded buffer */
            ps3a_free_voice(v);
        }
    }
    ps3a_unlock();
}

const char *S_PS3AUDIO_GetMusicFilename()
{
    return "";
}

int S_PS3AUDIO_GetMusicLoopCount()
{
    return 0;
}

unsigned int S_PS3AUDIO_GetMusicOffset()
{
    return 0;
}

} /* extern "C" */
