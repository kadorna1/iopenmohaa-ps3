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
#ifdef __cplusplus
}
#endif
#define PS3_STEP(msg) PS3_BootTracef("STEP: " msg " (line %d)", __LINE__)
/* PS3_STEPF("fmt", args...): printf-style and cheap (no fsync): for loops. */
#define PS3_STEPF(...) PS3_BootTraceFastf("STEP: " __VA_ARGS__)
#else
#define PS3_STEP(msg) ((void)0)
#define PS3_STEPF(...) ((void)0)
#endif

#endif
