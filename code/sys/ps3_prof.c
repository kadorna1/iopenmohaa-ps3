/* Minimal PS3 frame-profiler backend. */
#include <string.h>
#include "ps3_platform.h"
#include "ps3_log.h"

int ps3_prof_smp = 0;
ps3gl_stats_t ps3gl_stats;
unsigned ps3gl_tex_bytes = 0;
unsigned ps3gl_tex_failed = 0;

static unsigned long long ps3_prof_ticks[PS3_PROF_COUNT];

void PS3_ProfAdd(int slot, unsigned long long ticks)
{
    if ((unsigned)slot < PS3_PROF_COUNT)
        ps3_prof_ticks[slot] += ticks;
}

void PS3_ProfSyscall(const char *module, const intptr_t *args, unsigned long long ticks)
{
    (void)module;
    (void)args;
    (void)ticks;
}

/* The Cell PPU timebase runs at 79.8 MHz. Keep the report window long enough
 * to avoid console-log overhead while exposing the dominant render phases. */
#define PS3_PROF_REPORT_FRAMES 120u
#define PS3_PPU_TICKS_PER_MS 79800.0

void PS3_ProfReportFrame(void)
{
    static unsigned frames;
    unsigned long long sample[PS3_PROF_COUNT];
    double divisor;
    unsigned i;

    if (++frames < PS3_PROF_REPORT_FRAMES)
        return;

    for (i = 0; i < PS3_PROF_COUNT; ++i) {
        sample[i] = ps3_prof_ticks[i];
        ps3_prof_ticks[i] = 0;
    }

    divisor = (double)frames * PS3_PPU_TICKS_PER_MS;
    PS3_Logf("[fps] samples=%u smp=%d backend=%.2fms fence=%.2fms flip=%.2fms "
             "shade=%.2fms draw=%.2fms vcopy=%.2fms surf=%.2fms anim=%.2fms "
             "bones=%.2fms colors=%.2fms tc=%.2fms "
             "draws=%u direct=%u copied=%u verts=%u copyKB=%u ringKB=%u dropped=%u "
             "texMB=%u texfail=%u",
             frames, ps3_prof_smp,
             sample[PS3_PROF_BACKEND] / divisor,
             sample[PS3_PROF_FENCEWAIT] / divisor,
             sample[PS3_PROF_FLIPWAIT] / divisor,
             sample[PS3_PROF_SHADE] / divisor,
             sample[PS3_PROF_GLDRAW] / divisor,
             sample[PS3_PROF_GLCOPY] / divisor,
             sample[PS3_PROF_SURF] / divisor,
             sample[PS3_PROF_ANIM] / divisor,
             sample[PS3_PROF_BONES] / divisor,
             sample[PS3_PROF_COLORS] / divisor,
             sample[PS3_PROF_TCOORDS] / divisor,
             ps3gl_stats.draws, ps3gl_stats.direct_draws,
             ps3gl_stats.copy_draws, ps3gl_stats.copy_verts,
             ps3gl_stats.copy_bytes / 1024u, ps3gl_stats.vring_peak / 1024u,
             ps3gl_stats.dropped, ps3gl_tex_bytes / (1024u * 1024u),
             ps3gl_tex_failed);

    memset(&ps3gl_stats, 0, sizeof(ps3gl_stats));
    frames = 0;
}
