/* probe_rsx.c -- "is my EBOOT running at all?" with a VISIBLE answer.
 *
 * Initialises RSX and video, then shows solid red / green / blue / white,
 * changing once a second for about 10 seconds, and logs every step to
 * probe-lv2.log (lv2 only: stdio is not safe from a constructor). If you see the colours,
 * your EBOOT runs and the problem is file access; if you bounce straight back
 * to the XMB, the EBOOT never starts.
 *
 * Build:
 *   export PSL1GHT="${PSL1GHT:-$PS3DEV}"
 *   $PS3DEV/ppu/bin/ppu-gcc -O2 -I$PSL1GHT/ppu/include -L$PSL1GHT/ppu/lib \
 *       probe_rsx.c -o probe_rsx.elf -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2
 */
#include <stdio.h>  /* snprintf only, in main() */
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/file.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

#define CB_SIZE   (1 * 1024 * 1024)
#define HOST_SIZE (8 * 1024 * 1024)   /* must be larger than CB_SIZE: the command buffer lives inside it */
#define FB_COUNT  2
#define FRAMES    600

static const char *const dirs[] = {
    "/dev_hdd0/tmp", "/dev_hdd0/game/OMOHPROBE/USRDIR", "/dev_usb000", "/dev_usb001", NULL
};

/* lv2 only: no stdio, safe from a constructor (see probe.c) */
static void beacon(const char *tag)
{
    char line[112], path[160];
    size_t n = strlen(tag);
    int i;

    if (n > sizeof(line) - 2)
        n = sizeof(line) - 2;
    memcpy(line, tag, n);
    line[n++] = '\n';

    for (i = 0; dirs[i]; i++) {
        s32 fd = -1;
        u64 written = 0;

        strcpy(path, dirs[i]);
        strcat(path, "/probe-lv2.log");
        if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0) == 0) {
            sysLv2FsWrite(fd, line, (u64)n, &written);
            sysLv2FsClose(fd);
        }
    }
}

__attribute__((constructor(101))) static void early(void) { beacon("rsx probe: ctor"); }

int main(void)
{
    static const u32 colors[4] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };
    gcmContextData *ctx = NULL;
    videoState state;
    videoResolution res;
    videoConfiguration vc;
    u32 w, h, pitch, off[FB_COUNT], *fb[FB_COUNT];
    void *host;
    int i, frame, idx = 0;
    char msg[96];

    beacon("rsx probe: main start");
    host = memalign(1024 * 1024, HOST_SIZE);
    if (!host) { beacon("rsx probe: memalign failed"); return 1; }
    i = rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);
    snprintf(msg, sizeof(msg), "rsx probe: rsxInit ret=0x%08x", (unsigned)i);
    beacon(msg);
    if (i != 0 || !ctx) return 1;

    memset(&state, 0, sizeof(state));
    videoGetState(0, 0, &state);
    videoGetResolution(state.displayMode.resolution, &res);
    w = res.width; h = res.height; pitch = w * 4;
    memset(&vc, 0, sizeof(vc));
    vc.resolution = state.displayMode.resolution;
    vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch = pitch;
    vc.aspect = state.displayMode.aspect;
    snprintf(msg, sizeof(msg), "rsx probe: videoConfigure ret=0x%08x (%ux%u)",
             (unsigned)videoConfigure(0, &vc, NULL, 0), (unsigned)w, (unsigned)h);
    beacon(msg);

    gcmSetFlipMode(GCM_FLIP_VSYNC);
    for (i = 0; i < FB_COUNT; i++) {
        fb[i] = (u32 *)rsxMemalign(64, pitch * h);
        if (!fb[i] || rsxAddressToOffset(fb[i], &off[i]) != 0) { beacon("rsx probe: fb alloc failed"); return 1; }
        gcmSetDisplayBuffer(i, off[i], pitch, w, h);
    }
    gcmResetFlipStatus();
    beacon("rsx probe: buffers ok, drawing");

    for (frame = 0; frame < FRAMES; frame++) {
        if (frame > 0) {
            while (gcmGetFlipStatus() != 0) usleep(200);
            gcmResetFlipStatus();
        }
        /* new colour once a second: fill both display buffers with the CPU
         * (no GPU state involved, so nothing to get wrong) */
        if (frame % 60 == 0) {
            u32 c = colors[(frame / 60) & 3], k;
            int b;
            for (b = 0; b < FB_COUNT; b++)
                for (k = 0; k < w * h; k++) fb[b][k] = c;
        }
        gcmSetFlip(ctx, idx);
        rsxFlushBuffer(ctx);
        gcmSetWaitFlip(ctx);
        idx = (idx + 1) % FB_COUNT;
    }
    beacon("rsx probe: finished");
    return 0;
}
