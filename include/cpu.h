#ifndef CPU_H
#define CPU_H

#include "common.h"

typedef struct {
    word_t  gpr[32];
    vaddr_t pc;
} CPU_state;

extern CPU_state cpu;

/* Execution status tracked across cpu_exec() boundaries.
 *
 *   RUNNING  — in the middle of cpu_exec's loop
 *   STOP     — cleanly paused (watchpoint hit, step count exhausted)
 *   END      — program hit EBREAK; emulation finished normally
 *   ABORT    — something went wrong (invalid instruction, etc.)
 */
typedef enum {
    TEMU_RUNNING,
    TEMU_STOP,
    TEMU_END,
    TEMU_ABORT,
} temu_state_t;

/* Decoded fetch info passed from cpu_exec into the ISA. Populated for
 * every instruction; isa_exec_once may write to dnpc to redirect PC
 * (branches, jumps). */
typedef struct {
    vaddr_t  pc;    /* address of this instruction                  */
    vaddr_t  snpc;  /* static next PC (pc + 4) — fall-through        */
    vaddr_t  dnpc;  /* dynamic next PC — what cpu_exec writes back   */
    uint32_t inst;  /* 32-bit encoded instruction                   */
} Decode;

/* Zero the register file and set PC to RESET_VECTOR. Call once before
 * entering the REPL or batch-mode execution. */
void cpu_init(void);

/* Execute up to `n` instructions. Pass UINT64_MAX (or -1 cast to
 * uint64_t) to run until halt. Prints a summary on end/abort. */
void cpu_exec(uint64_t n);

/* Queried by main.c to decide the batch-mode exit code, and by sdb.c
 * to decide whether `c`/`si` should proceed. */
temu_state_t temu_state(void);

/* Called from ISA code to signal program termination. */
void temu_set_end(vaddr_t pc, word_t halt_ret);
void temu_set_abort(void);

/* Implemented in src/isa/<arch>/inst.c — decodes and executes one
 * instruction. May update s->dnpc to redirect PC. */
int  isa_exec_once(Decode *s);

const char *reg_name(int idx);

/* Look up a register by symbolic name: "pc", "xN" (0..31), or ABI alias
 * like "zero", "ra", "sp", "a0"... Sets *success and returns the value
 * held by that register. */
word_t isa_reg_val(const char *name, bool *success);

/* Pretty-print all implemented CSRs to stdout. Used by `info c`. */
void csr_dump(void);

/* Look up a CSR by name (e.g. "mstatus", "mepc"). Returns true and
 * fills *out if found. Used by the expression evaluator so that
 * `p $mstatus` works. */
bool csr_lookup(const char *name, word_t *out);

/* EBREAK behaviour mode.
 *
 *   HALT — legacy: ebreak terminates emulation and reports halt_ret
 *          (the pre-6a contract that the existing 65 isa+program
 *          tests depend on). Default.
 *   TRAP — spec-compliant: ebreak raises a BREAKPOINT exception.
 *          Selected via `--ebreak=trap` on the command line. */
typedef enum { EBREAK_HALT, EBREAK_TRAP } ebreak_mode_t;
extern ebreak_mode_t g_ebreak_mode;

#endif /* CPU_H */
