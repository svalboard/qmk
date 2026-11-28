// sval_hand_scroll and sval_scroll_hand_settled (keyboards/svalboard/scroll_speed.c).
// F29: a hand in Scroll Mode keeps the two-finger scroll its pad already put
// in h and v when another hand's motion runs the conversion.
// F26: the axis lock's mm units do not overflow for a large boosted report.
// F31: the Scroll speed migration never settles a hand no half reports.
#include <stdio.h>
#include "scroll_speed.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

int main(void) {
    int16_t h, v;
    int32_t mh, mv;

    // ---- a pad's two-finger scroll, no pointer motion on that hand: kept as it was
    {
        sval_ball_scroll_t s = {0};
        sval_hand_scroll(&s, 0, 0, 40, -120, 1210, 1138, 100, false, &h, &v, &mh, &mv);
        CHECK(h == 40 && v == -120); // assigning the ball's 0 and 0 dropped them
        CHECK(mh == 0 && mv == 0);
    }
    // ---- a trackball hand's motion as before: the same as sval_ball_scroll
    {
        sval_ball_scroll_t s = {0}, ref = {0};
        int16_t            rh, rv;
        for (int i = -300; i <= 300; i += 13) {
            sval_hand_scroll(&s, i, -i / 2, 0, 0, 1600, 1600, 150, i > 0, &h, &v, &mh, &mv);
            sval_ball_scroll(&ref, i, -i / 2, 1600, 1600, 150, i > 0, &rh, &rv);
            CHECK(h == rh && v == rv);
            CHECK(mh == (int32_t)i * 100000 / 1600 && mv == (int32_t)(-i / 2) * 100000 / 1600);
        }
    }
    // ---- both at once add, and the sum saturates
    {
        sval_ball_scroll_t s = {0};
        sval_hand_scroll(&s, 5, -7, 10, 20, 800, 800, 100, false, &h, &v, &mh, &mv);
        CHECK(h == 15 && v == 13);
        sval_hand_scroll(&s, 0, 0, 32767, -32768, 800, 800, 100, false, &h, &v, &mh, &mv);
        CHECK(h == 32767 && v == -32768);
        sval_hand_scroll(&s, 100, -100, 32767, -32768, 800, 800, 100, false, &h, &v, &mh, &mv);
        CHECK(h == 32767 && v == -32768);
    }
    // ---- a boosted report above 21474 counts: the mm units keep the sign and size
    {
        sval_ball_scroll_t s = {0};
        sval_hand_scroll(&s, 30000, -32768, 0, 0, 800, 800, 100, false, &h, &v, &mh, &mv);
        CHECK(mh == 3750000 && mv == -4096000); // (int32)x * 100000 wrapped
        sval_hand_scroll(&s, 32767, 32767, 0, 0, 200, 200, 100, false, &h, &v, &mh, &mv);
        CHECK(mh == 16383500 && mv == 16383500);
    }
    // ---- the migration's settle
    for (int local = 0; local < 2; local++)
        for (int searched = 0; searched < 2; searched++)
            for (int passed = 0; passed < 2; passed++) {
                // Unseen: the other half's hand on the half without USB, or before the link is up.
                CHECK(!sval_scroll_hand_settled(SVAL_SCROLL_MODULE_UNSEEN, local, searched, passed));
                CHECK(sval_scroll_hand_settled(SVAL_SCROLL_MODULE_ANSWERED, local, searched, passed));
                CHECK(sval_scroll_hand_settled(SVAL_SCROLL_MODULE_UNCHECKED, local, searched, passed) == (bool)passed);
                CHECK(sval_scroll_hand_settled(SVAL_SCROLL_MODULE_NO_ANSWER, local, searched, passed) == (bool)(local ? searched : passed));
            }
    if (failures) return 1;
    printf("hand_scroll OK\n");
    return 0;
}
