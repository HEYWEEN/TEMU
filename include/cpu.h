#ifndef CPU_H
#define CPU_H

#include "common.h"

typedef struct {
    word_t  gpr[32];
    vaddr_t pc;
} CPU_state;

extern CPU_state cpu;

const char *reg_name(int idx);

/* Look up a register by symbolic name: "pc", "xN" (0..31), or ABI alias
 * like "zero", "ra", "sp", "a0"... Sets *success and returns the value
 * held by that register (in Stage 1 everything is zero). */
word_t isa_reg_val(const char *name, bool *success);

#endif /* CPU_H */
