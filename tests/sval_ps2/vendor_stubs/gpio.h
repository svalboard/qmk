// Host test stand-ins for what ps2_vendor.c gets through gpio.h (the ChibiOS
// HAL, OSAL and input buffers queues). ps2_vendor_test.c drives them.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCU_RP
#define PS2_ENABLE
#define PS2_PIO_USE_PIO1
#define GP23 23
#define GP24 24
#define PS2_DATA_PIN GP23
#define PS2_CLOCK_PIN GP24

typedef unsigned int uint;
typedef int32_t      msg_t;
typedef uint32_t     iomode_t;
typedef void        *thread_reference_t;
#define MSG_OK 0
#define MSG_TIMEOUT -1
#define MSG_RESET -2
#define TIME_MS2I(ms) (ms)

#define OSAL_IRQ_HANDLER(name) void name(void)
#define OSAL_IRQ_PROLOGUE()
#define OSAL_IRQ_EPILOGUE()
#define CORTEX_MAX_KERNEL_PRIORITY 2
#define RP_PIO0_IRQ_0_NUMBER 7
#define RP_PIO1_IRQ_0_NUMBER 9
#define RESETS_ALLREG_PIO0 1
#define RESETS_ALLREG_PIO1 2
#define PAL_RP_PAD_IE 0
#define PAL_RP_GPIO_OE 0
#define PAL_RP_PAD_SLEWFAST 0
#define PAL_RP_PAD_DRIVE12 0
#define PAL_RP_IOCTRL_OEOVER_DRVINVPERI 0
#define PAL_MODE_ALTERNATE_PIO0 0
#define PAL_MODE_ALTERNATE_PIO1 0

static inline void osalSysLock(void) {}
static inline void osalSysUnlock(void) {}
static inline void osalSysLockFromISR(void) {}
static inline void osalSysUnlockFromISR(void) {}
static inline void osalOsRescheduleS(void) {}
static inline void osalThreadResumeI(thread_reference_t *t, msg_t m) {
    (void)t;
    (void)m;
}
static inline msg_t osalThreadSuspendTimeoutS(thread_reference_t *t, int timeout) {
    (void)t;
    (void)timeout;
    return MSG_OK;
}
static inline void nvicEnableVector(int n, int p) {
    (void)n;
    (void)p;
}
static inline void hal_lld_peripheral_unreset(int m) {
    (void)m;
}
static inline void palSetLineMode(int line, iomode_t mode) {
    (void)line;
    (void)mode;
}

// The clock line, as the test's simulated device drives it.
bool sim_clock_high(void);
#define gpio_get(pin) ((pin) == PS2_CLOCK_PIN ? sim_clock_high() : true)

// An input buffers queue of 4-byte frames (hal_buffers.h), without blocking.
typedef struct {
    uint32_t *frames;
    size_t    n, head, count;
} input_buffers_queue_t;
#define BQ_BUFFER_SIZE(n, size) ((n) * (size))
static inline void ibqObjectInit(input_buffers_queue_t *q, bool suspended, uint8_t *bp, size_t size, size_t n, void *infy, void *link) {
    (void)suspended;
    (void)size;
    (void)infy;
    (void)link;
    q->frames = (uint32_t *)bp;
    q->n      = n;
    q->head = q->count = 0;
}
static inline uint8_t *ibqGetEmptyBufferI(input_buffers_queue_t *q) {
    return q->count == q->n ? NULL : (uint8_t *)&q->frames[(q->head + q->count) % q->n];
}
static inline void ibqPostFullBufferI(input_buffers_queue_t *q, size_t size) {
    (void)size;
    q->count++;
}
static inline bool ibqIsEmptyI(input_buffers_queue_t *q) {
    return q->count == 0;
}
static inline void ibqResetI(input_buffers_queue_t *q) {
    q->head = q->count = 0;
}
static inline msg_t ibqReadTimeout(input_buffers_queue_t *q, uint8_t *bp, size_t n, int timeout) {
    (void)n;
    (void)timeout;
    if (q->count == 0) return MSG_TIMEOUT;
    *(uint32_t *)bp = q->frames[q->head];
    q->head         = (q->head + 1) % q->n;
    q->count--;
    return MSG_OK;
}
