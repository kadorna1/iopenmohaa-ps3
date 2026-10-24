/* ps3_step.h -- PS3_STEP("text"): one synced line in the early trace.
 *
 * For init paths only: every call opens, writes and fsyncs a file (see
 * PS3_BootTracef in ps3_log.c), so keep it out of per-frame code. Compiles to
 * nothing on other platforms. Kept out of ps3_platform.h on purpose: that
 * header is force-included everywhere, so touching it rebuilds the world.
 */
#ifndef PS3_STEP_H
#define PS3_STEP_H

#if defined(__PS3__)
#ifdef __cplusplus
extern "C" {
#endif
void PS3_BootTracef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void PS3_BootTraceFastf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void PS3_TraceMemory(const char *tag);
/* Zone-allocator diagnostics (qcommon/memory.c). PS3_ZoneCheck walks every zone
 * block, validating links and the trailing guard word, and logs any corrupt
 * block with its size, tag and allocator addresses. verbose=1 also logs a summary. */
int PS3_ZoneCheck(const char *where, int verbose);
int PS3_TraceLevel(void);
void PS3_DumpTraceRing(const char *why);
extern void *ps3_zone_caller_hint;
/* Frame-trace window: boot traces the first 3 frames; PS3_RearmFrameTrace(n)
 * re-opens the window for the next n frames (used when a map finishes loading,
 * so the connect/gamestate/cgame handshake frames are traced too). */
extern int ps3_frame_trace;
extern int ps3_frame_trace_limit;
extern int ps3_clframe_trace;
extern int ps3_clframe_trace_limit;
void PS3_RearmFrameTrace(int frames);
#ifdef __cplusplus
}
#endif
#define PS3_STEP(msg) PS3_BootTracef("STEP: " msg " (line %d)", __LINE__)
/* PS3_STEPF("fmt", args...): printf-style and cheap (no fsync): for loops. */
#define PS3_STEPF(...) PS3_BootTraceFastf("STEP: " __VA_ARGS__)
/* PS3_TRACEF("fmt", args...): printf-style, synced (survives a crash). Init paths only. */
#define PS3_TRACEF(...) PS3_BootTracef(__VA_ARGS__)
/* PS3_CINF("fmt", ...): cinematic crumbs; throttled to the ring + crumb file at trace level 1. */
#define PS3_CINF(...) PS3_BootTracef("CIN " __VA_ARGS__)
/* PS3_TRACEMEM("tag"): log lv2 user-memory total/available and the newlib heap in use. */
#define PS3_TRACEMEM(tag) PS3_TraceMemory(tag)
#define PS3_ZONECHECK(where) PS3_ZoneCheck((where), 1)
#define PS3_ZONECHECK_QUIET(where) PS3_ZoneCheck((where), 0)
/* PS3_FTRACE("fmt", ...): synced, but only while the frame-trace window is open. */
#define PS3_FTRACE(...) do { if (ps3_frame_trace < ps3_frame_trace_limit || PS3_TraceLevel() < 2) PS3_BootTracef("NET " __VA_ARGS__); } while (0)
#else
#define PS3_TRACEMEM(tag) ((void)0)
#define PS3_CINF(...) ((void)0)
#define PS3_ZONECHECK(where) ((void)0)
#define PS3_ZONECHECK_QUIET(where) ((void)0)
#define PS3_FTRACE(...) ((void)0)
#define PS3_TRACEF(...) ((void)0)
#define PS3_STEP(msg) ((void)0)
#define PS3_STEPF(...) ((void)0)
#endif

#endif
