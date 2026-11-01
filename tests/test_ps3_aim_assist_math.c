#include <assert.h>
#include <math.h>
#include "../code/qcommon/ps3_aim_assist_math.h"

static void AssertNear(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.001f);
}

int main(void)
{
    /* Wrap correctly across the +/-180 degree boundary. */
    AssertNear(PS3_AimAssistAngleDelta(1.0f, 359.0f), 2.0f);
    AssertNear(PS3_AimAssistAngleDelta(359.0f, 1.0f), -2.0f);
    AssertNear(PS3_AimAssistAngleDelta(-179.0f, 179.0f), 2.0f);
    AssertNear(PS3_AimAssistAngleDelta(181.0f, 0.0f), -179.0f);

    /* Targets centered on the reticle have maximum weight. */
    AssertNear(PS3_AimAssistConeScore(0.0f, 0.0f), 0.0f);
    AssertNear(PS3_AimAssistWeight(0.0f), 1.0f);

    /* Each axis reaches zero weight at its configured cone boundary. */
    AssertNear(PS3_AimAssistConeScore(12.0f, 0.0f), 1.0f);
    AssertNear(PS3_AimAssistConeScore(0.0f, 8.0f), 1.0f);
    AssertNear(PS3_AimAssistWeight(1.0f), 0.0f);

    /* Elliptical cone and smooth radial falloff. */
    AssertNear(PS3_AimAssistConeScore(6.0f, 4.0f), 0.5f);
    AssertNear(PS3_AimAssistWeight(0.25f), 0.5f);
    AssertNear(PS3_AimAssistWeight(0.5f), 1.0f - sqrtf(0.5f));
    AssertNear(PS3_AimAssistWeight(1.5f), 0.0f);

    return 0;
}
