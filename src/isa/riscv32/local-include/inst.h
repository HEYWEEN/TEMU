#ifndef RISCV32_INST_H
#define RISCV32_INST_H

#include "common.h"
#include "cpu.h"

/* ------------------------------------------------------------------ */
/* Bit-field helpers                                                   */
/* ------------------------------------------------------------------ */

/* Extract inst[hi:lo] as an unsigned 32-bit value. Caller must keep
 * (hi - lo + 1) < 32 to avoid UB in the mask computation. */
#define BITS(x, hi, lo) \
    (((uint32_t)(x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

/* Sign-extend a value whose sign bit is at position (n-1). Uses the
 * well-known "left-shift then arithmetic-right-shift" trick — signed
 * right shift is implementation-defined in C, but both clang and gcc
 * emit arithmetic shifts on every target we care about. */
#define SEXT(x, n) \
    ((word_t)((int32_t)((uint32_t)(x) << (32 - (n))) >> (32 - (n))))

/* Immediate decoders for each RV32I encoding type. The bit layout here
 * is the source of endless off-by-one bugs in hand-written simulators;
 * spell each one out explicitly instead of trying to be clever. */
#define immI(i) SEXT(BITS(i, 31, 20), 12)
#define immS(i) SEXT((BITS(i, 31, 25) << 5) | BITS(i, 11, 7), 12)
#define immB(i) SEXT((BITS(i, 31, 31) << 12) | (BITS(i, 7,  7) << 11) | \
                     (BITS(i, 30, 25) << 5)  | (BITS(i, 11, 8) << 1),  13)
#define immU(i) ((word_t)(BITS(i, 31, 12) << 12))
#define immJ(i) SEXT((BITS(i, 31, 31) << 20) | (BITS(i, 19, 12) << 12) | \
                     (BITS(i, 20, 20) << 11) | (BITS(i, 30, 21) << 1),  21)

/* Shorthand for reading / writing a GPR by index. rs/rd fields are
 * always 5 bits so n is in [0, 31]; R(0) reads zero and writes are
 * intentionally allowed here — cpu_exec's loop clears gpr[0] after
 * every instruction. */
#define R(n) (cpu.gpr[n])

/* ------------------------------------------------------------------ */
/* Operand decoding                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    TYPE_R, TYPE_I, TYPE_S, TYPE_B, TYPE_U, TYPE_J,
    TYPE_N,    /* no operands (ecall / ebreak / fence) */
} operand_type_t;

void decode_operand(Decode *s, operand_type_t type,
                    int *rd, word_t *src1, word_t *src2, word_t *imm);

bool pattern_match(uint32_t inst, const char *pat);

void invalid_inst(Decode *s);

/* Snapshot of the most recently decoded instruction. Populated by
 * isa_exec_once on every matched INSTPAT; consumed by the disassembler
 * (src/isa/riscv32/disasm.c) to format human-readable output for
 * itrace dumps and difftest divergence reports. */
typedef struct {
    const char    *name;      /* "addi", "bne", ... ; "invalid" on miss */
    operand_type_t type;
    uint32_t       inst;
    vaddr_t        pc;
    int            rd;
    int            rs1;
    int            rs2;
    word_t         imm;
} Disasm_info;

extern Disasm_info g_last_disasm;

/* Format a Disasm_info record into buf; returns the number of chars
 * written (excluding NUL). Caller typically passes &g_last_disasm
 * which was filled by the most recent isa_exec_once. */
int  disasm(char *buf, size_t n, const Disasm_info *d);

#endif /* RISCV32_INST_H */
