/* Minimal PS3 frame-profiler backend. */
#include <string.h>
#include "ps3_platform.h"

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
