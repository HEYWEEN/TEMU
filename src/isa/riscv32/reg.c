#include "cpu.h"

CPU_state cpu = { .gpr = {0}, .pc = 0 };

static const char *reg_abi[32] = {
    "zero", "ra", "sp",  "gp",  "tp",  "t0", "t1", "t2",
    "s0",   "s1", "a0",  "a1",  "a2",  "a3", "a4", "a5",
    "a6",   "a7", "s2",  "s3",  "s4",  "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3",  "t4", "t5", "t6",
};

const char *reg_name(int idx) {
    Assert(idx >= 0 && idx < 32, "register index %d out of range", idx);
    return reg_abi[idx];
}

/* Accepts: "pc", "x0".."x31", or an ABI alias. "fp" is an alias for s0
 * per the RV32I ABI convention. */
word_t isa_reg_val(const char *name, bool *success) {
    if (strcmp(name, "pc") == 0) {
        *success = true;
        return cpu.pc;
    }

    if (name[0] == 'x') {
        char *end;
        long n = strtol(name + 1, &end, 10);
        if (*end == '\0' && n >= 0 && n < 32) {
            *success = true;
            return cpu.gpr[n];
        }
    }

    if (strcmp(name, "fp") == 0) {
        *success = true;
        return cpu.gpr[8];
    }

    for (int i = 0; i < 32; i++) {
        if (strcmp(name, reg_abi[i]) == 0) {
            *success = true;
            return cpu.gpr[i];
        }
    }

    *success = false;
    return 0;
}
