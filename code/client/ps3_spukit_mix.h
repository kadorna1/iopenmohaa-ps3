/* SPDX-License-Identifier: MIT */
/* Shared PPU/SPU ABI for the experimental OpenMoHAA block mixer. */
#ifndef OPENMOHAA_PS3_SPUKIT_MIX_H
#define OPENMOHAA_PS3_SPUKIT_MIX_H
#include <stdint.h>
#define PS3A_SPU_JOB_MIX 0x4D495831u
#define PS3A_SPU_MAX_VOICES 48u
#define PS3A_SPU_BLOCK_FRAMES 256u
#define PS3A_SPU_STAGE_PER_VOICE 8192u
#define PS3A_SPU_PCM_LOCAL_BYTES 8192u
typedef struct __attribute__((aligned(16))) ps3a_spu_voice {
    uint32_t first_off, second_off, first_start, first_frames, second_frames, frames;
    uint64_t pos_q16, step_q16;
    float gain_l, gain_r, delta_l, delta_r, target_l, target_r;
    uint32_t width, chans, loop, active;
} ps3a_spu_voice_t;
typedef struct __attribute__((aligned(16))) ps3a_spu_mix_job {
    uint64_t voices_ea, stage_ea, output_ea;
    uint32_t voice_count, output_frames;
} ps3a_spu_mix_job_t;
#endif
