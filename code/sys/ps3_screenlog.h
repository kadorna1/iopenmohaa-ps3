/* ps3_screenlog.h -- on-screen boot log (see ps3_screenlog.c). */
#ifndef PS3_SCREENLOG_H
#define PS3_SCREENLOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feed text (any number of lines, partial lines allowed). Always safe to call:
 * before the framebuffer exists the text is kept and replayed on attach. */
void PS3_ScreenLog_Line(const char *msg);

/* Called once the display buffers exist. pitch is in bytes. Clears every
 * buffer, replays what was logged so far and keeps drawing from then on. The
 * caller must flip to a buffer afterwards so the text becomes visible. */
void PS3_ScreenLog_Attach(uint32_t **buffers, int count, int width, int height, int pitch);

/* Stop drawing (the GPU is about to render real frames into the buffers). */
void PS3_ScreenLog_Disable(void);

#ifdef __cplusplus
}
#endif
#endif
