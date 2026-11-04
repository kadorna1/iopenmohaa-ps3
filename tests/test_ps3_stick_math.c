#include <assert.h>
#include <stdio.h>

#include "../code/qcommon/ps3_stick_math.h"

static void test_deadzone_and_rescaling(void)
{
    int x, y;

    PS3_NormalizeStick(0, 0, 12, &x, &y);
    assert(x == 0 && y == 0);

    PS3_NormalizeStick(11, 0, 12, &x, &y);
    assert(x == 0 && y == 0);

    /* The first value outside the deadzone must be a small analog value,
     * rather than the old 30-to-31 jump to roughly one quarter scale. */
    PS3_NormalizeStick(13, 0, 12, &x, &y);
    assert(x > 0 && x < 1000 && y == 0);

    PS3_NormalizeStick(64, 0, 12, &x, &y);
    assert(x > 14000 && x < 16000 && y == 0);

    PS3_NormalizeStick(127, 0, 12, &x, &y);
    assert(x == 32767 && y == 0);

    PS3_NormalizeStick(-128, 0, 12, &x, &y);
    assert(x == -32767 && y == 0);
}

static void test_radial_direction_and_saturation(void)
{
    int x, y;
    int x2, y2;

    PS3_NormalizeStick(64, 64, 12, &x, &y);
    assert(x > 0 && y > 0 && x == y);
    assert(x < 32767 && y < 32767);

    PS3_NormalizeStick(127, 127, 12, &x, &y);
    assert(x == y);
    assert(x > 23000 && x < 24000);

    PS3_NormalizeStick(-90, 90, 12, &x, &y);
    PS3_NormalizeStick(90, -90, 12, &x2, &y2);
    assert(x == -x2 && y == -y2);

    /* Deadzone is configurable but clamped to a safe, non-degenerate range. */
    PS3_NormalizeStick(19, 0, 20, &x, &y);
    assert(x == 0 && y == 0);
    PS3_NormalizeStick(21, 0, 20, &x, &y);
    assert(x > 0 && x < 1000);
    PS3_NormalizeStick(127, 0, 500, &x, &y);
    assert(x == 32767 && y == 0);
}

int main(void)
{
    test_deadzone_and_rescaling();
    test_radial_direction_and_saturation();
    puts("PS3 right-stick analog math tests passed");
    return 0;
}
