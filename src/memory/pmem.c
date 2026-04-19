#include "common.h"
#include "device.h"
#include "memory.h"

/* 128 MB of guest physical DRAM, lived in the host binary's .bss. The
 * static lifetime means there is never a "before allocation" state —
 * every stage above this one can assume pmem is ready to touch. */
static uint8_t pmem[PMEM_SIZE];

bool paddr_touched_mmio = false;

static inline uint8_t *guest_to_host(paddr_t addr) {
    return pmem + (addr - PMEM_BASE);
}

bool in_pmem(paddr_t addr) {
    return addr >= PMEM_BASE && addr < PMEM_BASE + PMEM_SIZE;
}

static word_t pmem_read(paddr_t addr, int len) {
    word_t ret = 0;
    memcpy(&ret, guest_to_host(addr), (size_t)len);
    return ret;
}

static void pmem_write(paddr_t addr, int len, word_t data) {
    memcpy(guest_to_host(addr), &data, (size_t)len);
}

word_t paddr_read(paddr_t addr, int len) {
    Assert(len == 1 || len == 2 || len == 4,
           "paddr_read: bad length %d", len);

    if (in_pmem(addr) && in_pmem(addr + (paddr_t)len - 1)) {
        return pmem_read(addr, len);
    }
    if (mmio_in_range(addr)) {
        word_t data = 0;
        mmio_access(addr, len, false, &data);
        paddr_touched_mmio = true;
        return data;
    }
    panic("paddr_read out of bound: addr=0x%08" PRIx32 " len=%d", addr, len);
}

void paddr_write(paddr_t addr, int len, word_t data) {
    Assert(len == 1 || len == 2 || len == 4,
           "paddr_write: bad length %d", len);

    if (in_pmem(addr) && in_pmem(addr + (paddr_t)len - 1)) {
        pmem_write(addr, len, data);
        return;
    }
    if (mmio_in_range(addr)) {
        mmio_access(addr, len, true, &data);
        paddr_touched_mmio = true;
        return;
    }
    panic("paddr_write out of bound: addr=0x%08" PRIx32 " len=%d", addr, len);
}

long load_img(const char *path) {
    Assert(path != NULL, "load_img: NULL path");

    FILE *fp = fopen(path, "rb");
    Assert(fp != NULL, "cannot open image '%s'", path);

    Assert(fseek(fp, 0, SEEK_END) == 0, "fseek failed on '%s'", path);
    long size = ftell(fp);
    Assert(size >= 0, "ftell failed on '%s'", path);
    Assert((size_t)size <= PMEM_SIZE,
           "image too large: %ld bytes > pmem %d bytes", size, PMEM_SIZE);
    Assert(fseek(fp, 0, SEEK_SET) == 0, "fseek failed on '%s'", path);

    size_t got = fread(guest_to_host(RESET_VECTOR), 1, (size_t)size, fp);
    Assert(got == (size_t)size,
           "short read on '%s': got %zu of %ld", path, got, size);

    fclose(fp);
    Log("loaded %ld bytes from '%s' at 0x%08" PRIx32,
        size, path, (paddr_t)RESET_VECTOR);
    return size;
}
