#ifndef MEMORY_H
#define MEMORY_H

#include "common.h"

/* Physical-address read/write. Stage 1 provides stubs that return 0 /
 * do nothing; Stage 2 wires these up to the real pmem[] array. */
word_t paddr_read(paddr_t addr, int len);
void   paddr_write(paddr_t addr, int len, word_t data);

#endif /* MEMORY_H */
