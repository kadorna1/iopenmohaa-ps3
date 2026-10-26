/* ps3_glimp.c -- RSX/GCM display init, frame management and GLimp_*.
 *
 * From IoQuake3-PS3 (Mayo1970), adapted to the iortcw SP renderer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include "ps3_screenlog.h"

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

#include "../renderergl1/tr_local.h"
#include "../sys/ps3_glimp.h"
#include "../gl/ps3gl.h"
#include "ps3_log.h"

extern void QGL_Init(void);

/* RSX state */
#define RSX_FB_COUNT 3

static gcmContextData *ps3_gcm_context = NULL;
static gcmContextData *ps3_gcm_context_backup = (gcmContextData *)0xDEAD;
static u32 ps3_display_width  = 1280;
static u32 ps3_display_height = 720;
static u32 ps3_color_pitch    = 0;
static u32 ps3_color_offset[RSX_FB_COUNT];
static u32 *ps3_color_buffer[RSX_FB_COUNT];
static u32 ps3_depth_offset;
static u32 *ps3_depth_buffer = NULL;
static int ps3_current_fb = 0;
static int ps3_current_rt = -1;

/* RSX IO window, published for ps3gl_draw.c: gcmAddressToOffset does not
 * validate an unmapped main-memory address, so callers must range-check. */
const void *ps3_rsx_io_base = NULL;
size_t      ps3_rsx_io_size = 0;
static void *ps3_rsx_io_alloc = NULL;

static int ps3_rsx_up = 0;
static int ps3_fit_border = 0;
static cvar_t *ps3_fit_cvar;
static int ps3_fit_pct = -1;

/* vid_ps3_screenfit (70..100 %) into ps3gl, when it changed. Called from
 * GLimp_Init and each frame begin (render thread, owner of ps3gl). */
static void PS3_ApplyScreenFit(void)
{
    int pct, w, h;

    if (!ps3_fit_cvar || ps3_fit_cvar->integer == ps3_fit_pct)
        return;
    pct = ps3_fit_cvar->integer;
    if (pct < 70) pct = 70;
    if (pct > 100) pct = 100;
    ps3_fit_pct = ps3_fit_cvar->integer;
    w = ((int)ps3_display_width * pct / 100) & ~1;
    h = ((int)ps3_display_height * pct / 100) & ~1;
    ps3gl_set_fit(((int)ps3_display_width - w) / 2, ((int)ps3_display_height - h) / 2, w, h);
    ps3_fit_border = (pct < 100);
    PS3_Logf("[video] output %ux%u, screen fit %d%% -> %dx%d",
             ps3_display_width, ps3_display_height, pct, w, h);
}

static volatile u32 ps3_flip_queued    = 0;
static volatile u32 ps3_flip_completed = 0;
static volatile u32 ps3_graphics_error_count = 0;
static volatile u32 ps3_graphics_error_value = 0;
static unsigned ps3_reported_graphics_errors;
static unsigned ps3_frame_begin_trace_count;
static unsigned ps3_frame_end_trace_count;
static unsigned ps3_frame_begin_seq;

/* The GCM context's begin/current/end pointers address the PPU-side staging
 * FIFO, not the RSX-visible command-buffer offset stored in ctrl->put. Keep a
 * bounded snapshot of the pending FIFO before rsxFlushBuffer resets current. */
#define PS3_RSX_DUMP_WORD_LIMIT 4096u
#define PS3_RSX_DUMP_TAIL_WORDS 64u
static u32 ps3_rsx_dump_snapshot[PS3_RSX_DUMP_WORD_LIMIT];
static u32 ps3_rsx_dump_tail_snapshot[PS3_RSX_DUMP_TAIL_WORDS];
static u32 ps3_rsx_dump_snapshot_seq;
static u32 ps3_rsx_dump_snapshot_words;
static u32 ps3_rsx_dump_snapshot_pre_bytes;
static u32 ps3_rsx_dump_snapshot_used_bytes;
static u32 ps3_rsx_dump_snapshot_capacity_bytes;
static u32 ps3_rsx_dump_tail_start_bytes;
static u32 ps3_rsx_dump_tail_words;
static int ps3_rsx_dump_snapshot_valid;

static void ps3_flip_handler(const u32 head)
{
    (void)head;
    ps3_flip_completed++;
}

/* Keep the graphics callback interrupt-safe: capture the exception and let
 * the normal engine thread format/write the diagnostic record later. */
static void ps3_graphics_handler(const u32 value)
{
    ps3_graphics_error_value = value;
    __sync_synchronize();
    ps3_graphics_error_count++;
}

static void PS3_RSX_TraceHardwareState(const char *phase, unsigned seq)
{
    gcmControlRegister *ctrl = gcmGetControlRegister();
    unsigned put = ctrl ? ctrl->put : 0xffffffffu;
    unsigned get = ctrl ? ctrl->get : 0xffffffffu;
    unsigned ref = ctrl ? ctrl->ref : 0xffffffffu;
    unsigned seg = 0xffffffffu, head = 0xffffffffu, cap = 0xffffffffu;
    unsigned label0 = 0xffffffffu, label1 = 0xffffffffu;
    unsigned expect0 = 0xffffffffu, expect1 = 0xffffffffu;

    if ((uintptr_t)ps3gl_ptr > 0x1000) {
        seg = (unsigned)ps3gl.vring.cur_seg;
        head = ps3gl.vring.head;
        cap = ps3gl.vring.seg_capacity;
        expect0 = ps3gl.vring.fence_val[0];
        expect1 = ps3gl.vring.fence_val[1];
        if (ps3gl.vring.fence_label[0])
            label0 = *ps3gl.vring.fence_label[0];
        if (ps3gl.vring.fence_label[1])
            label1 = *ps3gl.vring.fence_label[1];
    }

    PS3_BootTraceFastf(
        "RSX_HW %s seq=%u put=%08x get=%08x ref=%08x flipStatus=%u vblank=%llu lastFlip=%lld queued=%u completed=%u gfxErrors=%u gfxValue=%08x vringSeg=%u head=%u cap=%u labels=%u/%u,%u/%u",
        phase, seq, put, get, ref, (unsigned)gcmGetFlipStatus(),
        (unsigned long long)gcmGetVBlankCount(), (long long)gcmGetLastFlipTime(),
        (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed,
        (unsigned)ps3_graphics_error_count, (unsigned)ps3_graphics_error_value,
        seg, head, cap, label0, expect0, label1, expect1);
}

static void PS3_RSX_ReportGraphicsErrors(unsigned seq)
{
    unsigned count = (unsigned)ps3_graphics_error_count;
    if (count != ps3_reported_graphics_errors) {
        __sync_synchronize();
        PS3_BootTraceFastf("RSX_GRAPHICS_EXCEPTION seq=%u count=%u value=0x%08x",
            seq, count, (unsigned)ps3_graphics_error_value);
        ps3_reported_graphics_errors = count;
    }
}

/* Triple-buffer throttle adapted from ioQuake3-PS3 and ioRTCW-PS3.
 * With three display buffers, one flip may remain outstanding while the PPU
 * renders into the third buffer. Block only when two flips are outstanding.
 * Never manufacture callback completion on timeout: doing so could let the
 * renderer overwrite a buffer still owned by the RSX. */
static qboolean PS3_RSX_WaitFlips(unsigned seq)
{
    unsigned loops = 0;
    const unsigned max_loops = 20000; /* nominally 2 seconds at 100 us */

    while ((u32)(ps3_flip_queued - ps3_flip_completed) > (RSX_FB_COUNT - 2)) {
        if ((loops % 5000u) == 0) {
            PS3_BootTraceFastf(
                "RSX_FLIPWAIT pending seq=%u loops=%u queued=%u completed=%u vblank=%llu lastFlip=%lld",
                seq, loops,
                (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed,
                (unsigned long long)gcmGetVBlankCount(),
                (long long)gcmGetLastFlipTime());
            if (loops == 0 || (loops % 10000u) == 0)
                PS3_RSX_TraceHardwareState("flipwait", seq);
            PS3_RSX_ReportGraphicsErrors(seq);
        }

        if (loops++ >= max_loops) {
            PS3_BootTraceFastf(
                "RSX_FLIPWAIT TIMEOUT seq=%u queued=%u completed=%u outstanding=%u vblank=%llu lastFlip=%lld",
                seq,
                (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed,
                (unsigned)(u32)(ps3_flip_queued - ps3_flip_completed),
                (unsigned long long)gcmGetVBlankCount(),
                (long long)gcmGetLastFlipTime());
            PS3_RSX_ReportGraphicsErrors(seq);
            PS3_RSX_TraceHardwareState("flipwait-timeout", seq);
            return qfalse;
        }
        usleep(100);
    }

    if (loops || seq <= 3) {
        PS3_BootTraceFastf(
            "RSX_FLIPWAIT ready seq=%u loops=%u queued=%u completed=%u vblank=%llu lastFlip=%lld",
            seq, loops, (unsigned)ps3_flip_queued,
            (unsigned)ps3_flip_completed,
            (unsigned long long)gcmGetVBlankCount(),
            (long long)gcmGetLastFlipTime());
    }
    return qtrue;
}

/* Log command-buffer cursor near the frame where flip completion stalled.
 * A 4 MiB command buffer is used in this build to test whether the earlier
 * 35-flip stall correlates with the 1 MiB command-buffer wrap boundary. */
static void PS3_RSX_TraceCommandBuffer(const char *phase, unsigned seq)
{
    u32 usedBytes, capacityBytes;

    if (!ps3_gcm_context || !phase)
        return;
    if (seq > 5 && !(seq >= 30 && seq <= 42) && (seq % 10u) != 0)
        return;

    usedBytes = (u32)((ps3_gcm_context->current - ps3_gcm_context->begin) * sizeof(u32));
    capacityBytes = (u32)((ps3_gcm_context->end - ps3_gcm_context->begin) * sizeof(u32));

    PS3_BootTraceFastf(
        "RSX_CB %s seq=%u begin=%p current=%p end=%p used=%u capacity=%u",
        phase, seq, (void *)ps3_gcm_context->begin,
        (void *)ps3_gcm_context->current, (void *)ps3_gcm_context->end,
        (unsigned)usedBytes, (unsigned)capacityBytes);
}

/* Snapshot the staging FIFO after ps3gl_end_frame, preserving the tail immediately
 * before wrap and the head after wrap. A single [begin,current) copy loses the
 * pre-wrap commands when the GCM context callback moves current back to begin.
 * ctrl->put is a different address space and is never used as a FIFO index. */
static void PS3_RSX_CaptureCommandBuffer(unsigned seq, u32 host_put_start,
                                         u32 local_pre_offset, u32 local_end_offset)
{
    u32 capacity_words, used_words, pre_words, capture_words, i, tail_start_words, tail_words;

    ps3_rsx_dump_snapshot_valid = 0;
    ps3_rsx_dump_snapshot_seq = seq;
    ps3_rsx_dump_snapshot_words = 0;
    ps3_rsx_dump_snapshot_pre_bytes = local_pre_offset;
    ps3_rsx_dump_snapshot_used_bytes = 0;
    ps3_rsx_dump_snapshot_capacity_bytes = 0;
    ps3_rsx_dump_tail_start_bytes = 0;
    ps3_rsx_dump_tail_words = 0;

    if (!ps3_gcm_context || seq < 18 || seq > 30 ||
        !ps3_gcm_context->begin || !ps3_gcm_context->current || !ps3_gcm_context->end)
        return;

    if (ps3_gcm_context->end <= ps3_gcm_context->begin ||
        ps3_gcm_context->current < ps3_gcm_context->begin ||
        ps3_gcm_context->current > ps3_gcm_context->end) {
        PS3_BootTraceFastf(
            "RSX_DUMP_CAPTURE invalid-context seq=%u begin=%p current=%p end=%p hostPutStart=%08x preLocal=%08x",
            seq, (void *)ps3_gcm_context->begin, (void *)ps3_gcm_context->current,
            (void *)ps3_gcm_context->end, host_put_start, local_pre_offset);
        return;
    }

    capacity_words = (u32)(ps3_gcm_context->end - ps3_gcm_context->begin);
    used_words = (u32)((local_end_offset + 3u) / sizeof(u32));
    pre_words = local_pre_offset / sizeof(u32);
    if (used_words > capacity_words)
        used_words = (u32)(ps3_gcm_context->current - ps3_gcm_context->begin);

    ps3_rsx_dump_snapshot_capacity_bytes = capacity_words * sizeof(u32);
    ps3_rsx_dump_snapshot_used_bytes = used_words * sizeof(u32);

    capture_words = used_words;
    if (capture_words > PS3_RSX_DUMP_WORD_LIMIT)
        capture_words = PS3_RSX_DUMP_WORD_LIMIT;
    for (i = 0; i < capture_words; ++i)
        ps3_rsx_dump_snapshot[i] = ps3_gcm_context->begin[i];
    ps3_rsx_dump_snapshot_words = capture_words;

    tail_words = capacity_words < PS3_RSX_DUMP_TAIL_WORDS ? capacity_words : PS3_RSX_DUMP_TAIL_WORDS;
    tail_start_words = capacity_words - tail_words;
    for (i = 0; i < tail_words; ++i)
        ps3_rsx_dump_tail_snapshot[i] = ps3_gcm_context->begin[tail_start_words + i];
    ps3_rsx_dump_tail_start_bytes = tail_start_words * sizeof(u32);
    ps3_rsx_dump_tail_words = tail_words;
    ps3_rsx_dump_snapshot_valid = 1;

    PS3_BootTraceFastf(
        "RSX_DUMP_CAPTURE seq=%u hostPutStart=%08x preLocal=%08x postLocal=%08x fifoCapacity=%u headWords=%u tailStart=%08x tailWords=%u wrap=%u",
        seq, host_put_start, local_pre_offset, local_end_offset,
        (unsigned)ps3_rsx_dump_snapshot_capacity_bytes,
        (unsigned)ps3_rsx_dump_snapshot_words,
        (unsigned)ps3_rsx_dump_tail_start_bytes,
        (unsigned)ps3_rsx_dump_tail_words,
        (unsigned)(local_end_offset < local_pre_offset));
    if (capture_words < used_words)
        PS3_BootTraceFastf("RSX_DUMP_CAPTURE head-truncated seq=%u usedWords=%u capturedWords=%u",
            seq, used_words, capture_words);
    PS3_BootTraceFastf(
        "RSX_DUMP_BOUNDARY seq=%u preIndex=%u postIndex=%u fifoWords=%u",
        seq, pre_words, used_words, capacity_words);
}

/* Emit a region, then flag any words that also match RSX jump packet encodings.
 * These are only candidates until packet boundaries are decoded. */
static void PS3_RSX_DumpRegion(unsigned seq, const char *region, u32 base_bytes,
                               const u32 *words, u32 word_count)
{
    u32 emitted = 0, i;
    while (emitted < word_count) {
        char line[240];
        u32 n = word_count - emitted, j;
        int len;
        if (n > 12u) n = 12u;
        len = snprintf(line, sizeof(line), "RSX_DUMP_%s seq=%u localOff=%08x:",
            region, seq, base_bytes + emitted * (u32)sizeof(u32));
        for (j = 0; j < n && len > 0 && (size_t)len < sizeof(line); ++j)
            len += snprintf(line + len, sizeof(line) - (size_t)len,
                " %08x", (unsigned)words[emitted + j]);
        PS3_BootTraceFastf("%s", line);
        emitted += n;
    }

    for (i = 0; i < word_count; ++i) {
        u32 word = words[i];
        if ((word & 0xe0000003u) == 0x20000000u) {
            PS3_BootTraceFastf(
                "RSX_DUMP_JUMP_CANDIDATE seq=%u region=%s localOff=%08x kind=old-jump word=%08x target=%08x",
                seq, region, base_bytes + i * (u32)sizeof(u32), word,
                word & 0x1ffffffcu);
        } else if ((word & 0xe0000003u) == 0x00000001u) {
            PS3_BootTraceFastf(
                "RSX_DUMP_JUMP_CANDIDATE seq=%u region=%s localOff=%08x kind=new-jump word=%08x target=%08x",
                seq, region, base_bytes + i * (u32)sizeof(u32), word,
                word & 0xfffffffcu);
        }
    }
}

static void PS3_RSX_DumpCommandBuffer(unsigned seq, u32 host_put_start, u32 host_put_end)
{
    if (seq < 18 || seq > 30)
        return;
    if (!ps3_rsx_dump_snapshot_valid || ps3_rsx_dump_snapshot_seq != seq) {
        PS3_BootTraceFastf("RSX_DUMP missing snapshot seq=%u snapshotSeq=%u valid=%d",
            seq, ps3_rsx_dump_snapshot_seq, ps3_rsx_dump_snapshot_valid);
        return;
    }

    PS3_BootTraceFastf(
        "RSX_DUMP_BEGIN seq=%u hostPutStart=%08x hostPutEnd=%08x preLocal=%08x headBytes=%u fifoCapacity=%08x tailStart=%08x tailWords=%u headWords=%u",
        seq, host_put_start, host_put_end, ps3_rsx_dump_snapshot_pre_bytes,
        ps3_rsx_dump_snapshot_used_bytes, ps3_rsx_dump_snapshot_capacity_bytes,
        ps3_rsx_dump_tail_start_bytes, ps3_rsx_dump_tail_words,
        ps3_rsx_dump_snapshot_words);
    PS3_RSX_DumpRegion(seq, "TAIL", ps3_rsx_dump_tail_start_bytes,
                       ps3_rsx_dump_tail_snapshot, ps3_rsx_dump_tail_words);
    PS3_RSX_DumpRegion(seq, "HEAD", 0, ps3_rsx_dump_snapshot,
                       ps3_rsx_dump_snapshot_words);
    PS3_BootTraceFastf("RSX_DUMP_END seq=%u tailWords=%u headWords=%u",
        seq, ps3_rsx_dump_tail_words, ps3_rsx_dump_snapshot_words);
}
#define RSX_FB_ALIGN    64
#define RSX_DEPTH_ALIGN 64

static qboolean PS3_RSX_AllocFramebuffers(void)
{
    ps3_color_pitch = ps3_display_width * 4; /* XRGB8888 */

    u32 color_size = ps3_color_pitch * ps3_display_height;
    u32 depth_size = ps3_display_width * ps3_display_height * 4; /* Z24S8 */

    memset(ps3_color_buffer, 0, sizeof(ps3_color_buffer));
    ps3_depth_buffer = NULL;
    ps3_depth_offset = 0;

    for (int i = 0; i < RSX_FB_COUNT; i++) {
        ps3_color_buffer[i] = (u32 *)rsxMemalign(RSX_FB_ALIGN, color_size);
        if (!ps3_color_buffer[i]) {
            printf("[ps3] FATAL: rsxMemalign failed for color buffer %d\n", i);
            for (int j = 0; j < i; j++) {
                rsxFree(ps3_color_buffer[j]);
                ps3_color_buffer[j] = NULL;
            }
            return qfalse;
        }
        if (rsxAddressToOffset(ps3_color_buffer[i], &ps3_color_offset[i]) != 0) {
            printf("[ps3] FATAL: rsxAddressToOffset failed for color buffer %d\n", i);
            for (int j = 0; j <= i; j++) {
                rsxFree(ps3_color_buffer[j]);
                ps3_color_buffer[j] = NULL;
            }
            return qfalse;
        }

        gcmSetDisplayBuffer(i, ps3_color_offset[i],
                            ps3_color_pitch, ps3_display_width, ps3_display_height);
    }

    ps3_depth_buffer = (u32 *)rsxMemalign(RSX_DEPTH_ALIGN, depth_size);
    if (!ps3_depth_buffer || rsxAddressToOffset(ps3_depth_buffer, &ps3_depth_offset) != 0) {
        if (ps3_depth_buffer) {
            rsxFree(ps3_depth_buffer);
            ps3_depth_buffer = NULL;
        }
        for (int i = 0; i < RSX_FB_COUNT; i++) {
            rsxFree(ps3_color_buffer[i]);
            ps3_color_buffer[i] = NULL;
        }
        printf("[ps3] FATAL: failed to allocate/map Z24S8 depth buffer\n");
        return qfalse;
    }
    return qtrue;
}

static void PS3_RSX_SetRenderTarget(int index)
{
    if (index == ps3_current_rt) return;
    ps3_current_rt = index;

    gcmSurface sf;
    memset(&sf, 0, sizeof(sf));

    sf.colorFormat    = GCM_SURFACE_A8R8G8B8;
    sf.colorTarget    = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = ps3_color_offset[index];
    sf.colorPitch[0]    = ps3_color_pitch;

    for (int i = 1; i < 4; i++) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorOffset[i]   = ps3_color_offset[index];
        sf.colorPitch[i]    = 64;
    }

    sf.depthFormat    = GCM_SURFACE_ZETA_Z24S8;
    sf.depthLocation  = GCM_LOCATION_RSX;
    sf.depthOffset    = ps3_depth_offset;
    sf.depthPitch     = ps3_display_width * 4;

    sf.type           = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias      = GCM_SURFACE_CENTER_1;

    sf.width          = ps3_display_width;
    sf.height         = ps3_display_height;
    sf.x              = 0;
    sf.y              = 0;

    rsxSetSurface(ps3_gcm_context, &sf);
}

/* Public interface */

/* GCM IO window layout for this diagnostic:
 *   0x000000-0x3fffff: 4 MiB GCM command-buffer allocation
 *   0x400000-0x7fffff: 4 MiB ps3gl vertex ring (see ps3gl.h)
 * Keep these regions disjoint. Preserve the context boundaries and callback
 * supplied by GCM: extending context->end manually bypasses FIFO rollover and
 * previously correlated with RSX GET escaping into the adjacent IO region. */
#define RSX_CB_SIZE          (4 * 1024 * 1024) /* 4 MiB GCM command-buffer allocation */
#define RSX_HOST_SIZE       (8 * 1024 * 1024) /* 8 MiB IO buffer */
#if PS3GL_VRING_IO_OFFSET < RSX_CB_SIZE
#error "PS3GL vertex ring overlaps the GCM command buffer"
#endif
#if (PS3GL_VRING_IO_OFFSET + PS3GL_VRING_IO_SIZE) > RSX_HOST_SIZE
#error "PS3GL vertex ring exceeds the GCM IO window"
#endif

void PS3_RSX_Init(void)
{
    if (ps3_rsx_up && ps3_gcm_context)
        return;

    ps3_log("PS3_RSX_Init: entered");

    /* Allocate the 1 MB-aligned IO buffer. */
    void *host_addr = memalign(1024 * 1024, RSX_HOST_SIZE);
    if (!host_addr) {
        ps3_log("PS3_RSX_Init: FATAL memalign failed for the IO buffer");
        return;
    }

    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "PS3_RSX_Init: host_addr=%p cmdSize=0x%x ioSize=0x%x",
                 host_addr, RSX_CB_SIZE, RSX_HOST_SIZE);
        ps3_log(dbg);
    }

    ps3_rsx_io_alloc = host_addr;
    ps3_rsx_io_base = host_addr;
    ps3_rsx_io_size = RSX_HOST_SIZE;

    /* On the current PS3/PSL1GHT build, calling the default FIFO sizing API
     * before rsxInit wedges during startup. Do not call it here: let GCM
     * initialize its native FIFO and callback. The important safety rule is
     * not to widen context->end after rsxInit, as that bypasses rollover. */
    PS3_BootTraceFastf(
        "RSX_CB_CONFIG skipped: preserving native GCM FIFO defaults; custom sizing API stalls before rsxInit");

    s32 ret = rsxInit(&ps3_gcm_context, RSX_CB_SIZE, RSX_HOST_SIZE, host_addr);
    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "PS3_RSX_Init: rsxInit ret=%d ctx=%p",
                 (int)ret, (void*)ps3_gcm_context);
        ps3_log(dbg);
    }
    if (ret != 0 || !ps3_gcm_context) {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "PS3_RSX_Init: rsxInit FAILED (ret=0x%08x), trying fallbacks",
                 (unsigned)(ret & 0xFFFFFFFF));
        ps3_log(dbg);

        /* Try to recover existing GCM context. */
        rsxSetDefaultCommandBuffer(&ps3_gcm_context);
        snprintf(dbg, sizeof(dbg),
                 "PS3_RSX_Init: rsxSetDefaultCommandBuffer ctx=%p",
                 (void*)ps3_gcm_context);
        ps3_log(dbg);

        if (ps3_gcm_context) {
            ps3_log("PS3_RSX_Init: recovered existing GCM context");
        } else {
            /* Try gcmInitBody as final fallback. */
            ret = gcmInitBody(&ps3_gcm_context, RSX_CB_SIZE, RSX_HOST_SIZE, host_addr);
            snprintf(dbg, sizeof(dbg),
                     "PS3_RSX_Init: gcmInitBody ret=%d ctx=%p",
                     (int)ret, (void*)ps3_gcm_context);
            ps3_log(dbg);

            if (ret == 0 && ps3_gcm_context) {
                rsxHeapInit();
                ps3_log("PS3_RSX_Init: gcmInitBody fallback succeeded");
            } else {
                ps3_log("PS3_RSX_Init: FATAL all GCM init attempts failed");
                return;
            }
        }
    }

    /* Observe (but do not rewrite) the native segment provided by GCM. */
    if (ps3_gcm_context && ps3_gcm_context->begin && ps3_gcm_context->end) {
        u32 span_bytes = (u32)(((char *)ps3_gcm_context->end -
                                (char *)ps3_gcm_context->begin));
        PS3_BootTraceFastf(
            "RSX_CB_SPAN native begin=%p current=%p end=%p activeBytes=%u callbackPresent=%u commandAllocationBytes=%u",
            (void *)ps3_gcm_context->begin, (void *)ps3_gcm_context->current,
            (void *)ps3_gcm_context->end, (unsigned)span_bytes,
            (unsigned)(ps3_gcm_context->callback != NULL),
            (unsigned)RSX_CB_SIZE);
    }

    /* rsxInit allocates the requested 4 MiB command area, but PSL1GHT's live
     * context starts with a 32 KiB default segment. This renderer emits GCM
     * commands through both safe helpers and batched GL operations, so the
     * default two-segment callback can expose a PUT/GET discontinuity at 64 KiB.
     * Use the full allocation as a linear span and explicitly wrap it from a
     * jump command after draining the stream. Keep the reserved 4 KiB prefix
     * before begin and the 16-byte guard at the allocation end. */
    if (ps3_gcm_context && ps3_gcm_context->begin && ps3_gcm_context->current) {
        char *alloc_begin = (char *)host_addr;
        char *alloc_end = alloc_begin + RSX_CB_SIZE;
        char *fifo_begin = (char *)ps3_gcm_context->begin;
        if (fifo_begin >= alloc_begin && fifo_begin < alloc_end &&
            alloc_end - fifo_begin >= 1024 * 1024) {
            const u32 old_span = (u32)((char *)ps3_gcm_context->end - fifo_begin);
            ps3_gcm_context->end = (u32 *)(alloc_end - 16);
            PS3_BootTraceFastf(
                "RSX_CB_SPAN linear begin=%p current=%p oldBytes=%u newBytes=%u callbackPresent=%u",
                (void *)ps3_gcm_context->begin, (void *)ps3_gcm_context->current,
                (unsigned)old_span,
                (unsigned)((char *)ps3_gcm_context->end - fifo_begin),
                (unsigned)(ps3_gcm_context->callback != NULL));
        } else {
            PS3_BootTraceFastf(
                "RSX_CB_SPAN FATAL invalid allocation begin=%p end=%p host=%p commandBytes=%u",
                (void *)fifo_begin, (void *)ps3_gcm_context->end, host_addr,
                (unsigned)RSX_CB_SIZE);
            ri.Error(ERR_FATAL, "PS3 RSX command-buffer allocation/context span mismatch");
            return;
        }
    } else {
        PS3_BootTraceFastf("RSX_CB_SPAN FATAL missing context/begin ctx=%p",
            (void *)ps3_gcm_context);
        ri.Error(ERR_FATAL, "PS3 RSX command-buffer context is incomplete");
        return;
    }

    /* Default to 720p: 2.25x fewer pixels than 1080p, which is what the RSX and
     * the fill-rate-bound GL1 renderer need to run well. USRDIR/video_mode.txt
     * containing 1080 selects 1080p instead. If the preferred mode is not exposed
     * by the TV/HDMI chain, fall back to the other one, then the current TV mode. */
    int want1080 = 0;
    {
        FILE *vf = fopen(PS3_USRDIR "/video_mode.txt", "rb");
        if (vf) {
            char vb[8] = {0};
            size_t got = fread(vb, 1, 4, vf);
            fclose(vf);
            if (got == 4 && strncmp(vb, "1080", 4) == 0) {
                want1080 = 1;
            }
        }
    }
    s32 vid_res = want1080 ? VIDEO_RESOLUTION_1080 : VIDEO_RESOLUTION_720;
    s32 alt_res = want1080 ? VIDEO_RESOLUTION_720 : VIDEO_RESOLUTION_1080;
    u8  vid_aspect = VIDEO_ASPECT_16_9;

    if (!videoGetResolutionAvailability(VIDEO_PRIMARY, vid_res,
                                        VIDEO_ASPECT_16_9, 0)) {
        ps3_log(want1080 ? "PS3_RSX_Init: 1080p unavailable, trying 720p"
                         : "PS3_RSX_Init: 720p unavailable, trying 1080p");
        vid_res = alt_res;
        if (!videoGetResolutionAvailability(VIDEO_PRIMARY, vid_res,
                                            VIDEO_ASPECT_16_9, 0)) {
            videoState state;
            videoGetState(0, 0, &state);
            vid_res = state.displayMode.resolution;
            vid_aspect = state.displayMode.aspect;
            ps3_log("PS3_RSX_Init: preferred modes unavailable, using TV default");
        }
    }

    videoResolution res;
    videoGetResolution(vid_res, &res);

    ps3_display_width  = res.width;
    ps3_display_height = res.height;
    printf("[ps3] Display: %ux%u\n", ps3_display_width, ps3_display_height);

    videoConfiguration vconfig;
    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution  = vid_res;
    vconfig.format      = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch       = ps3_display_width * 4;
    vconfig.aspect      = vid_aspect;
    videoConfigure(0, &vconfig, NULL, 0);

    if (!PS3_RSX_AllocFramebuffers()) {
        rsxFinish(ps3_gcm_context, 1);
        free(host_addr);
        ps3_rsx_io_base = NULL;
        ps3_rsx_io_size = 0;
        ps3_gcm_context = NULL;
        return;
    }

    ps3_current_fb = 0;
    ps3_flip_queued    = 0;
    ps3_flip_completed = 0;
    ps3_graphics_error_count = 0;
    ps3_graphics_error_value = 0;
    ps3_reported_graphics_errors = 0;
    gcmSetGraphicsHandler(ps3_graphics_handler);
    gcmSetFlipHandler(ps3_flip_handler);
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    gcmResetFlipStatus();
    PS3_RSX_SetRenderTarget(ps3_current_fb);

    ps3gl_init(ps3_gcm_context, ps3_display_width, ps3_display_height);

    {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
                 "PS3_RSX_Init: after ps3gl_init ps3gl_ptr=%p ctx=%p",
                 (void*)ps3gl_ptr, (void*)ps3_gcm_context);
        ps3_log(dbg);
    }

    /* Backup context pointer. */
    ps3_gcm_context_backup = ps3_gcm_context;
    ps3_rsx_up = 1;

    /* On-screen boot log: the CPU draws text straight into the display
     * buffers, then buffer 0 is flipped in exactly like a rendered frame so
     * it becomes visible. It survives a GPU or console freeze, because the
     * display controller keeps scanning out the last flipped buffer. */
    PS3_ScreenLog_Attach(ps3_color_buffer, RSX_FB_COUNT, (int)ps3_display_width,
                         (int)ps3_display_height, (int)ps3_color_pitch);
    /* Submit the visible boot page first. Do not leave a wait-flip command
     * sitting in the unflushed command buffer; EndFrame queues the donor-style
     * wait-flip and current flip together, then flushes the pair. */
    PS3_BootTraceFastf("RSX_INIT before boot flip fb=%d queued=%u completed=%u",
        ps3_current_fb, (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed);
    {
        s32 flip_ret = gcmSetFlip(ps3_gcm_context, ps3_current_fb);
        if (flip_ret != 0) {
            PS3_BootTraceFastf(
                "RSX_INIT FATAL gcmSetFlip failed fb=%d ret=0x%08x",
                ps3_current_fb, (unsigned)flip_ret);
            ri.Error(ERR_FATAL, "PS3 RSX boot gcmSetFlip failed (0x%08x)",
                     (unsigned)flip_ret);
            return;
        }
    }
    ps3_flip_queued++;
    rsxFlushBuffer(ps3_gcm_context);
    PS3_BootTraceFastf("RSX_INIT after boot flip/flush queued=%u completed=%u",
        (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed);
    ps3_current_fb = (ps3_current_fb + 1) % RSX_FB_COUNT;

    /* The boot page has been queued for display. Do not continue drawing the
     * CPU-side logger into RSX-owned framebuffers while the renderer initializes.
     * File and UDP diagnostics remain enabled; only direct framebuffer writes stop. */
    PS3_ScreenLog_Disable();

    {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
                 "PS3_RSX_Init: ctx=%p &ctx=%p backup=%p ps3gl_ptr=%p sizeof=%u",
                 (void*)ps3_gcm_context,
                 (void*)&ps3_gcm_context,
                 (void*)ps3_gcm_context_backup,
                 (void*)ps3gl_ptr,
                 (unsigned)sizeof(ps3gl_state_t));
        ps3_log(dbg);
    }
}

void PS3_RSX_Shutdown(void)
{
    static u32 finish_ref = 0x1000;

    if (!ps3_rsx_up)
        return;
    ps3_rsx_up = 0;

    /* A new reference every time: rsxFinish with a value it already saw can
     * hang the whole console. Keep callbacks registered while the final GCM
     * stream drains, and do not free shader/texture memory while RSX may still
     * be reading it. */
    PS3_BootTracef("RSX_SHUTDOWN before rsxFinish queued=%u completed=%u",
                   (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed);
    rsxFinish(ps3_gcm_context, ++finish_ref);
    PS3_BootTracef("RSX_SHUTDOWN after rsxFinish ref=%u queued=%u completed=%u",
                   (unsigned)finish_ref, (unsigned)ps3_flip_queued,
                   (unsigned)ps3_flip_completed);

    /* No more GCM work is in flight, so handlers and RSX-owned allocations
     * can now be torn down without racing the GPU. */
    gcmSetFlipHandler(NULL);
    gcmSetGraphicsHandler(NULL);

    ps3_current_rt = -1;
    ps3gl_shutdown();

    for (int i = 0; i < RSX_FB_COUNT; i++) {
        if (ps3_color_buffer[i]) {
            rsxFree(ps3_color_buffer[i]);
            ps3_color_buffer[i] = NULL;
        }
    }
    if (ps3_depth_buffer) {
        rsxFree(ps3_depth_buffer);
        ps3_depth_buffer = NULL;
    }

    if (ps3_rsx_io_alloc) {
        free(ps3_rsx_io_alloc);
        ps3_rsx_io_alloc = NULL;
    }
    ps3_rsx_io_base = NULL;
    ps3_rsx_io_size = 0;
    ps3_gcm_context = NULL;
    ps3_gcm_context_backup = NULL;
}

/* Flip completion is ordered by the donor-style GCM wait-flip command queued
 * before each frame flip and flushed in the same command batch. The PPU uses
 * the asynchronous callback count only to throttle triple buffering when two
 * flips are outstanding; it never polls gcmGetFlipStatus() or fakes completion. */

/* Wrap the expanded command FIFO only when at least 1 MiB remains. This
 * avoids letting PSL1GHT's native 32 KiB callback switch to a new staging span
 * while the RSX PUT/GET addresses still describe the old span. The jump is
 * submitted after all prior commands have drained; GET must reach the mapped
 * context start before the PPU is allowed to reuse that memory. */
static qboolean PS3_RSX_WrapCommandBufferIfNeeded(unsigned seq)
{
    gcmContextData *ctx = ps3_gcm_context;
    gcmControlRegister *ctrl = gcmGetControlRegister();
    char *begin, *cur, *end;
    ptrdiff_t remaining;
    u32 start_offset = 0;
    unsigned waited;

    if (!ctx || !ctx->begin || !ctx->current || !ctx->end || !ctrl)
        return qfalse;

    begin = (char *)ctx->begin;
    cur = (char *)ctx->current;
    end = (char *)ctx->end;
    if (cur < begin || cur > end) {
        PS3_BootTraceFastf(
            "RSX_CB_WRAP FATAL invalid cursor seq=%u begin=%p current=%p end=%p",
            seq, (void *)ctx->begin, (void *)ctx->current, (void *)ctx->end);
        return qfalse;
    }
    remaining = end - cur;
    if (remaining >= 1024 * 1024)
        return qtrue;

    if (gcmAddressToOffset(ctx->begin, &start_offset) != 0) {
        PS3_BootTraceFastf("RSX_CB_WRAP FATAL could not map begin seq=%u begin=%p",
            seq, (void *)ctx->begin);
        return qfalse;
    }

    PS3_BootTraceFastf(
        "RSX_CB_WRAP begin seq=%u begin=%p current=%p end=%p remaining=%u startOffset=%08x put=%08x get=%08x",
        seq, (void *)ctx->begin, (void *)ctx->current, (void *)ctx->end,
        (unsigned)remaining, start_offset, (unsigned)ctrl->put, (unsigned)ctrl->get);

    /* First drain the last submitted stream, including its flip. */
    for (waited = 0; ctrl->get != ctrl->put && waited < 30000u; ++waited)
        usleep(100);
    if (ctrl->get != ctrl->put) {
        PS3_BootTraceFastf(
            "RSX_CB_WRAP FATAL pre-jump drain timeout seq=%u loops=%u put=%08x get=%08x",
            seq, waited, (unsigned)ctrl->put, (unsigned)ctrl->get);
        return qfalse;
    }

    /* PSL1GHT ResetCommandBuffer protocol: append the JUMP at the current
     * cursor, then move PUT to the jump target (not to current). If PUT is
     * left at the post-jump cursor, RSX treats the old bytes between begin
     * and PUT as live commands and runs through stale FIFO contents. */
    rsxSetJumpCommand(ctx, start_offset);
    __sync_synchronize();
    ctrl->put = start_offset;
    PS3_BootTraceFastf(
        "RSX_CB_WRAP jump submitted seq=%u startOffset=%08x put=%08x get=%08x current=%p",
        seq, start_offset, (unsigned)ctrl->put, (unsigned)ctrl->get,
        (void *)ctx->current);

    for (waited = 0; ctrl->get != start_offset && waited < 30000u; ++waited)
        usleep(100);
    if (ctrl->get != start_offset) {
        PS3_BootTraceFastf(
            "RSX_CB_WRAP FATAL jump wait timeout seq=%u loops=%u expectedGet=%08x put=%08x get=%08x",
            seq, waited, start_offset, (unsigned)ctrl->put, (unsigned)ctrl->get);
        return qfalse;
    }

    ctx->current = ctx->begin;
    __sync_synchronize();
    PS3_BootTraceFastf(
        "RSX_CB_WRAP complete seq=%u current reset to %p offset=%08x",
        seq, (void *)ctx->current, start_offset);
    return qtrue;
}

void PS3_RSX_BeginFrame(void)
{
    unsigned seq;
    /* Restore from .data backup if corrupted */
    if ((uintptr_t)ps3_gcm_context <= 0x1000
        && (uintptr_t)ps3_gcm_context_backup > 0x1000)
        ps3_gcm_context = ps3_gcm_context_backup;

    if ((uintptr_t)ps3_gcm_context <= 0x1000) {
        PS3_BootTraceFastf("RSX_BEGIN no context ctx=%p backup=%p",
            (void *)ps3_gcm_context, (void *)ps3_gcm_context_backup);
        return;
    }

    seq = ++ps3_frame_begin_seq;
    PS3_RSX_ReportGraphicsErrors(seq);
    if (seq >= 20 && seq <= 30)
        PS3_RSX_TraceHardwareState("begin-enter", seq);
    PS3_BootTraceFastf("RSX_BEGIN seq=%u enter fb=%d queued=%u completed=%u",
        seq, ps3_current_fb, (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed);

    if (ps3_frame_begin_trace_count == 0)
        PS3_ScreenLog_Disable();    /* the GPU owns the buffers from here on */

    if (ps3_frame_begin_trace_count < 3) {
        PS3_BootTracef("RSX: BeginFrame #%u fb=%d ctx=%p",
                 ++ps3_frame_begin_trace_count, ps3_current_fb, (void *)ps3_gcm_context);
    }

    /* Follow the donor ports' triple-buffer strategy. One queued flip is
     * normal: keep rendering into the third framebuffer instead of blocking
     * the whole game on a single gcmGetFlipStatus() result. The callback count
     * is used as completion telemetry and throttles only when two flips are
     * outstanding. */
    PS3_BootTraceFastf("RSX_BEGIN seq=%u triple-buffer throttle queued=%u completed=%u outstanding=%u",
        seq, (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed,
        (unsigned)(u32)(ps3_flip_queued - ps3_flip_completed));

    PS3_RSX_TraceCommandBuffer("begin-before-flipwait", seq);
    if (!PS3_RSX_WaitFlips(seq)) {
        PS3_RSX_TraceCommandBuffer("begin-flipwait-timeout", seq);
        ri.Error(ERR_FATAL,
                 "PS3 RSX flip queue stalled at frame %u; see openmohaa-ps3.log",
                 seq);
        return;
    }
    PS3_RSX_TraceCommandBuffer("begin-after-flipwait", seq);
    if (!PS3_RSX_WrapCommandBufferIfNeeded(seq)) {
        ri.Error(ERR_FATAL, "PS3 RSX command-buffer wrap failed at frame %u; see openmohaa-ps3.log", seq);
        return;
    }
    PS3_RSX_SetRenderTarget(ps3_current_fb);
    PS3_ApplyScreenFit();
    {
        /* waits for the GPU to be done with this vertex ring segment */
        unsigned long long t0 = PS3_ProfTick();
        PS3_BootTraceFastf("RSX_BEGIN seq=%u before ps3gl_begin_frame fb=%d", seq, ps3_current_fb);
        ps3gl_begin_frame();
        PS3_BootTraceFastf("RSX_BEGIN seq=%u after ps3gl_begin_frame fb=%d", seq, ps3_current_fb);
        PS3_ProfAdd(PS3_PROF_FENCEWAIT, PS3_ProfTick() - t0);
    }
    /* triple buffering: every buffer needs its screen-fit border cleared */
    if (ps3_fit_border) {
        PS3_BootTraceFastf("RSX_BEGIN seq=%u before screen-fit clear", seq);
        ps3gl_clear_full_surface();
        PS3_BootTraceFastf("RSX_BEGIN seq=%u after screen-fit clear", seq);
    }
    PS3_BootTraceFastf("RSX_BEGIN seq=%u return fb=%d", seq, ps3_current_fb);
}

void PS3_RSX_EndFrame(void)
{
    /* Restore from .data backup if corrupted */
    if ((uintptr_t)ps3_gcm_context <= 0x1000
        && (uintptr_t)ps3_gcm_context_backup > 0x1000)
        ps3_gcm_context = ps3_gcm_context_backup;

    if ((uintptr_t)ps3_gcm_context <= 0x1000) return;

    if (ps3_frame_end_trace_count < 3) {
        PS3_BootTracef("RSX: EndFrame #%u fb=%d",
                 ++ps3_frame_end_trace_count, ps3_current_fb);
    }

    PS3_RSX_ReportGraphicsErrors(ps3_frame_begin_seq);
    if (ps3_frame_begin_seq >= 20 && ps3_frame_begin_seq <= 30)
        PS3_RSX_TraceHardwareState("end-before-render-submit", ps3_frame_begin_seq);
    /* The control-register PUT is the first not-yet-consumed boundary for the
     * commands this frame is about to flush. Preserve it before appending the
     * final backend fence so the dump includes the complete render stream. */
    u32 ps3_stream_start_offset = 0xffffffffu;
    gcmControlRegister *ps3_stream_ctrl = gcmGetControlRegister();
    if (ps3_stream_ctrl)
        ps3_stream_start_offset = ps3_stream_ctrl->put;
    PS3_RSX_TraceCommandBuffer("end-before-render-submit", ps3_frame_begin_seq);
    u32 ps3_stream_local_pre_offset = 0xffffffffu;
    if (ps3_gcm_context && ps3_gcm_context->current && ps3_gcm_context->begin)
        ps3_stream_local_pre_offset = (u32)((ps3_gcm_context->current - ps3_gcm_context->begin) * sizeof(u32));
    ps3gl_end_frame();
    PS3_RSX_TraceCommandBuffer("end-after-ps3gl", ps3_frame_begin_seq);
    u32 ps3_stream_local_end_offset = 0xffffffffu;
    if (ps3_gcm_context && ps3_gcm_context->current && ps3_gcm_context->begin)
        ps3_stream_local_end_offset = (u32)((ps3_gcm_context->current - ps3_gcm_context->begin) * sizeof(u32));
    /* Submit once per frame.  The old diagnostic split flushed renderer commands,
     * waited for the backend label, then queued the flip and flushed a second time.
     * At a native 32 KiB GCM segment boundary this exercises two independent flushes
     * while the context callback is switching segments.  Use the normal ordering:
     * backend label, flip, one flush.  The backend label is still before the flip,
     * so it tells us whether the renderer command stream completed. */
    {
        unsigned seg = (unsigned)ps3gl.vring.cur_seg;
        volatile u32 *label = ps3gl.vring.fence_label[seg];
        u32 target = ps3gl.vring.fence_val[seg];
        unsigned errors_before = (unsigned)ps3_graphics_error_count;
        unsigned loops = 0;
        s32 flip_ret;

        PS3_BootTraceFastf(
            "RSX_SUBMIT combined seq=%u seg=%u label=%p target=%u current=%u errors=%u",
            ps3_frame_begin_seq, seg, (void *)label, (unsigned)target,
            label ? (unsigned)*label : 0xffffffffu, errors_before);
        if (!label) {
            PS3_BootTraceFastf("RSX_SUBMIT FATAL missing backend fence seq=%u seg=%u",
                ps3_frame_begin_seq, seg);
            ri.Error(ERR_FATAL, "PS3 RSX missing backend fence at frame %u",
                     ps3_frame_begin_seq);
            return;
        }

        PS3_BootTraceFastf(
            "RSX_END before gcmSetFlip fb=%d queued=%u completed=%u",
            ps3_current_fb, (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed);
        flip_ret = gcmSetFlip(ps3_gcm_context, ps3_current_fb);
        if (flip_ret != 0) {
            PS3_BootTraceFastf(
                "RSX_END FATAL gcmSetFlip failed seq=%u fb=%d ret=0x%08x",
                ps3_frame_begin_seq, ps3_current_fb, (unsigned)flip_ret);
            ri.Error(ERR_FATAL, "PS3 RSX gcmSetFlip failed (0x%08x)",
                     (unsigned)flip_ret);
            return;
        }

        /* Capture the pending native segment after both the renderer fence and flip
         * have been appended, immediately before the single flush. */
        if (ps3_gcm_context && ps3_gcm_context->current && ps3_gcm_context->begin)
            ps3_stream_local_end_offset = (u32)((ps3_gcm_context->current -
                ps3_gcm_context->begin) * sizeof(u32));
        PS3_RSX_CaptureCommandBuffer(ps3_frame_begin_seq, ps3_stream_start_offset,
            ps3_stream_local_pre_offset, ps3_stream_local_end_offset);

        /* Increment before flush: the flip callback may race with the flush return. */
        ps3_flip_queued++;
        PS3_BootTraceFastf("RSX_SUBMIT before single flush seq=%u queued=%u completed=%u",
            ps3_frame_begin_seq, (unsigned)ps3_flip_queued,
            (unsigned)ps3_flip_completed);
        rsxFlushBuffer(ps3_gcm_context);
        PS3_RSX_TraceCommandBuffer("after-single-flush", ps3_frame_begin_seq);
        PS3_RSX_TraceHardwareState("after-single-flush", ps3_frame_begin_seq);

        /* The backend fence is emitted before the flip.  Wait for the fence or a
         * graphics exception; do not issue a second flush at a segment boundary. */
        while (*label != target && loops < 5000u &&
               (unsigned)ps3_graphics_error_count == errors_before) {
            if (loops == 0 || (loops % 1000u) == 0) {
                PS3_RSX_TraceHardwareState("combined-submit-wait", ps3_frame_begin_seq);
                PS3_RSX_ReportGraphicsErrors(ps3_frame_begin_seq);
            }
            usleep(100);
            loops++;
        }

        PS3_RSX_TraceHardwareState("combined-submit-result", ps3_frame_begin_seq);
        PS3_RSX_ReportGraphicsErrors(ps3_frame_begin_seq);
        if (*label != target ||
            (unsigned)ps3_graphics_error_count != errors_before) {
            PS3_BootTraceFastf(
                "RSX_SUBMIT FAILED seq=%u seg=%u target=%u current=%u loops=%u errors=%u value=%08x",
                ps3_frame_begin_seq, seg, (unsigned)target, (unsigned)*label, loops,
                (unsigned)ps3_graphics_error_count, (unsigned)ps3_graphics_error_value);
            gcmControlRegister *dump_ctrl = gcmGetControlRegister();
            u32 ps3_stream_host_end_offset = dump_ctrl ? dump_ctrl->put : 0xffffffffu;
            PS3_RSX_DumpCommandBuffer(ps3_frame_begin_seq,
                ps3_stream_start_offset, ps3_stream_host_end_offset);
            ri.Error(ERR_FATAL,
                "PS3 RSX combined command stream failed before backend fence at frame %u; see openmohaa-ps3.log",
                ps3_frame_begin_seq);
            return;
        }
        PS3_BootTraceFastf(
            "RSX_SUBMIT PASSED seq=%u seg=%u target=%u loops=%u queued=%u completed=%u",
            ps3_frame_begin_seq, seg, (unsigned)target, loops,
            (unsigned)ps3_flip_queued, (unsigned)ps3_flip_completed);
    }

    ps3_current_fb = (ps3_current_fb + 1) % RSX_FB_COUNT;
}

/* GLimp interface -- called by the iortcw renderer */
void GLimp_Init(qboolean fixedFunction)
{
    (void)fixedFunction;

    /* RSX/ps3gl must exist before qgl* entry points are wired. */
    PS3_BootTracef("GLimp_Init: entered");
    PS3_RSX_Init();
    PS3_BootTracef("GLimp_Init: PS3_RSX_Init returned");
    if (!ps3_rsx_up || !ps3_gcm_context || (uintptr_t)ps3gl_ptr <= 0x1000) {
        PS3_BootTracef("GLimp_Init: FATAL RSX/ps3gl initialization incomplete");
        ri.Error(ERR_FATAL, "PS3 RSX initialization failed; see openmohaa-ps3.log");
        return;
    }
    PS3_BootTracef("GLimp_Init: RSX/ps3gl ready; initializing qgl entry points");
    QGL_Init();
    PS3_BootTracef("GLimp_Init: QGL_Init returned");

    /* Screen fit (TV overscan): the engine sees the whole 1280x720 and
     * ps3gl scales it into a centered rect of vid_ps3_screenfit percent
     * (PS3_ApplyScreenFit, every frame: the menu slider applies live). */
    ps3_fit_cvar = ri.Cvar_Get("vid_ps3_screenfit", PS3_DEFAULT_SCREENFIT, CVAR_ARCHIVE);
    ps3_fit_pct = -1;
    PS3_ApplyScreenFit();
    glConfig.vidWidth  = ps3_display_width;
    glConfig.vidHeight = ps3_display_height;
    glConfig.windowAspect       = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
    glConfig.colorBits          = 32;
    glConfig.depthBits          = 24;
    glConfig.stencilBits        = 8;
    glConfig.isFullscreen       = qtrue;
    glConfig.stereoEnabled      = qfalse;
    glConfig.smpActive          = qfalse;
    glConfig.displayFrequency   = 60;
    glConfig.driverType         = GLDRV_ICD;
    glConfig.hardwareType       = GLHW_GENERIC;
    glConfig.deviceSupportsGamma = qfalse;
    glConfig.maxTextureSize     = 2048;
    glConfig.numTextureUnits    = PS3GL_MAX_TMUS;
    glConfig.textureCompression = TC_NONE;
    glConfig.textureEnvAddAvailable = qtrue;

    textureFilterAnisotropic = qfalse;
    maxAnisotropy = 0;
    haveClampToEdge = qtrue;

    Q_strncpyz(glConfig.vendor_string, "Sony", sizeof(glConfig.vendor_string));
    Q_strncpyz(glConfig.renderer_string, "RSX (ps3gl)", sizeof(glConfig.renderer_string));
    Q_strncpyz(glConfig.version_string, "1.1 ps3gl", sizeof(glConfig.version_string));
    Q_strncpyz(glConfig.extensions_string,
               "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_EXT_texture_env_add",
               sizeof(glConfig.extensions_string));

    ri.Cvar_Get("r_availableModes", "", CVAR_ROM);

    ri.IN_Init(NULL);
}

void GLimp_Shutdown(void)
{
    /* The RSX stays up for the whole process (vid_restart keeps it). */
}

void GLimp_EndFrame(void)
{
    PS3_RSX_EndFrame();
}

void GLimp_Minimize(void)
{
}

void GLimp_SetGamma(unsigned char red[256], unsigned char green[256],
                    unsigned char blue[256])
{
    (void)red; (void)green; (void)blue;
}

void GLimp_LogComment(char *comment)
{
    (void)comment;
}

int PS3_RSX_Width(void)  { return (int)ps3_display_width; }
int PS3_RSX_Height(void) { return (int)ps3_display_height; }

/* ---- SMP: the renderer back end on the PPU's second hardware thread -------
 *
 * The front end (game thread: server, cgame, culling, sorting) builds frame
 * N+1 while this thread runs the back end of frame N (shading, skinning,
 * ps3gl, flips). Same scheme as the r_smp of the original RTCW: two
 * backEndData buffers, the front end waits for the back end before handing
 * it the next one (GLimp_FrontEndSleep) and before any GL call of its own
 * (R_IssuePendingRenderCommands). ps3gl is only ever used by one thread at a
 * time; PS3GL_FRONT_CHECK in ps3gl logs and waits if that is ever broken. */

#include <sys/thread.h>
#include <sys/sem.h>
#include <lv2/thread.h>

#define PS3_RENDER_STACK    (512 * 1024)
#define PS3_RENDER_PRIO     1000        /* same as the game thread */

static sys_ppu_thread_t ps3_render_tid;
static sys_sem_t ps3_render_work, ps3_render_done;
static void (*ps3_render_func)(void);
static void *volatile ps3_render_data;
static int ps3_render_up;
static int ps3_render_pending;          /* front end only: a batch not waited for */

volatile int ps3_render_busy;           /* the back end is running a batch */
uintptr_t ps3_render_stack_lo, ps3_render_stack_hi;

static void ps3_render_thread(void *arg)
{
    sys_ppu_thread_stack_t st;
    char marker;

    (void)arg;
    /* the render thread's stack, for PS3GL_FRONT_CHECK; must contain this
     * frame or every back end call would look like the game thread's */
    if (sysThreadGetStackInformation(&st) == 0 && st.addr && st.size
        && (uintptr_t)&marker >= (uintptr_t)st.addr
        && (uintptr_t)&marker < (uintptr_t)st.addr + st.size) {
        ps3_render_stack_lo = (uintptr_t)st.addr;
        ps3_render_stack_hi = (uintptr_t)st.addr + st.size;
    } else {
        ps3_render_stack_hi = (uintptr_t)&marker + 4096;
        ps3_render_stack_lo = ps3_render_stack_hi - PS3_RENDER_STACK + 8192;
    }
    PS3_Logf("[smp] render thread stack 0x%lx-0x%lx",
             (unsigned long)ps3_render_stack_lo, (unsigned long)ps3_render_stack_hi);
    ps3_render_func();
    sysThreadExit(0);
}

qboolean GLimp_SpawnRenderThread(void (*function)(void))
{
    sys_sem_attr_t attr;
    s32 ret;

    if (ps3_render_up)
        return qtrue;

    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
    attr.attr_pshared  = SYS_SEM_ATTR_PSHARED;
    memcpy(attr.name, "rwork", 6);
    if (sysSemCreate(&ps3_render_work, &attr, 0, 1) != 0)
        return qfalse;
    memcpy(attr.name, "rdone", 6);
    if (sysSemCreate(&ps3_render_done, &attr, 0, 1) != 0) {
        sysSemDestroy(ps3_render_work);
        return qfalse;
    }

    ps3_render_func = function;
    ps3_render_data = NULL;
    ps3_render_pending = 0;
    ps3_render_busy = 0;
    ret = sysThreadCreate(&ps3_render_tid, ps3_render_thread, NULL, PS3_RENDER_PRIO,
                          PS3_RENDER_STACK, THREAD_JOINABLE, "render");
    if (ret != 0) {
        PS3_Logf("[smp] sysThreadCreate failed (0x%08x): back end stays on the game thread",
                 (unsigned)ret);
        sysSemDestroy(ps3_render_done);
        sysSemDestroy(ps3_render_work);
        return qfalse;
    }
    ps3_render_up = 1;
    ps3_prof_smp = 1;
    PS3_Logf("[smp] render thread up: the back end runs on the 2nd PPU thread");
    return qtrue;
}

/* render thread: report the last batch done, sleep until the next one */
void *GLimp_RendererSleep(void)
{
    static int had_batch;
    void *data;

    if (had_batch) {
        __asm__ volatile("sync" ::: "memory");
        ps3_render_busy = 0;
        sysSemPost(ps3_render_done, 1);
    }
    sysSemWait(ps3_render_work, 0);
    __asm__ volatile("sync" ::: "memory");
    data = ps3_render_data;
    had_batch = (data != NULL);
    return data;
}

/* front end: wait until the back end is idle (no-op if it already is) */
void GLimp_FrontEndSleep(void)
{
    unsigned long long t0;

    if (!ps3_render_pending)
        return;
    t0 = PS3_ProfTick();
    sysSemWait(ps3_render_done, 0);
    __asm__ volatile("sync" ::: "memory");
    ps3_render_pending = 0;
    PS3_ProfAdd(PS3_PROF_RWAIT, PS3_ProfTick() - t0);
}

/* front end: hand the back end a command list (NULL: the thread exits) */
void GLimp_WakeRenderer(void *data)
{
    GLimp_FrontEndSleep();
    ps3_render_data = data;
    ps3_render_pending = (data != NULL);
    ps3_render_busy = (data != NULL);
    __asm__ volatile("sync" ::: "memory");
    sysSemPost(ps3_render_work, 1);
}

void GLimp_ShutdownRenderThread(void)
{
    u64 retval;

    if (!ps3_render_up)
        return;
    GLimp_WakeRenderer(NULL);
    sysThreadJoin(ps3_render_tid, &retval);
    sysSemDestroy(ps3_render_done);
    sysSemDestroy(ps3_render_work);
    ps3_render_up = 0;
    ps3_prof_smp = 0;
    ps3_render_busy = 0;
    ps3_render_stack_lo = ps3_render_stack_hi = 0;
}

/* ps3gl called from the game thread while the back end runs: a missing sync
 * in the renderer. Say where once, and wait so both do not write RSX commands
 * at the same time. */
void ps3gl_front_call(const char *func)
{
    static const char *logged[16];
    sys_ppu_thread_t self;
    int i;

    /* never wait on ourselves: if the stack range is wrong, stop checking */
    if (sysThreadGetId(&self) == 0 && self == ps3_render_tid) {
        PS3_Logf("[smp] %s: render thread outside its stack range, check off", func);
        ps3_render_stack_lo = 0;
        ps3_render_stack_hi = ~(uintptr_t)0;
        return;
    }

    for (i = 0; i < 16 && logged[i]; i++)
        if (logged[i] == func)
            break;
    if (i < 16 && !logged[i]) {
        logged[i] = func;
        PS3_Logf("[smp] %s from the game thread while the back end runs: waiting", func);
    }
    GLimp_FrontEndSleep();
}
