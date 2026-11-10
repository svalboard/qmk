// Scroll test motion (scroll_test.h). Dev builds only.
#include "scroll_test.h"
#include <string.h>
#include "timer.h"

static struct {
    bool     on;
    uint8_t  hand;
    int32_t  rate;  // counts per second
    uint32_t start, last, ms;
    int32_t  rem;   // counts x 1000 not yet sent
    int32_t  total; // counts injected since this run started
} st;

void sval_scroll_test_command(uint8_t *data, uint8_t length) {
    (void)length;
    uint8_t action = data[7];
    if (action == 1) {
        int32_t rate;
        memcpy(&rate, data + 9, 4);
        st.hand  = data[8] & 1;
        st.rate  = rate;
        st.ms    = (uint32_t)(data[13] | (data[14] << 8));
        st.start = st.last = timer_read32();
        st.rem   = 0;
        st.total = 0;
        st.on    = true;
    } else {
        st.on = false;
    }
    int32_t total = st.total;
    memset(data + 6, 0, 26);
    data[6] = 0xC4;
    data[7] = action <= 1 ? 0 : 2;
    memcpy(data + 8, &total, 4);
}

bool sval_scroll_test_forces(uint8_t hand) {
    return st.on && hand == st.hand;
}

void sval_scroll_test_motion(report_mouse_t *left, report_mouse_t *right) {
    if (!st.on) return;
    uint32_t now = timer_read32();
    if (TIMER_DIFF_32(now, st.start) >= st.ms) {
        st.on = false;
        return;
    }
    uint32_t dt = TIMER_DIFF_32(now, st.last);
    if (!dt) return;
    st.last = now;
    st.rem += st.rate * (int32_t)dt;
    int32_t y = st.rem / 1000;
    if (y > 2000) y = 2000;
    if (y < -2000) y = -2000;
    st.rem -= y * 1000;
    st.total += y;
    report_mouse_t *r = st.hand ? right : left;
    r->y += (mouse_xy_report_t)y;
}
