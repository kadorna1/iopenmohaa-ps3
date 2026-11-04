/* PS3 frame-profiler backend. */
#include <string.h>
#include <sys/systime.h>
#include "ps3_platform.h"
#include "ps3_log.h"

int ps3_prof_smp = 0;
ps3gl_stats_t ps3gl_stats;
unsigned ps3gl_tex_bytes = 0;
unsigned ps3gl_tex_failed = 0;

#define PS3_PROF_REPORT_FRAMES 120u
static volatile unsigned long long ps3_prof_ticks[PS3_PROF_COUNT];
static unsigned ps3_prof_window_frames;
static unsigned long long ps3_prof_window_start;
static unsigned long long ps3_prof_backend_start;
static unsigned long long ps3_prof_timebase_hz;

void PS3_ProfAdd(int slot, unsigned long long ticks)
{
    if ((unsigned)slot < PS3_PROF_COUNT && ticks)
        __sync_fetch_and_add(&ps3_prof_ticks[slot], ticks);
}
static unsigned long long PS3_ProfTake(int slot)
{
    return __sync_lock_test_and_set(&ps3_prof_ticks[slot], 0ULL);
}
void PS3_ProfBackendBegin(void)
{
    const unsigned long long now = PS3_ProfTick();
    if (ps3_prof_window_frames == 0)
        ps3_prof_window_start = now;
    ps3_prof_backend_start = now;
}
void PS3_ProfBackendEnd(void)
{
    const unsigned long long now = PS3_ProfTick();
    unsigned long long total[PS3_PROF_COUNT];
    unsigned long long elapsed;
    double tick_to_ms, fps, frame_ms;
    ps3gl_stats_t stats;
    unsigned frames;
    int i;

    if (!ps3_prof_backend_start) return;
    PS3_ProfAdd(PS3_PROF_BACKEND, now - ps3_prof_backend_start);
    ps3_prof_backend_start = 0;
    if (++ps3_prof_window_frames < PS3_PROF_REPORT_FRAMES) return;

    frames = ps3_prof_window_frames;
    elapsed = now - ps3_prof_window_start;
    ps3_prof_window_frames = 0;
    ps3_prof_window_start = 0;
    if (!ps3_prof_timebase_hz)
        ps3_prof_timebase_hz = (unsigned long long)sysGetTimebaseFrequency();
    if (!ps3_prof_timebase_hz || !elapsed) return;

    for (i = 0; i < PS3_PROF_COUNT; i++) total[i] = PS3_ProfTake(i);
    stats = ps3gl_stats;
    memset(&ps3gl_stats, 0, sizeof(ps3gl_stats));

    tick_to_ms = 1000.0 / (double)ps3_prof_timebase_hz;
    fps = (double)frames * (double)ps3_prof_timebase_hz / (double)elapsed;
    frame_ms = (double)elapsed * tick_to_ms / (double)frames;
#define PS3_PROF_MS(slot) ((double)total[(slot)] * tick_to_ms / (double)frames)
    PS3_Logf("[ps3perf] n=%u renderFps=%.2f renderFrame=%.2fms "
             "sv=%.2f cl=%.2f cgame=%.2f backend=%.2f "
             "flipwait=%.2f fencewait=%.2f rwait=%.2f sound=%.2f "
             "anim=%.2f bones=%.2f surf=%.2f shade=%.2f colors=%.2f tcoords=%.2f "
             "gldraw=%.2f glcopy=%.2f draws=%.1f verts=%.1f indices=%.1f "
             "direct=%.1f copied=%.1f copyverts=%.1f ringPeak=%uKB dropped=%u "
             "tex=%uKB texFail=%u",
             frames, fps, frame_ms,
             PS3_PROF_MS(PS3_PROF_SV), PS3_PROF_MS(PS3_PROF_CL),
             PS3_PROF_MS(PS3_PROF_CGAME), PS3_PROF_MS(PS3_PROF_BACKEND),
             PS3_PROF_MS(PS3_PROF_FLIPWAIT), PS3_PROF_MS(PS3_PROF_FENCEWAIT),
             PS3_PROF_MS(PS3_PROF_RWAIT), PS3_PROF_MS(PS3_PROF_SOUND),
             PS3_PROF_MS(PS3_PROF_ANIM), PS3_PROF_MS(PS3_PROF_BONES),
             PS3_PROF_MS(PS3_PROF_SURF), PS3_PROF_MS(PS3_PROF_SHADE),
             PS3_PROF_MS(PS3_PROF_COLORS), PS3_PROF_MS(PS3_PROF_TCOORDS),
             PS3_PROF_MS(PS3_PROF_GLDRAW), PS3_PROF_MS(PS3_PROF_GLCOPY),
             (double)stats.draws / frames, (double)stats.verts / frames,
             (double)stats.indices / frames, (double)stats.direct_draws / frames,
             (double)stats.copied_draws / frames, (double)stats.copied_verts / frames,
             stats.vring_peak / 1024u, stats.dropped,
             ps3gl_tex_bytes / 1024u, ps3gl_tex_failed);
#undef PS3_PROF_MS
}
void PS3_ProfSyscall(const char *module, const intptr_t *args, unsigned long long ticks)
{
    (void)module; (void)args; (void)ticks;
}
