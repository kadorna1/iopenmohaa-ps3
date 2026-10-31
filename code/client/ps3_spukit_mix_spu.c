/* SPDX-License-Identifier: MIT */
#include <spukit/spu.h>
#include <sys/spu_thread.h>
#include "ps3_spukit_mix.h"
#define DMA_TAG 0u
static ps3a_spu_voice_t voices[PS3A_SPU_MAX_VOICES] __attribute__((aligned(128)));
static uint8_t pcm[PS3A_SPU_PCM_LOCAL_BYTES] __attribute__((aligned(128)));
static float output[PS3A_SPU_BLOCK_FRAMES * 2] __attribute__((aligned(128)));
static uint32_t align16(uint32_t n) { return (n + 15u) & ~15u; }
static float sample_at(const ps3a_spu_voice_t *v, uint32_t frame, uint32_t ch, uint32_t second_ls_off)
{
    const uint8_t *p;
    const uint32_t bpf = v->width * v->chans;
    uint32_t local_frame;
    if (frame >= v->first_start && frame < v->first_start + v->first_frames) {
        local_frame = frame - v->first_start;
        p = pcm + local_frame * bpf;
    } else if (frame < v->second_frames) {
        local_frame = frame;
        p = pcm + second_ls_off + local_frame * bpf;
    } else return 0.0f;
    if (v->width == 2u) {
        const int16_t s = (int16_t)((uint16_t)p[ch < v->chans ? ch * 2 : 0] |
                                    ((uint16_t)p[ch < v->chans ? ch * 2 + 1 : 1] << 8));
        return (float)s * (1.0f / 32768.0f);
    }
    return (float)((int)p[ch < v->chans ? ch : 0] - 128) * (1.0f / 128.0f);
}
static int mix_handler(void *ctx, const spukit_job *job, spukit_result *res)
{
    (void)ctx;
    if (job->kind != PS3A_SPU_JOB_MIX || job->size < sizeof(ps3a_spu_mix_job_t)) return -1;
    ps3a_spu_mix_job_t p;
    __builtin_memcpy(&p, job->data, sizeof p);
    if (p.voice_count != PS3A_SPU_MAX_VOICES || p.output_frames != PS3A_SPU_BLOCK_FRAMES) return -2;
    spukit_dma_get(voices, p.voices_ea, sizeof voices, DMA_TAG);
    spukit_dma_wait(1u << DMA_TAG);
    for (uint32_t k = 0; k < PS3A_SPU_BLOCK_FRAMES * 2u; k++) output[k] = 0.0f;
    for (uint32_t i = 0; i < PS3A_SPU_MAX_VOICES; i++) {
        ps3a_spu_voice_t *v = &voices[i];
        if (!v->active || !v->frames || !v->first_frames ||
            (v->width != 1u && v->width != 2u) ||
            (v->chans != 1u && v->chans != 2u)) continue;
        uint32_t bpf = v->width * v->chans;
        uint32_t first_bytes = align16(v->first_frames * bpf);
        uint32_t second_bytes = align16(v->second_frames * bpf);
        uint32_t second_ls_off = first_bytes;
        if (first_bytes + second_bytes > sizeof pcm) return -3;
        spukit_dma_get(pcm, p.stage_ea + v->first_off, first_bytes, DMA_TAG);
        spukit_dma_wait(1u << DMA_TAG);
        if (v->second_frames) {
            spukit_dma_get(pcm + second_ls_off, p.stage_ea + v->second_off, second_bytes, DMA_TAG);
            spukit_dma_wait(1u << DMA_TAG);
        }
        for (uint32_t k = 0; k < PS3A_SPU_BLOCK_FRAMES; k++) {
            uint32_t f0 = (uint32_t)(v->pos_q16 >> 16);
            if (f0 >= v->frames) {
                if (v->loop) {
                    v->pos_q16 -= (uint64_t)v->frames << 16;
                    f0 = (uint32_t)(v->pos_q16 >> 16);
                    if (f0 >= v->frames) { v->active = 0; break; }
                } else { v->active = 0; break; }
            }
            uint32_t f1 = f0 + 1u;
            if (f1 >= v->frames) f1 = v->loop ? 0u : f0;
            const float frac = (float)(v->pos_q16 & 0xFFFFu) * (1.0f / 65536.0f);
            float l0 = sample_at(v, f0, 0, second_ls_off);
            float l1 = sample_at(v, f1, 0, second_ls_off);
            float left = l0 + (l1 - l0) * frac;
            float right = left;
            if (v->chans == 2u) {
                float r0 = sample_at(v, f0, 1, second_ls_off);
                float r1 = sample_at(v, f1, 1, second_ls_off);
                right = r0 + (r1 - r0) * frac;
            }
            output[k * 2u] += left * v->gain_l;
            output[k * 2u + 1u] += right * v->gain_r;
            v->gain_l += v->delta_l;
            v->gain_r += v->delta_r;
            v->pos_q16 += v->step_q16;
        }
        v->gain_l = v->target_l;
        v->gain_r = v->target_r;
    }
    for (uint32_t k = 0; k < PS3A_SPU_BLOCK_FRAMES * 2u; k++) {
        if (output[k] > 1.0f) output[k] = 1.0f;
        else if (output[k] < -1.0f) output[k] = -1.0f;
    }
    spukit_dma_put(output, p.output_ea, sizeof output, DMA_TAG);
    spukit_dma_put(voices, p.voices_ea, sizeof voices, DMA_TAG);
    res->error = 0;
    return 0;
}
int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
    (void)arg; (void)a2; (void)a3;
    spukit_run(ctrl_ea, mix_handler, NULL);
    spu_thread_exit(0);
    return 0;
}
