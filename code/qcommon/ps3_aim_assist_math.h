#ifndef PS3_AIM_ASSIST_MATH_H
#define PS3_AIM_ASSIST_MATH_H

#include <math.h>

/* Half-angle limits for the controller-only target acquisition cone. */
#define PS3_AIM_ASSIST_YAW_CONE_DEGREES    12.0f
#define PS3_AIM_ASSIST_PITCH_CONE_DEGREES   8.0f

static float PS3_AimAssistAngleDelta(float target, float current)
{
    float delta = target - current;

    if (!isfinite(delta)) {
        return 0.0f;
    }

    while (delta > 180.0f) {
        delta -= 360.0f;
    }
    while (delta < -180.0f) {
        delta += 360.0f;
    }

    return delta;
}

static float PS3_AimAssistConeScore(float yawDelta, float pitchDelta)
{
    const float yaw = yawDelta / PS3_AIM_ASSIST_YAW_CONE_DEGREES;
    const float pitch = pitchDelta / PS3_AIM_ASSIST_PITCH_CONE_DEGREES;

    if (!isfinite(yaw) || !isfinite(pitch)) {
        return 2.0f;
    }

    return yaw * yaw + pitch * pitch;
}

/* Smooth radial falloff; zero at and outside the cone boundary. */
static float PS3_AimAssistWeight(float score)
{
    if (!isfinite(score) || score < 0.0f || score >= 1.0f) {
        return 0.0f;
    }

    return 1.0f - sqrtf(score);
}

#endif /* PS3_AIM_ASSIST_MATH_H */
