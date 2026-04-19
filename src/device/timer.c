#include <sys/time.h>

#include "common.h"
#include "device.h"

/* 64-bit monotonic microsecond counter, read-only. Two 32-bit words:
 * offset 0 returns the low half, offset 4 the high half. The counter
 * starts at zero when init_timer() is called and advances with real
 * wall time.
 *
 * Wall-clock time (rather than instruction count) is the pragmatic
 * choice: programs that want to sleep for N microseconds can poll the
 * timer and compare. The trade-off is that the reported rate depends
 * on how fast the emulator runs; a slower run reports the same real
 * time as a faster run. For scheduling demos in Stage 6 this is fine;
 * an ISA-accurate rate would need to count executed instructions
 * instead, which we can revisit then. */

#define TIMER_BASE 0xa0000048u
#define TIMER_SIZE 8

static uint64_t boot_time_us;

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static void timer_cb(paddr_t off, int len, bool is_write, word_t *data) {
    (void)len;
    Assert(!is_write, "timer: write to read-only register at offset %u", off);
    uint64_t elapsed = now_us() - boot_time_us;
    if      (off == 0) *data = (word_t)(elapsed & 0xffffffffu);
    else if (off == 4) *data = (word_t)(elapsed >> 32);
    else panic("timer: bad offset %u", off);
}

void init_timer(void) {
    boot_time_us = now_us();
    mmio_add_map(TIMER_BASE, TIMER_BASE + TIMER_SIZE, timer_cb, "timer");
}
