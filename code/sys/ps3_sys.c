/* OpenMoHAA PS3/PSL1GHT platform layer. */
#include <sys/dbg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/mutex.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "sys/sys_local.h"
#include "ps3_platform.h"
#include "ps3_log.h"

#ifndef MAX_FOUND_FILES
#define MAX_FOUND_FILES 0x1000
#endif

/* Process parameters.
 *
 * Without this the ELF carries no process parameter block (the empty LOOS+1
 * program header in readelf), and the main thread gets the loader's default
 * stack, which I believe is 64 KB. The engine runs everything on the main
 * thread and R_CreateBuiltinImages alone reserves 70,848 bytes of locals
 * (a 64 KB lightmap buffer), so the stack pointer used to land outside the
 * stack and the console locked up on entry. 1 MB is the largest the loader
 * accepts for the primary thread. */
#ifdef SYS_PROCESS_PARAM
/* The second argument is the primary thread's stack size IN BYTES. (The
 * SYS_PROCESS_SPAWN_STACK_SIZE_* enum, 0x70 for 1M, belongs to
 * sysProcessExitSpawn and is not valid here: stored as 112 bytes the loader
 * ignores it and keeps the default.) */
SYS_PROCESS_PARAM(1001, 0x100000)
#endif

char *Sys_DefaultHomePath(void) { return PS3_USRDIR; }
char *Sys_DefaultHomeConfigPath(void) { return PS3_USRDIR; }
char *Sys_DefaultHomeDataPath(void) { return PS3_USRDIR; }
char *Sys_DefaultHomeStatePath(void) { return PS3_USRDIR; }
char *Sys_SteamPath(void) { return ""; }
char *Sys_GogPath(void) { return ""; }

char *Sys_BinaryPathRelative(const char *relative)
{
    static char out[MAX_OSPATH];
    if (!relative || !*relative)
        return Sys_BinaryPath();
    Com_sprintf(out, sizeof(out), "%s/%s", Sys_BinaryPath(), relative);
    return out;
}

char *Sys_Cwd(void) { return PS3_USRDIR; }

const char *Sys_Basename(char *path)
{
    char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

const char *Sys_Dirname(char *path)
{
    static char out[MAX_OSPATH];
    char *p;
    Q_strncpyz(out, path, sizeof(out));
    p = strrchr(out, '/');
    if (p) {
        if (p == out)
            p[1] = '\0';
        else
            *p = '\0';
    } else {
        Q_strncpyz(out, ".", sizeof(out));
    }
    return out;
}


qboolean Sys_RandomBytes(byte *string, int len)
{
    struct timeval tv;
    unsigned int seed;
    int i;
    gettimeofday(&tv, NULL);
    seed = (unsigned int)(tv.tv_sec ^ tv.tv_usec ^ (uintptr_t)string);
    for (i = 0; i < len; ++i) {
        seed = seed * 1103515245u + 12345u;
        string[i] = (byte)(seed >> 16);
    }
    return qtrue;
}

char *Sys_GetCurrentUser(void)
{
    static char username[SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE];
    static int resolved;
    if (!resolved) {
        resolved = 1;
        username[0] = '\0';
        if (sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME,
                username, SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE) != 0)
            username[0] = '\0';
        username[SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE - 1] = '\0';
        if (!username[0]) {
            if (sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_NICKNAME,
                    username, SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE) != 0)
                username[0] = '\0';
            username[SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE - 1] = '\0';
        }
    }
    return username[0] ? username : "player";
}

qboolean Sys_LowPhysicalMemory(void) { return qfalse; }
qboolean Sys_SetMaxFileLimit(void) { return qfalse; }

FILE *Sys_FOpen(const char *ospath, const char *mode) { return fopen(ospath, mode); }
FILE *Sys_Mkfifo(const char *ospath) { (void)ospath; return NULL; }

qboolean Sys_Mkdir(const char *path)
{
    struct stat st;
    if (!mkdir(path, 0777) || errno == EEXIST)
        return qtrue;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void Sys_SetEnv(const char *name, const char *value)
{
    if (value && *value) setenv(name, value, 1);
    else unsetenv(name);
}

int Sys_PIDIsRunning(int pid) { (void)pid; return qtrue; }
int Sys_PID(void) { return 1; }

static u64 ps3_time_base;
int Sys_Milliseconds(void)
{
    struct timeval tv;
    u64 now;
    gettimeofday(&tv, NULL);
    now = (u64)tv.tv_sec * 1000000ULL + (u64)tv.tv_usec;
    if (!ps3_time_base) ps3_time_base = now;
    return (int)((now - ps3_time_base) / 1000ULL);
}

void Sys_Sleep(int msec)
{
    if (msec > 0) usleep((unsigned)msec * 1000U);
}

void Sys_ListFilteredFiles(const char *basedir, char *subdirs, char *filter,
                           qboolean wantsubs, char **list, int *numfiles)
{
    char search[MAX_OSPATH], filename[MAX_OSPATH], child[MAX_OSPATH];
    DIR *dir;
    struct dirent *d;
    struct stat st;

    if (*numfiles >= MAX_FOUND_FILES - 1) return;
    Com_sprintf(search, sizeof(search), "%s%s%s", basedir,
                *subdirs ? "/" : "", *subdirs ? subdirs : "");
    dir = opendir(search);
    if (!dir) return;

    while ((d = readdir(dir)) != NULL) {
        if (!Q_stricmp(d->d_name, ".") || !Q_stricmp(d->d_name, "..")) continue;
        Com_sprintf(filename, sizeof(filename), "%s/%s", search, d->d_name);
        if (stat(filename, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!wantsubs) {
                if (Com_FilterPath(filter, d->d_name, qfalse)) {
                    list[(*numfiles)++] = CopyString(
                        *subdirs ? va("%s/%s", subdirs, d->d_name) : d->d_name);
                }
            }
            if (*numfiles < MAX_FOUND_FILES - 1) {
                Q_strncpyz(child, *subdirs ? va("%s/%s", subdirs, d->d_name) : d->d_name, sizeof(child));
                Sys_ListFilteredFiles(basedir, child, filter, wantsubs, list, numfiles);
            }
            continue;
        }
        if (wantsubs) continue;
        Com_sprintf(child, sizeof(child), "%s%s%s", *subdirs ? subdirs : "",
                    *subdirs ? "/" : "", d->d_name);
        if (!filter || Com_FilterPath(filter, child, qfalse))
            list[(*numfiles)++] = CopyString(child);
        if (*numfiles >= MAX_FOUND_FILES - 1) break;
    }
    closedir(dir);
}

char **Sys_ListFiles(const char *directory, const char *extension,
                     const char *filter, int *numfiles, qboolean wantsubs)
{
    char *tmp[MAX_FOUND_FILES];
    char **result;
    DIR *dir;
    struct dirent *d;
    struct stat st;
    int n = 0, i;
    size_t extlen = extension ? strlen(extension) : 0;

    *numfiles = 0;
    if (!directory || !*directory) return NULL;

    if (filter) {
        Sys_ListFilteredFiles(directory, "", (char *)filter, wantsubs, tmp, &n);
    } else {
        dir = opendir(directory);
        if (!dir) return NULL;
        while ((d = readdir(dir)) != NULL && n < MAX_FOUND_FILES - 1) {
            char path[MAX_OSPATH];
            if (!Q_stricmp(d->d_name, ".") || !Q_stricmp(d->d_name, "..")) continue;
            Com_sprintf(path, sizeof(path), "%s/%s", directory, d->d_name);
            if (stat(path, &st) != 0) continue;
            if (wantsubs && !S_ISDIR(st.st_mode)) continue;
            if (!wantsubs && S_ISDIR(st.st_mode)) continue;
            if (extension && extlen && Q_stricmp(extension, "/")) {
                size_t len = strlen(d->d_name);
                if (len < extlen || Q_stricmp(d->d_name + len - extlen, extension)) continue;
            }
            tmp[n++] = CopyString(d->d_name);
        }
        closedir(dir);
    }
    if (!n) return NULL;
    result = (char **)Z_Malloc((size_t)(n + 1) * sizeof(*result));
    for (i = 0; i < n; ++i) result[i] = tmp[i];
    result[n] = NULL;
    *numfiles = n;
    return result;
}

void Sys_FreeFileList(char **list)
{
    int i;
    if (!list) return;
    for (i = 0; list[i]; ++i) Z_Free(list[i]);
    Z_Free(list);
}

void *Sys_InitializeCriticalSection(void)
{
    sys_mutex_attr_t attr;
    sys_mutex_t *mutex = (sys_mutex_t *)malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    sysMutexAttrInitialize(attr);
    attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
    if (sysMutexCreate(mutex, &attr) != 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

void Sys_EnterCriticalSection(void *ptr)
{
    if (ptr) sysMutexLock(*(sys_mutex_t *)ptr, 0);
}

void Sys_LeaveCriticalSection(void *ptr)
{
    if (ptr) sysMutexUnlock(*(sys_mutex_t *)ptr);
}

qboolean Sys_DllExtension(const char *name)
{
    (void)name;
    return qfalse;
}

char *Sys_MicrosoftStorePath(void)
{
    return "";
}

void Sys_DebugPrint(const char *message)
{
    if (message)
        PS3_LogRaw(message);
}

void Sys_PumpMessageLoop(void)
{
    PS3_PlatformPump();
}

void SetNormalThreadPriority(void) {}
void SetBelowNormalThreadPriority(void) {}

void Sys_ErrorDialog(const char *error)
{
    PS3_Logf("[ERROR] %s", error ? error : "unknown error");
}

dialogResult_t Sys_Dialog(dialogType_t type, const char *message, const char *title)
{
    (void)type;
    PS3_Logf("[dialog] %s: %s", title ? title : "", message ? message : "");
    return DR_OK;
}

static volatile qboolean ps3_platform_running = qtrue;

#if !defined(DEDICATED)
static void PS3_SysutilCallback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;
    if (status == SYSUTIL_EXIT_GAME)
        ps3_platform_running = qfalse;
}
#endif

void PS3_PlatformPump(void)
{
#if !defined(DEDICATED)
    /* PSL1GHT delivers XMB/OSK notifications through this pump. */
    sysUtilCheckCallback();
#endif
}

int PS3_PlatformRunning(void)
{
    return ps3_platform_running != qfalse;
}

void Sys_GLimpSafeInit(void) {}
void Sys_GLimpInit(void) {}
/* PPU exception handler: runs on its own thread when any PPU thread faults
 * (bad pointer, illegal instruction, alignment...). Log the faulting PC, LR, CTR,
 * the data address and the registers, walk the stack, and dump the trace ring, so
 * the crash site can be resolved against the build's ELF with addr2line. */
static volatile int ps3_exc_active = 0;

static int ps3_exc_addr_ok(u64 a)
{
    return a >= 0x10000ULL && a < 0xD0000000ULL;
}

static void PS3_ExceptionHandler(u64 cause, sys_ppu_thread_t tid, u64 dar)
{
    sys_dbg_ppu_thread_context_t ctx;
    s32 rc;
    u64 sp;
    int i;

    if (ps3_exc_active++) {
        return;
    }
    PS3_BootTracef("EXCEPTION cause=0x%llx tid=0x%llx dar=0x%llx", (unsigned long long)cause,
        (unsigned long long)tid, (unsigned long long)dar);
    memset(&ctx, 0, sizeof(ctx));
    rc = sysDbgReadPPUThreadContext(tid, &ctx);
    PS3_BootTracef("EXCEPTION context rc=0x%x pc=0x%08x lr=0x%08x ctr=0x%08x cr=0x%08x xer=0x%08x",
        (unsigned)rc, (unsigned)ctx.pc, (unsigned)ctx.lr, (unsigned)ctx.ctr, (unsigned)ctx.cr, (unsigned)ctx.xer);
    if (rc == 0) {
        PS3_BootTracef("EXCEPTION r0=%llx r1=%llx r2=%llx r3=%llx r4=%llx r5=%llx r6=%llx r7=%llx",
            (unsigned long long)ctx.gpr[0], (unsigned long long)ctx.gpr[1], (unsigned long long)ctx.gpr[2],
            (unsigned long long)ctx.gpr[3], (unsigned long long)ctx.gpr[4], (unsigned long long)ctx.gpr[5],
            (unsigned long long)ctx.gpr[6], (unsigned long long)ctx.gpr[7]);
        PS3_BootTracef("EXCEPTION r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r28=%llx r29=%llx r30=%llx r31=%llx",
            (unsigned long long)ctx.gpr[8], (unsigned long long)ctx.gpr[9], (unsigned long long)ctx.gpr[10],
            (unsigned long long)ctx.gpr[11], (unsigned long long)ctx.gpr[12], (unsigned long long)ctx.gpr[28],
            (unsigned long long)ctx.gpr[29], (unsigned long long)ctx.gpr[30], (unsigned long long)ctx.gpr[31]);
        /* PPC64 ELF: 0(sp) = back chain; a function's return address is saved at 16(back chain). */
        sp = ctx.gpr[1];
        for (i = 0; i < 24; ++i) {
            u64 prev;
            u64 ret;

            if (!ps3_exc_addr_ok(sp) || (sp & 7)) {
                break;
            }
            prev = *(const u64 *)(uintptr_t)sp;
            if (!ps3_exc_addr_ok(prev) || (prev & 7) || prev <= sp || !ps3_exc_addr_ok(prev + 16)) {
                break;
            }
            ret = *(const u64 *)(uintptr_t)(prev + 16);
            PS3_BootTracef("EXCEPTION bt%d sp=0x%llx ret=0x%llx", i, (unsigned long long)sp, (unsigned long long)ret);
            sp = prev;
        }
    }
    PS3_DumpTraceRing("exception");
    PS3_BootTracef("EXCEPTION report complete");
}

/* Opt-in: on the first build that registered it unconditionally, the game went
 * straight back to the XMB right after the "trace level" line, i.e. the firmware's
 * sys_lv2dbg calls kill the process on that console (probably a retail kernel).
 * Create USRDIR/exception_handler.txt containing 1, 2 or 3 to try:
 *   1 = register with flags=1 (stop the faulting threads)
 *   2 = register with flags=0
 *   3 = set the handler stack size first, then register with flags=1
 * Each step is logged *before* the call, so the last line shows which call killed
 * the process if it does. */
static void ps3_register_exception_handler(void)
{
    int mode = 0;
    s32 rc;
    FILE *f = fopen(PS3_USRDIR "/exception_handler.txt", "rb");

    if (f) {
        int c = fgetc(f);
        if (c >= '0' && c <= '9') {
            mode = c - '0';
        }
        fclose(f);
    }
    if (mode < 1 || mode > 3) {
        PS3_BootTracef("Sys_PlatformInit: PPU exception handler disabled (put 1, 2 or 3 in USRDIR/exception_handler.txt to try it)");
        return;
    }
    if (mode == 3) {
        PS3_BootTracef("Sys_PlatformInit: about to call sysDbgSetStacksizePPUExceptionHandler(0x10000)");
        rc = sysDbgSetStacksizePPUExceptionHandler(0x10000);
        PS3_BootTracef("Sys_PlatformInit: sysDbgSetStacksizePPUExceptionHandler returned rc=0x%x", (unsigned)rc);
    }
    PS3_BootTracef("Sys_PlatformInit: about to call sysDbgRegisterPPUExceptionHandler(flags=%d)", mode == 2 ? 0 : 1);
    rc = sysDbgRegisterPPUExceptionHandler(PS3_ExceptionHandler, mode == 2 ? 0 : 1);
    PS3_BootTracef("Sys_PlatformInit: sysDbgRegisterPPUExceptionHandler returned rc=0x%x", (unsigned)rc);
}

void Sys_PlatformInit(void)
{
    PS3_BootTracef("Sys_PlatformInit: entry USRDIR=%s", PS3_USRDIR);
    ps3_platform_running = qtrue;
#if !defined(DEDICATED)
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, PS3_SysutilCallback, NULL);
#endif
    Sys_SetBinaryPath(PS3_USRDIR);
    Sys_SetDefaultInstallPath(PS3_USRDIR);
    Sys_Milliseconds();
    PS3_LogInit();
    PS3_BootTracef("Sys_PlatformInit: PS3_LogInit returned");
    PS3_Logf("OpenMoHAA PS3 platform init (%s)", PS3_USRDIR);
    PS3_Logf("PS3 build stamp: %s %s (trace19: native audio driver, 720p default, cgame crumbs)", __DATE__, __TIME__);
    PS3_BootTracef("Sys_PlatformInit: trace level %d (create USRDIR/trace_level.txt containing 2 for full tracing)", PS3_TraceLevel());
    ps3_register_exception_handler();
}
void Sys_PlatformExit(void)
{
#if !defined(DEDICATED)
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    /* Slot 1 is owned by the PS3 OSK; it may still be registered if the XMB
     * asks the process to exit while an OSK transition is in flight. */
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1);
#endif
    PS3_LogShutdown();
}
void Sys_StartProcess(char *cmdline, qboolean doexit) { (void)cmdline; (void)doexit; }
void Sys_OpenURL(char *url, qboolean doexit) { (void)url; (void)doexit; }
qboolean Sys_OpenFolderInPlatformFileManager(const char *path) { (void)path; return qfalse; }
