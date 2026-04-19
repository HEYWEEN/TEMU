#include "memory.h"

/* Stage 1 stub. Returns 0 silently so that expressions containing *EXPR
 * still evaluate. Stage 2 replaces this with the real pmem[] array. */
word_t paddr_read(paddr_t addr, int len) {
    (void)addr;
    (void)len;
    return 0;
}

void paddr_write(paddr_t addr, int len, word_t data) {
    (void)addr;
    (void)len;
    (void)data;
}
