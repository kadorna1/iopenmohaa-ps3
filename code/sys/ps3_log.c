/* ps3_log.c -- log file in USRDIR, thread-safe, rotated at boot, size-capped.
 *
 * The previous run's log is kept as openmohaa-ps3.old.log. Every line is
 * flushed so a hang or a crash still leaves the last line on disk.
 *
 * v40: the early trace (PS3_BootTracef) no longer uses stdio. It writes with
 * the lv2 file API, fsyncs every line (a console freeze loses anything still
 * in the filesystem cache), and optionally mirrors every line over UDP. The
 * first PS3_LOG_MIRROR_LINES lines of the normal log are mirrored the same way
 * so RSX/renderer init messages survive a hard freeze too.
 *
 * Build knobs (-D...):
 *   PS3_LOG_HAVE_FSYNC=0     the SDK has no sysLv2FsFsync: skip the fsync
 *   PS3_LOG_MIRROR_LINES=n   how many normal-log lines are also synced (300)
 *   PS3_NETLOG_PORT=n        default UDP port (9999)
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/file.h>
#include <sys/mutex.h>
#include <net/net.h>
#include <net/socket.h>
#include <netinet/in.h>

#include "ps3_log.h"
#include "ps3_screenlog.h"

#ifndef PS3_LOG_HAVE_FSYNC
#define PS3_LOG_HAVE_FSYNC 1
#endif
#ifndef PS3_LOG_MIRROR_LINES
#define PS3_LOG_MIRROR_LINES 300
#endif
#ifndef PS3_NETLOG_PORT
#define PS3_NETLOG_PORT 9999
#endif

#define PS3_LOG_OLD_PATH PS3_USRDIR "/openmohaa-ps3.old.log"
#define PS3_LOG_MAX_BYTES (4 * 1024 * 1024)
#define PS3_LOG_LINE_MAX 1400   /* keeps one UDP datagram under the MTU */

/* ---- raw synced file write ------------------------------------------------ */

/* trunc=1 starts the file afresh. The first emit of every run does that, so
 * openmohaa-earlytrace.log only ever holds the latest run. */
static int boot_append(const char *path, const char *buf, size_t len, int sync, int trunc)
{
    s32 fd = -1;
    u64 written = 0;

    if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | (trunc ? SYS_O_TRUNC : SYS_O_APPEND),
                     &fd, 0666, NULL, 0) != 0)
        return 0;
    sysLv2FsWrite(fd, buf, len, &written);
#if PS3_LOG_HAVE_FSYNC
    if (sync)
        sysLv2FsFsync(fd);
#else
    (void)sync;
#endif
    sysLv2FsClose(fd);
    return written == len;
}

/* Two sinks: the game's USRDIR and /dev_hdd0/tmp. If the USRDIR one fails
 * (wrong TITLE_ID, no permission) the other candidates are tried so some file
 * still appears. fast=1 skips the fsync and the second sink: about a
 * millisecond per line instead of tens, for tight init loops. A process crash
 * still leaves those lines on disk; only a full console lock-up can lose them. */
static int boot_first_emit = 1;

static void boot_emit(const char *buf, size_t len, int fast)
{
    static const char *const fallbacks[] = {
        "/dev_hdd0/game/OMOHAA020/USRDIR/openmohaa-earlytrace.log",
        "/dev_hdd0/game/OMOHAA001/USRDIR/openmohaa-earlytrace.log",
        NULL
    };
    unsigned i;
    int primary_ok;
    int trunc = boot_first_emit;

    boot_first_emit = 0;
    primary_ok = boot_append(PS3_USRDIR "/openmohaa-earlytrace.log", buf, len, !fast, trunc);
    if (fast) {
        if (!primary_ok)
            boot_append("/dev_hdd0/tmp/openmohaa-earlytrace.log", buf, len, 0, trunc);
        return;
    }
    boot_append("/dev_hdd0/tmp/openmohaa-earlytrace.log", buf, len, 1, trunc);
    if (!primary_ok) {
        for (i = 0; fallbacks[i]; ++i)
            boot_append(fallbacks[i], buf, len, 1, trunc);
    }
}

/* ---- UDP sink -------------------------------------------------------------- */

int ps3_log_net_up;
static int net_sock = -1;
static struct sockaddr_in net_dst;

static void net_send(const char *buf, size_t len)
{
    if (net_sock >= 0)
        sendto(net_sock, buf, len, 0, (struct sockaddr *)&net_dst, sizeof(net_dst));
}

static int net_read_config(const char *path, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    out[0] = '\0';
    if (!fgets(out, (int)outsz, f))
        out[0] = '\0';
    fclose(f);
    return out[0] != '\0';
}

void PS3_LogNetInit(void)
{
    char cfg[64];
    unsigned a, b, c, d, port = PS3_NETLOG_PORT;
    int n;

    if (net_sock >= 0)
        return;

    if (!net_read_config(PS3_USRDIR "/netlog.txt", cfg, sizeof(cfg)) &&
        !net_read_config("/dev_hdd0/tmp/netlog.txt", cfg, sizeof(cfg))) {
        PS3_BootTracef("NETLOG: no netlog.txt, UDP logging off");
        return;
    }
    n = sscanf(cfg, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port);
    if (n < 4 || a > 255 || b > 255 || c > 255 || d > 255 || port == 0 || port > 65535) {
        PS3_BootTracef("NETLOG: bad netlog.txt, expected a.b.c.d[:port]");
        return;
    }
    if (netInitialize() < 0) {
        PS3_BootTracef("NETLOG: netInitialize failed");
        return;
    }
    ps3_log_net_up = 1;
    net_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (net_sock < 0) {
        PS3_BootTracef("NETLOG: socket() failed");
        return;
    }
    memset(&net_dst, 0, sizeof(net_dst));
    net_dst.sin_len = sizeof(net_dst);
    net_dst.sin_family = AF_INET;
    /* PPU is big-endian: host order == network order. */
    net_dst.sin_port = (unsigned short)port;
    net_dst.sin_addr.s_addr = (a << 24) | (b << 16) | (c << 8) | d;
    PS3_BootTracef("NETLOG: sending to %u.%u.%u.%u:%u", a, b, c, d, port);
}

/* ---- early trace ------------------------------------------------------------ */

static unsigned boot_seq;

static void boot_trace_v(int fast, const char *fmt, va_list ap)
{
    char message[PS3_LOG_LINE_MAX + 24];
    int n;

    n = snprintf(message, sizeof(message), "[%u] ", ++boot_seq);
    if (n < 0)
        n = 0;
    vsnprintf(message + n, PS3_LOG_LINE_MAX, fmt, ap);
    n = (int)strlen(message);
    message[n++] = '\n';
    message[n] = '\0';

    boot_emit(message, (size_t)n, fast);
    net_send(message, (size_t)n);
    PS3_ScreenLog_Line(message);
}

void PS3_BootTracef(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    boot_trace_v(0, fmt, ap);
    va_end(ap);
}

void PS3_BootTraceFastf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    boot_trace_v(1, fmt, ap);
    va_end(ap);
}

/* ---- normal log -------------------------------------------------------------- */

static FILE *log_file;
static long log_bytes;
static int log_capped;
static sys_mutex_t log_mutex;
static int log_mutex_ok;
static unsigned mirror_lines;

static void log_lock(void)   { if (log_mutex_ok) sysMutexLock(log_mutex, 0); }
static void log_unlock(void) { if (log_mutex_ok) sysMutexUnlock(log_mutex); }

void PS3_LogInit(void)
{
    sys_mutex_attr_t attr;
    PS3_BootTracef("PS3_LogInit: entered, compiled USRDIR=%s", PS3_USRDIR);

    if (log_file)
        return;

    sysMutexAttrInitialize(attr);
    attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
    log_mutex_ok = (sysMutexCreate(&log_mutex, &attr) == 0);

    remove(PS3_LOG_OLD_PATH);
    rename(PS3_LOG_PATH, PS3_LOG_OLD_PATH);
    log_file = fopen(PS3_LOG_PATH, "w");
    PS3_BootTracef("PS3_LogInit: fopen %s %s", PS3_LOG_PATH, log_file ? "OK" : "FAILED");
    log_bytes = 0;
    log_capped = 0;

    PS3_LogNetInit();
}

void PS3_LogShutdown(void)
{
    log_lock();
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    log_unlock();
}

/* Copies msg (truncated) into buf with a trailing newline when asked for.
 * Returns the length. buf must hold PS3_LOG_LINE_MAX + 2 bytes. */
static size_t log_line(char *buf, const char *msg, int newline)
{
    size_t len = strlen(msg);

    if (len > PS3_LOG_LINE_MAX)
        len = PS3_LOG_LINE_MAX;
    memcpy(buf, msg, len);
    if (newline && (len == 0 || buf[len - 1] != '\n'))
        buf[len++] = '\n';
    buf[len] = '\0';
    return len;
}

/* UDP always; synced file copy only for the first PS3_LOG_MIRROR_LINES lines
 * (an fsync per line is slow, and the early lines are the ones a freeze eats). */
static void log_mirror(const char *msg, int newline)
{
    char buf[PS3_LOG_LINE_MAX + 2];
    size_t len;

    len = log_line(buf, msg, newline);
    PS3_ScreenLog_Line(buf);
    if (net_sock < 0 && mirror_lines >= PS3_LOG_MIRROR_LINES)
        return;
    net_send(buf, len);
    if (mirror_lines < PS3_LOG_MIRROR_LINES) {
        mirror_lines++;
        boot_emit(buf, len, 0);
    }
}

/* Writes msg as-is; adds a newline only when newline is set and msg lacks one. */
static void log_write(const char *msg, int newline)
{
    size_t len;

    log_mirror(msg, newline);

    if (!log_file || log_capped)
        return;

    len = strlen(msg);
    if (log_bytes + (long)len > PS3_LOG_MAX_BYTES) {
        fputs("\n[log] size cap reached, logging stopped\n", log_file);
        fflush(log_file);
        log_capped = 1;
        return;
    }
    fwrite(msg, 1, len, log_file);
    log_bytes += (long)len;
    if (newline && (len == 0 || msg[len - 1] != '\n')) {
        fputc('\n', log_file);
        log_bytes++;
    }
    fflush(log_file);
}

/* the map test (MAPWALK) reports these per map */
int ps3_log_warnings, ps3_log_cams;

void PS3_LogRaw(const char *msg)
{
    /* not the two shader-script warnings of the game data on every
     * renderer start (the PC version prints them too) */
    if ((strstr(msg, "WARNING") || strstr(msg, "Warning") ||
         strstr(msg, "ERROR") || strstr(msg, "Error")) && !strstr(msg, "In shader file"))
        ps3_log_warnings++;
    else if (!strncmp(msg, "[cam] start", 11))
        ps3_log_cams++;
    log_lock();
    log_write(msg, 0);
    log_unlock();
}

void ps3_log(const char *msg)
{
    log_lock();
    log_write(msg, 1);
    log_unlock();
}

void PS3_Logf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ps3_log(buf);
}
