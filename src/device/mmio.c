#include "common.h"
#include "device.h"

#define MAX_MMIO 8

typedef struct {
    paddr_t     lo;
    paddr_t     hi;
    mmio_cb_t   cb;
    const char *name;
} mmio_map_t;

static mmio_map_t map[MAX_MMIO];
static int        nr_map = 0;

void mmio_add_map(paddr_t lo, paddr_t hi, mmio_cb_t cb, const char *name) {
    Assert(nr_map < MAX_MMIO, "too many MMIO mappings (%d)", MAX_MMIO);
    Assert(lo < hi,           "bad MMIO range: 0x%08" PRIx32 "..0x%08" PRIx32,
           lo, hi);
    for (int i = 0; i < nr_map; i++) {
        Assert(!(lo < map[i].hi && hi > map[i].lo),
               "MMIO region '%s' overlaps with '%s'", name, map[i].name);
    }
    map[nr_map++] = (mmio_map_t){ lo, hi, cb, name };
    Log("mmio: '%s' -> [0x%08" PRIx32 ", 0x%08" PRIx32 ")", name, lo, hi);
}

bool mmio_in_range(paddr_t addr) {
    for (int i = 0; i < nr_map; i++) {
        if (addr >= map[i].lo && addr < map[i].hi) return true;
    }
    return false;
}

void mmio_access(paddr_t addr, int len, bool is_write, word_t *data) {
    for (int i = 0; i < nr_map; i++) {
        if (addr >= map[i].lo && addr < map[i].hi) {
            Assert(addr + (paddr_t)len <= map[i].hi,
                   "MMIO access at 0x%08" PRIx32 " len=%d overruns '%s'",
                   addr, len, map[i].name);
            map[i].cb(addr - map[i].lo, len, is_write, data);
            return;
        }
    }
    panic("mmio_access: no mapping for 0x%08" PRIx32, addr);
}

/* Device init hooks. Each device implements its own init_<name>() in
 * src/device/<name>.c and is listed here. */
void init_serial(void);

void init_devices(void) {
    init_serial();
}
