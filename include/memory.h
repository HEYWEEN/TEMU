#ifndef MEMORY_H
#define MEMORY_H

#include "common.h"

/* Physical-memory layout. PMEM_BASE is the guest physical address of
 * byte 0 of pmem[]; RESET_VECTOR is where PC is initialized and also
 * where load_img() drops the image. */
#define PMEM_BASE    0x80000000u
#define PMEM_SIZE    (128 * 1024 * 1024)   /* 128 MB */
#define RESET_VECTOR PMEM_BASE

bool in_pmem(paddr_t addr);

/* Load a raw binary image into pmem at RESET_VECTOR. Returns the number
 * of bytes loaded. Panics on open / read error or if the image is
 * larger than PMEM_SIZE. */
long load_img(const char *path);

/* Physical-address read / write. `len` must be 1, 2, or 4. Accesses
 * are routed to pmem[] if the address is in-range, otherwise to the
 * MMIO table registered by src/device/. Out-of-bound + un-mapped
 * accesses panic. */
word_t paddr_read(paddr_t addr, int len);
void   paddr_write(paddr_t addr, int len, word_t data);

/* Set to true by paddr_read/write whenever the access was served by
 * MMIO rather than pmem. Difftest reads + clears this flag after each
 * main-side instruction so the reference CPU can be skipped on
 * side-effecting MMIO steps. */
extern bool paddr_touched_mmio;

#endif /* MEMORY_H */
