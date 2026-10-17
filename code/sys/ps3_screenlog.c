/* ps3_screenlog.c -- on-screen boot log, drawn by the CPU straight into the
 * display buffers.
 *
 * Why this way: when the console freezes, the display controller keeps
 * scanning out whatever is in the front buffer, so text the CPU wrote there
 * stays on the TV even if the GPU or the process is dead. No GPU commands, no
 * flips and no read-back from video memory are used (reads from RSX memory are
 * very slow): only plain 32-bit stores of the foreground pixels.
 *
 * Everything that goes through PS3_BootTracef / PS3_LogRaw / ps3_log lands
 * here. Lines logged before the display exists are kept in a small ring and
 * replayed on attach. When the page fills up it is cleared and drawing starts
 * again at the top (no scrolling: that would need reading video memory).
 *
 * Drawing stops at the first rendered frame (PS3_ScreenLog_Disable), because
 * from then on the GPU owns the buffers.
 */
#include <stdint.h>
#include <string.h>

#include "ps3_screenlog.h"
#include "ps3_screenlog_font.h"

#define SL_RING  300
#define SL_LINE  200
#define SL_MAXFB 4

#define SL_BG      0xff101830u
#define SL_FG      0xffe8e8e8u
#define SL_FG_STEP 0xff80e0ffu
#define SL_FG_BAD  0xffff6060u

static char     sl_ring[SL_RING][SL_LINE];
static unsigned sl_total;                 /* completed lines so far */
static char     sl_cur[SL_LINE];          /* line being assembled */
static int      sl_cur_len;

static uint32_t *sl_fb[SL_MAXFB];
static int  sl_nfb, sl_w, sl_h, sl_stride;   /* stride in pixels */
static int  sl_scale, sl_x0, sl_y0, sl_cols, sl_rows, sl_row;
static int  sl_attached, sl_enabled = 1, sl_busy;

static uint32_t sl_color(const char *s)
{
    if (strstr(s, "FAIL") || strstr(s, "rror") || strstr(s, "WARNING") || strstr(s, "fatal"))
        return SL_FG_BAD;
    if (strstr(s, "STEP:") || strstr(s, "IMG:") || strstr(s, "TEX:") ||
        strstr(s, "RENDERER:") || strstr(s, "BOOT:"))
        return SL_FG_STEP;
    return SL_FG;
}

static void sl_clear_page(void)
{
    int b, i, n = sl_stride * sl_h;

    for (b = 0; b < sl_nfb; b++) {
        uint32_t *p = sl_fb[b];
        for (i = 0; i < n; i++)
            p[i] = SL_BG;
    }
    sl_row = 0;
}

static void sl_draw_char(uint32_t *fb, int px, int py, unsigned char c, uint32_t color)
{
    const unsigned char *g;
    int r, cb, sy, sx;

    if (c < 32 || c > 126)
        c = '?';
    g = ps3sl_font[c - 32];
    for (r = 0; r < 16; r++) {
        unsigned bits = g[r];
        if (!bits)
            continue;
        for (cb = 0; cb < 8; cb++) {
            if (!(bits & (0x80u >> cb)))
                continue;
            for (sy = 0; sy < sl_scale; sy++) {
                uint32_t *row = fb + (py + r * sl_scale + sy) * sl_stride + px + cb * sl_scale;
                for (sx = 0; sx < sl_scale; sx++)
                    row[sx] = color;
            }
        }
    }
}

/* Draws one logical line, wrapping at the right edge. */
static void sl_put_line(const char *s)
{
    uint32_t color = sl_color(s);
    int len = (int)strlen(s);

    while (len > 0) {
        int chunk = len < sl_cols ? len : sl_cols;
        int i, b;

        if (sl_row >= sl_rows)
            sl_clear_page();
        for (b = 0; b < sl_nfb; b++)
            for (i = 0; i < chunk; i++)
                sl_draw_char(sl_fb[b], sl_x0 + i * 8 * sl_scale,
                             sl_y0 + sl_row * 16 * sl_scale, (unsigned char)s[i], color);
        sl_row++;
        s += chunk;
        len -= chunk;
    }
}

static void sl_finish_line(void)
{
    if (sl_cur_len == 0)
        return;
    sl_cur[sl_cur_len] = '\0';
    memcpy(sl_ring[sl_total % SL_RING], sl_cur, (size_t)sl_cur_len + 1);
    sl_total++;
    sl_cur_len = 0;
    if (sl_attached && sl_enabled)
        sl_put_line(sl_ring[(sl_total - 1) % SL_RING]);
}

void PS3_ScreenLog_Line(const char *msg)
{
    if (!msg || sl_busy)
        return;
    sl_busy = 1;
    for (; *msg; msg++) {
        unsigned char c = (unsigned char)*msg;

        if (c == '\r')
            continue;
        if (c == '\n') {
            sl_finish_line();
            continue;
        }
        if (c == '\t')
            c = ' ';
        if (c < 32 || c > 126)
            c = '?';
        if (sl_cur_len < SL_LINE - 1)
            sl_cur[sl_cur_len++] = (char)c;
    }
    sl_busy = 0;
}

void PS3_ScreenLog_Attach(uint32_t **buffers, int count, int width, int height, int pitch)
{
    int i, cell_w, cell_h;
    unsigned first;

    if (!buffers || count <= 0 || width < 320 || height < 240 || pitch < width * 4)
        return;
    if (count > SL_MAXFB)
        count = SL_MAXFB;
    for (i = 0; i < count; i++) {
        if (!buffers[i])
            return;
        sl_fb[i] = buffers[i];
    }
    sl_nfb    = count;
    sl_w      = width;
    sl_h      = height;
    sl_stride = pitch / 4;
    sl_scale  = (height >= 1000) ? 2 : 1;
    cell_w    = 8 * sl_scale;
    cell_h    = 16 * sl_scale;
    sl_x0     = width / 16;              /* stay inside TV overscan */
    sl_y0     = height / 16;
    sl_cols   = (width - 2 * sl_x0) / cell_w;
    sl_rows   = (height - 2 * sl_y0) / cell_h;

    sl_busy = 1;
    sl_clear_page();
    sl_attached = 1;

    /* replay the most recent lines that fit on one page */
    first = 0;
    if (sl_total > (unsigned)(sl_rows - 1))
        first = sl_total - (unsigned)(sl_rows - 1);
    if (sl_total > SL_RING && first < sl_total - SL_RING)
        first = sl_total - SL_RING;
    for (; first < sl_total; first++)
        sl_put_line(sl_ring[first % SL_RING]);
    sl_busy = 0;
}

void PS3_ScreenLog_Disable(void)
{
    sl_enabled = 0;
}
