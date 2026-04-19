#ifndef DEVICE_H
#define DEVICE_H

#include "common.h"

/* MMIO device model.
 *
 * Devices register callbacks over a [lo, hi) address range via
 * mmio_add_map. paddr_read/write check pmem first, then this table.
 * If neither matches, the access panics as out-of-bound.
 *
 * The callback is invoked with an offset relative to the mapping's
 * lo bound, the access length (1, 2, or 4), an is_write flag, and a
 * pointer to the data slot. For reads the callback should write the
 * value there; for writes it should read the value and act on it.
 */

typedef void (*mmio_cb_t)(paddr_t off, int len, bool is_write, word_t *data);

void mmio_add_map(paddr_t lo, paddr_t hi, mmio_cb_t cb, const char *name);

bool mmio_in_range(paddr_t addr);
void mmio_access  (paddr_t addr, int len, bool is_write, word_t *data);

/* Register all stock devices (serial, timer). Call from main.c after
 * cpu_init(). Idempotent. */
void init_devices(void);

#endif /* DEVICE_H */
