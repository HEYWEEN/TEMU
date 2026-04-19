#ifndef DIFFTEST_H
#define DIFFTEST_H

#include "common.h"
#include "cpu.h"

/* Differential testing. Second RV32I implementation runs in lock-step
 * with the main one; any divergence in GPR / PC state triggers an
 * abort so bugs are caught at the first offending instruction instead
 * of propagating into confusing downstream failures. */

void difftest_enable(bool on);
bool difftest_is_enabled(void);

/* Initialize the reference CPU state from the main CPU. Call once
 * after cpu_init(), before cpu_exec(). */
void difftest_init(void);

/* Called by cpu_exec after each main-side instruction. Runs one
 * instruction on the reference CPU and compares state. On divergence,
 * prints a diff and calls temu_set_abort(). */
void difftest_step(void);

#endif /* DIFFTEST_H */
