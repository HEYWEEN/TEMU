#ifndef RISCV32_TRAP_H
#define RISCV32_TRAP_H

#include "common.h"

/* Cause codes (Privileged Spec v1.12 Table 3.6). Bit 31 distinguishes
 * interrupts (set) from synchronous exceptions (clear). */
#define CAUSE_BREAKPOINT       3u
#define CAUSE_ECALL_M         11u
#define CAUSE_INT_MTI        (0x80000000u | 7u)

/* Stage a trap. Called from INSTPAT bodies (ecall, ebreak-as-trap)
 * and, in stage 6a-4, from cpu_exec's interrupt poll. Does NOT modify
 * PC or CSRs — that's trap_commit()'s job. Splitting stage and commit
 * keeps INSTPAT bodies pure: they only hand a Decode::dnpc back to
 * cpu_exec, and cpu_exec decides when PC is actually redirected. */
void trap_take(word_t cause, word_t tval, word_t epc);
bool trap_pending(void);

/* Apply the staged trap: update mepc/mcause/mstatus and redirect
 * cpu.pc to mtvec. Called by cpu_exec after each instruction once the
 * ISA-level side effects (GPR writes, dnpc) have settled. */
void trap_commit(void);

/* Executed by the mret instruction body. Writes the return PC through
 * *dnpc (so the pattern "dnpc is the only PC channel" stays intact)
 * and restores mstatus.MIE from MPIE. */
void trap_mret(word_t *dnpc);

#endif /* RISCV32_TRAP_H */
