/* ps3_rsxtest.c -- RSX bring-up test, runs instead of the engine.
 *
 * Runs when USRDIR/rsxtest.txt (or /dev_hdd0/tmp/rsxtest.txt) exists, or always
 * when built with -DOPENMOHAA_PS3_RSX_TEST=ON. main() checks right after
 * Sys_PlatformInit(), before Com_Init(), so the C++ static constructors and
 * the logger have already run but no engine subsystem has. Delete the file to
 * boot the engine normally again; one installed PKG covers both.
 *
 * The first character of rsxtest.txt picks the stage (default 0):
 *   0  self-contained GCM: rsxInit, 3 display buffers, solid colour clears,
 *      flips. Does not touch ps3_glimp.c or ps3gl at all.
 *   1  the engine's own PS3_RSX_Init() (includes ps3gl_init), then the same
 *      colour clears through PS3_RSX_BeginFrame()/PS3_RSX_EndFrame().
 *   2  stage 1 plus ps3gl immediate-mode drawing: ps3gl_Clear, an orthographic
 *      projection and a moving vertex-coloured triangle. Exercises the vertex
 *      ring, the shaders and the matrix path the renderer depends on.
 *
 * Expected result: ~10 s of a screen cycling red, green, blue, white (stages
 * 0 and 1) or a dark-blue screen with a red/green/blue triangle sweeping left
 * and right (stage 2), then the process exits to the XMB. Every step is logged
 * with PS3_BootTracef (synced, and sent over UDP when netlog.txt exists), so
 * the last line written is where a freeze happened.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

#include "ps3_log.h"
#include "ps3_glimp.h"
#include "../gl/ps3gl.h"

#define T(...) PS3_BootTracef("RSXTEST: " __VA_ARGS__)

#define TEST_FB_COUNT      3
#define TEST_FRAMES        600
#define TEST_FRAMES_PER_COLOR 60
#define TEST_CB_SIZE       (1 * 1024 * 1024)
#define TEST_HOST_SIZE     (8 * 1024 * 1024)

static const u32 test_colors[4] = {
    0xffff0000u,   /* red   (XRGB) */
    0xff00ff00u,   /* green */
    0xff0000ffu,   /* blue  */
    0xffffffffu    /* white */
};

static int test_stage(void)
{
    char c = '0';
    FILE *f = fopen(PS3_USRDIR "/rsxtest.txt", "r");

    if (!f)
        f = fopen("/dev_hdd0/tmp/rsxtest.txt", "r");
    if (f) {
        int ch = fgetc(f);
        if (ch >= '0' && ch <= '2')
            c = (char)ch;
        fclose(f);
    }
    return c - '0';
}

int PS3_RSXTest_Wanted(void)
{
#ifdef PS3_RSX_TEST
    return 1;
#else
    FILE *f = fopen(PS3_USRDIR "/rsxtest.txt", "r");

    if (!f)
        f = fopen("/dev_hdd0/tmp/rsxtest.txt", "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
#endif
}

static void test_clear(gcmContextData *ctx, u32 w, u32 h, u32 color)
{
    float scale[4], offset[4];

    scale[0]  = w * 0.5f;  scale[1]  = h * -0.5f;  scale[2]  = 0.5f;  scale[3]  = 0.0f;
    offset[0] = w * 0.5f;  offset[1] = h * 0.5f;   offset[2] = 0.5f;  offset[3] = 0.0f;
    rsxSetViewport(ctx, 0, 0, w, h, 0.0f, 1.0f, scale, offset);
    rsxSetScissor(ctx, 0, 0, w, h);
    rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetClearColor(ctx, color);
    rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

/* ---- stage 0: plain GCM ------------------------------------------------------ */

static void stage0(void)
{
    gcmContextData *ctx = NULL;
    void *host;
    s32 ret;
    videoState state;
    videoResolution res;
    videoConfiguration vc;
    u32 w, h, pitch, depth_off = 0;
    u32 color_off[TEST_FB_COUNT];
    u32 *color[TEST_FB_COUNT];
    u32 *depth;
    int i, idx = 0, frame;

    T("stage 0: memalign IO buffer (%u bytes)", (unsigned)TEST_HOST_SIZE);
    host = memalign(1024 * 1024, TEST_HOST_SIZE);
    if (!host) { T("FAIL memalign"); return; }

    ret = rsxInit(&ctx, TEST_CB_SIZE, TEST_HOST_SIZE, host);
    T("rsxInit ret=%d ctx=%p", (int)ret, (void *)ctx);
    if (ret != 0 || !ctx) { T("FAIL rsxInit"); return; }

    memset(&state, 0, sizeof(state));
    ret = videoGetState(0, 0, &state);
    T("videoGetState ret=%d state=%d resolution=%d aspect=%d", (int)ret,
      (int)state.state, (int)state.displayMode.resolution, (int)state.displayMode.aspect);
    if (ret != 0) { T("FAIL videoGetState"); return; }

    ret = videoGetResolution(state.displayMode.resolution, &res);
    if (ret != 0) { T("FAIL videoGetResolution ret=%d", (int)ret); return; }
    w = res.width;
    h = res.height;
    pitch = w * 4;
    T("display %ux%u", (unsigned)w, (unsigned)h);

    memset(&vc, 0, sizeof(vc));
    vc.resolution = state.displayMode.resolution;
    vc.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch      = pitch;
    vc.aspect     = state.displayMode.aspect;
    ret = videoConfigure(0, &vc, NULL, 0);
    T("videoConfigure ret=%d", (int)ret);
    if (ret != 0) { T("FAIL videoConfigure"); return; }

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (i = 0; i < TEST_FB_COUNT; i++) {
        color[i] = (u32 *)rsxMemalign(64, pitch * h);
        if (!color[i] || rsxAddressToOffset(color[i], &color_off[i]) != 0) {
            T("FAIL color buffer %d (ptr=%p)", i, (void *)color[i]);
            return;
        }
        gcmSetDisplayBuffer(i, color_off[i], pitch, w, h);
    }
    depth = (u32 *)rsxMemalign(64, w * h * 4);
    if (!depth || rsxAddressToOffset(depth, &depth_off) != 0) {
        T("FAIL depth buffer (ptr=%p)", (void *)depth);
        return;
    }
    T("buffers ok: color offsets 0x%x 0x%x 0x%x depth 0x%x",
      (unsigned)color_off[0], (unsigned)color_off[1], (unsigned)color_off[2], (unsigned)depth_off);

    gcmResetFlipStatus();

    for (frame = 0; frame < TEST_FRAMES; frame++) {
        gcmSurface sf;
        u32 color_value = test_colors[(frame / TEST_FRAMES_PER_COLOR) & 3];

        PS3_PlatformPump();
        if (!PS3_PlatformRunning()) { T("XMB exit requested at frame %d", frame); break; }

        if (frame > 0) {
            while (gcmGetFlipStatus() != 0)
                usleep(200);
            gcmResetFlipStatus();
        }

        memset(&sf, 0, sizeof(sf));
        sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
        sf.colorTarget      = GCM_SURFACE_TARGET_0;
        sf.colorLocation[0] = GCM_LOCATION_RSX;
        sf.colorOffset[0]   = color_off[idx];
        sf.colorPitch[0]    = pitch;
        for (i = 1; i < 4; i++) {
            sf.colorLocation[i] = GCM_LOCATION_RSX;
            sf.colorOffset[i]   = color_off[idx];
            sf.colorPitch[i]    = 64;
        }
        sf.depthFormat   = GCM_SURFACE_ZETA_Z24S8;
        sf.depthLocation = GCM_LOCATION_RSX;
        sf.depthOffset   = depth_off;
        sf.depthPitch    = w * 4;
        sf.type          = GCM_SURFACE_TYPE_LINEAR;
        sf.antiAlias     = GCM_SURFACE_CENTER_1;
        sf.width         = w;
        sf.height        = h;
        sf.x = 0;
        sf.y = 0;
        rsxSetSurface(ctx, &sf);

        test_clear(ctx, w, h, color_value);

        gcmSetFlip(ctx, idx);
        rsxFlushBuffer(ctx);
        gcmSetWaitFlip(ctx);
        idx = (idx + 1) % TEST_FB_COUNT;

        if (frame < 3 || frame % 60 == 0)
            T("frame %d flipped (colour 0x%08x)", frame, (unsigned)color_value);
    }
    T("stage 0 finished");
}

/* ---- stage 1: the engine's RSX layer -------------------------------------------- */

static void stage1(void)
{
    gcmContextData *ctx;
    int frame;

    T("stage 1: calling PS3_RSX_Init() (rsxInit + framebuffers + ps3gl_init)");
    PS3_RSX_Init();
    ctx = ps3gl_get_ctx();
    T("PS3_RSX_Init returned, ctx=%p ps3gl_ptr=%p size %dx%d", (void *)ctx,
      (void *)ps3gl_ptr, PS3_RSX_Width(), PS3_RSX_Height());
    if (!ctx || (uintptr_t)ps3gl_ptr <= 0x1000) { T("FAIL RSX/ps3gl not up"); return; }

    for (frame = 0; frame < TEST_FRAMES; frame++) {
        u32 color_value = test_colors[(frame / TEST_FRAMES_PER_COLOR) & 3];

        PS3_PlatformPump();
        if (!PS3_PlatformRunning()) { T("XMB exit requested at frame %d", frame); break; }

        PS3_RSX_BeginFrame();
        ctx = ps3gl_get_ctx();
        if (!ctx) { T("FAIL lost ctx at frame %d", frame); return; }
        ps3gl_clear_full_surface();          /* opens viewport/scissor/mask */
        rsxSetClearColor(ctx, color_value);
        rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        PS3_RSX_EndFrame();

        if (frame < 3 || frame % 60 == 0)
            T("frame %d done (colour 0x%08x)", frame, (unsigned)color_value);
    }
    T("stage 1 loop finished, PS3_RSX_Shutdown()");
    PS3_RSX_Shutdown();
    T("stage 1 finished");
}

/* ---- stage 2: ps3gl immediate mode ------------------------------------------------ */

static void stage2(void)
{
    gcmContextData *ctx;
    int frame, w, h;

    T("stage 2: calling PS3_RSX_Init(), then drawing through ps3gl");
    PS3_RSX_Init();
    ctx = ps3gl_get_ctx();
    w = PS3_RSX_Width();
    h = PS3_RSX_Height();
    T("PS3_RSX_Init returned, ctx=%p ps3gl_ptr=%p size %dx%d", (void *)ctx,
      (void *)ps3gl_ptr, w, h);
    if (!ctx || (uintptr_t)ps3gl_ptr <= 0x1000) { T("FAIL RSX/ps3gl not up"); return; }

    for (frame = 0; frame < TEST_FRAMES; frame++) {
        float cx = (float)w * 0.5f + (float)(((frame % 240) - 120) * 3);
        float tw = (float)h * 0.3f;

        PS3_PlatformPump();
        if (!PS3_PlatformRunning()) { T("XMB exit requested at frame %d", frame); break; }

        PS3_RSX_BeginFrame();
        if (!ps3gl_get_ctx()) { T("FAIL lost ctx at frame %d", frame); return; }

        ps3gl_ClearColor(0.05f, 0.05f, 0.25f, 1.0f);
        ps3gl_Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ps3gl_Viewport(0, 0, w, h);
        ps3gl_Disable(GL_DEPTH_TEST);
        ps3gl_Disable(GL_CULL_FACE);
        ps3gl_Disable(GL_BLEND);
        ps3gl_Disable(GL_TEXTURE_2D);
        ps3gl_MatrixMode(GL_PROJECTION);
        ps3gl_LoadIdentity();
        ps3gl_Ortho(0.0, (double)w, (double)h, 0.0, -1.0, 1.0);
        ps3gl_MatrixMode(GL_MODELVIEW);
        ps3gl_LoadIdentity();

        ps3gl_Begin(GL_TRIANGLES);
        ps3gl_Color4ub(255, 0, 0, 255);   ps3gl_Vertex2f(cx,       (float)h * 0.25f);
        ps3gl_Color4ub(0, 255, 0, 255);   ps3gl_Vertex2f(cx - tw,  (float)h * 0.75f);
        ps3gl_Color4ub(0, 0, 255, 255);   ps3gl_Vertex2f(cx + tw,  (float)h * 0.75f);
        ps3gl_End();

        PS3_RSX_EndFrame();

        if (frame < 3 || frame % 60 == 0)
            T("frame %d drawn, glGetError=0x%x", frame, (unsigned)ps3gl_GetError());
    }
    T("stage 2 loop finished, PS3_RSX_Shutdown()");
    PS3_RSX_Shutdown();
    T("stage 2 finished");
}

void PS3_RSXTest_Run(void)
{
    int stage = test_stage();

    T("=== RSX bring-up test, stage %d ===", stage);
    if (stage == 2)
        stage2();
    else if (stage == 1)
        stage1();
    else
        stage0();
    T("=== test over, exiting ===");
    exit(0);
}
