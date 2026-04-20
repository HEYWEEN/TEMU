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

#define TIMER_BASE    0xa0000048u
#define TIMER_SIZE    8
#define MTIMECMP_BASE 0xa0000050u
#define MTIMECMP_SIZE 8

static uint64_t boot_time_us;

/* Compare register. 64-bit, software-writable. Polled by cpu_exec
 * each instruction; when mtime >= mtimecmp the machine timer
 * interrupt-pending bit (mip.MTIP) is set. Initialised to UINT64_MAX
 * so no interrupt fires until software arms it — boot code must
 * tolerate the CPU starting with mtime at zero. */
static uint64_t mtimecmp;

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

uint64_t timer_mtime(void) {
    return now_us() - boot_time_us;
}

uint64_t timer_mtimecmp(void) {
    return mtimecmp;
}

static void timer_cb(paddr_t off, int len, bool is_write, word_t *data) {
    (void)len;
    Assert(!is_write, "timer: write to read-only register at offset %u", off);
    uint64_t elapsed = timer_mtime();
    if      (off == 0) *data = (word_t)(elapsed & 0xffffffffu);
    else if (off == 4) *data = (word_t)(elapsed >> 32);
    else panic("timer: bad offset %u", off);
}

/* mtimecmp is a normal 64-bit register exposed as two 32-bit words.
 * Ordering note: writing the low word first can momentarily create a
 * stale threshold that trips the interrupt immediately — matches real
 * CLINT behaviour, which is why Linux writes the high word first when
 * disarming. Stage 6a's tests don't care; documenting the hazard for
 * future 6d kernel porters. */
static void mtimecmp_cb(paddr_t off, int len, bool is_write, word_t *data) {
    (void)len;
    if (is_write) {
        if      (off == 0) mtimecmp = (mtimecmp & 0xffffffff00000000ull) | (uint64_t)*data;
        else if (off == 4) mtimecmp = (mtimecmp & 0xffffffffull)         | ((uint64_t)*data << 32);
        else panic("mtimecmp: bad write offset %u", off);
    } else {
        if      (off == 0) *data = (word_t)(mtimecmp & 0xffffffffu);
        else if (off == 4) *data = (word_t)(mtimecmp >> 32);
        else panic("mtimecmp: bad read offset %u", off);
    }
}

void init_timer(void) {
    boot_time_us = now_us();
    mtimecmp     = UINT64_MAX;
    mmio_add_map(TIMER_BASE,    TIMER_BASE    + TIMER_SIZE,    timer_cb,    "timer");
    mmio_add_map(MTIMECMP_BASE, MTIMECMP_BASE + MTIMECMP_SIZE, mtimecmp_cb, "mtimecmp");
}
