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
             "cmds=%.2f drawCmd=%.2f spriteCmd=%.2f drawList=%.2f spriteList=%.2f "
             "dispatch=%.2f sfFace=%.2f sfGrid=%.2f sfPoly=%.2f sfMark=%.2f "
             "sfFlare=%.2f sfEntity=%.2f sfDisplay=%.2f sfSkel=%.2f sfStatic=%.2f "
             "sfSwipe=%.2f sfSprite=%.2f sfTerrain=%.2f sfTriangles=%.2f "
             "cmdStretch=%.2f cmdMisc=%.2f cmdSwap=%.2f glfinish=%.2f "
             "gameEvents=%.2f gamePreAnim=%.2f gameNav=%.2f gameBot=%.2f "
             "gameEntities=%.2f gamePost=%.2f tex=%uKB texFail=%u",
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
             PS3_PROF_MS(PS3_PROF_COMMANDS), PS3_PROF_MS(PS3_PROF_DRAW_CMD),
             PS3_PROF_MS(PS3_PROF_SPRITE_CMD), PS3_PROF_MS(PS3_PROF_DRAW_LIST),
             PS3_PROF_MS(PS3_PROF_SPRITE_LIST), PS3_PROF_MS(PS3_PROF_SURF_DISPATCH),
             PS3_PROF_MS(PS3_PROF_SF_FACE), PS3_PROF_MS(PS3_PROF_SF_GRID),
             PS3_PROF_MS(PS3_PROF_SF_POLY), PS3_PROF_MS(PS3_PROF_SF_MARK),
             PS3_PROF_MS(PS3_PROF_SF_FLARE), PS3_PROF_MS(PS3_PROF_SF_ENTITY),
             PS3_PROF_MS(PS3_PROF_SF_DISPLAY), PS3_PROF_MS(PS3_PROF_SF_SKEL),
             PS3_PROF_MS(PS3_PROF_SF_STATIC), PS3_PROF_MS(PS3_PROF_SF_SWIPE),
             PS3_PROF_MS(PS3_PROF_SF_SPRITE), PS3_PROF_MS(PS3_PROF_SF_TERRAIN),
             PS3_PROF_MS(PS3_PROF_SF_TRIANGLES),
             PS3_PROF_MS(PS3_PROF_CMD_STRETCH), PS3_PROF_MS(PS3_PROF_CMD_MISC),
             PS3_PROF_MS(PS3_PROF_CMD_SWAP), PS3_PROF_MS(PS3_PROF_GL_FINISH),
             PS3_PROF_MS(PS3_PROF_GAME_EVENTS), PS3_PROF_MS(PS3_PROF_GAME_PREANIM),
             PS3_PROF_MS(PS3_PROF_GAME_NAV), PS3_PROF_MS(PS3_PROF_GAME_BOT),
             PS3_PROF_MS(PS3_PROF_GAME_ENTITIES), PS3_PROF_MS(PS3_PROF_GAME_POST),
             ps3gl_tex_bytes / 1024u, ps3gl_tex_failed);
#undef PS3_PROF_MS
}
void PS3_ProfSyscall(const char *module, const intptr_t *args, unsigned long long ticks)
{
    (void)module; (void)args; (void)ticks;
}

/* The Cell PPU timebase runs at 79.8 MHz. Keep the report window long enough
 * to avoid console-log overhead while exposing the dominant render phases. */
#define PS3_PROF_REPORT_FRAMES 120u
#define PS3_PPU_TICKS_PER_MS 79800.0

void PS3_ProfReportFrame(void)
{
    static unsigned frames;
    static unsigned long long windowStart;
    unsigned long long sample[PS3_PROF_COUNT];
    unsigned long long now;
    double divisor;
    double fps;
    unsigned i;

    if (frames == 0)
        windowStart = PS3_ProfTick();
    ++frames;
    if (frames < PS3_PROF_REPORT_FRAMES)
        return;

    now = PS3_ProfTick();
    for (i = 0; i < PS3_PROF_COUNT; ++i) {
        sample[i] = ps3_prof_ticks[i];
        ps3_prof_ticks[i] = 0;
    }

    divisor = (double)frames * PS3_PPU_TICKS_PER_MS;
    fps = (now > windowStart)
        ? ((double)(frames - 1u) * 79800000.0 / (double)(now - windowStart))
        : 0.0;
    PS3_Logf("[fps] avg=%.1f samples=%u smp=%d backend=%.2fms fence=%.2fms flip=%.2fms "
             "shade=%.2fms draw=%.2fms vcopy=%.2fms surf=%.2fms anim=%.2fms "
             "bones=%.2fms colors=%.2fms tc=%.2fms "
             "draws=%u direct=%u copied=%u verts=%u copyKB=%u ringKB=%u dropped=%u "
             "texMB=%u texfail=%u",
             fps, frames, ps3_prof_smp,
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
