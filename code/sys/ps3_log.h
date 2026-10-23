#ifndef PS3_LOG_H
#define PS3_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void PS3_LogInit(void);
void PS3_LogShutdown(void);
void PS3_LogRaw(const char *msg);            /* engine console text, as-is */
extern int ps3_log_warnings, ps3_log_cams;  /* counted in PS3_LogRaw */
void ps3_log(const char *msg);               /* one line */
void PS3_Logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Early boot trace, independent of PS3_LogInit and its mutex.
 * Every line is written with the raw lv2 file API and fsync'd, and is also
 * sent over UDP once PS3_LogNetInit() has run, so it survives a console
 * freeze. Safe to call from static constructors (no malloc, no stdio). */
void PS3_BootTracef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Trace throttling (ps3_log.c): level 1 keeps per-frame/per-image lines in a ring buffer,
 * level 2 (USRDIR/trace_level.txt containing "2") writes everything. */
int PS3_TraceLevel(void);
void PS3_DumpTraceRing(const char *why);
/* Same, without the fsync and with a single sink: cheap enough for loops. */
void PS3_BootTraceFastf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* UDP log sink. Reads "ip[:port]" from USRDIR/netlog.txt (or
 * /dev_hdd0/tmp/netlog.txt); does nothing when the file is missing.
 * ps3_log_net_up is set when this module called netInitialize(), so the
 * engine's PS3_Net_Init()/PS3_Net_Shutdown() must not do it again. */
extern int ps3_log_net_up;
void PS3_LogNetInit(void);

#ifdef __cplusplus
}
#endif
#endif
