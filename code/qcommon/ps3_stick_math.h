#ifndef PS3_STICK_MATH_H
#define PS3_STICK_MATH_H

#include <math.h>

/* Convert a centered DS3 stick pair to the engine's signed 16-bit axis range.
 * A radial deadzone keeps diagonals consistent; rescaling the remaining range
 * avoids the large jump caused by simply zeroing values below a threshold. */
static void PS3_NormalizeStick(int raw_x, int raw_y, int deadzone,
                              int *out_x, int *out_y)
{
    const float axis_limit = 127.0f;
    float radius;
    float clamped_radius;
    float output_magnitude;
    float scale;

    if (!out_x || !out_y) {
        return;
    }

    *out_x = 0;
    *out_y = 0;

    if (deadzone < 0) {
        deadzone = 0;
    } else if (deadzone > 64) {
        deadzone = 64;
    }

    radius = sqrtf((float)raw_x * (float)raw_x +
                   (float)raw_y * (float)raw_y);
    if (radius <= (float)deadzone || radius <= 0.0f) {
        return;
    }

    /* A diagonal can have a larger Euclidean magnitude than either raw axis.
     * Saturate at the calibrated cardinal maximum while preserving direction. */
    clamped_radius = radius > axis_limit ? axis_limit : radius;
    output_magnitude = (clamped_radius - (float)deadzone) /
                       (axis_limit - (float)deadzone);
    if (output_magnitude > 1.0f) {
        output_magnitude = 1.0f;
    }
    scale = output_magnitude * 32767.0f / radius;

    *out_x = (int)((float)raw_x * scale);
    *out_y = (int)((float)raw_y * scale);
}

#endif /* PS3_STICK_MATH_H */
