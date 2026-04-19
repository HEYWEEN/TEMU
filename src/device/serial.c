#include "common.h"
#include "device.h"

/* Minimal UART: one byte-wide output register. Writes drop the low 8
 * bits onto stdout and flush immediately; reads return zero (no input
 * path is modelled yet). The region is 8 bytes wide so `sw` from a
 * program to this address works in "forgiving" mode — we just take
 * the low byte regardless of access length.
 *
 * Address 0xa00003f8 matches the classical UART 16550 COM1 offset,
 * picked because it sits clearly outside pmem ([0x80000000, 0x88000000))
 * and any future devices. */

#define SERIAL_BASE 0xa00003f8u
#define SERIAL_SIZE 8

static void serial_cb(paddr_t off, int len, bool is_write, word_t *data) {
    (void)off;
    (void)len;
    if (is_write) {
        putchar((int)(*data & 0xff));
        fflush(stdout);
    } else {
        *data = 0;
    }
}

void init_serial(void) {
    mmio_add_map(SERIAL_BASE, SERIAL_BASE + SERIAL_SIZE, serial_cb, "serial");
}
