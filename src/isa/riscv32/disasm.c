/*
 * disasm.c — human-readable formatting of a Disasm_info record.
 *
 * The record is populated by isa_exec_once's INSTPAT table on every
 * matched instruction. Here we just turn it into strings. Format
 * choices are deliberately close to riscv-gnu-binutils objdump
 * conventions so output is already familiar:
 *
 *   addi a0, a0, -1
 *   bne  t0, zero, 0x8000_0008
 *   lw   a0, 0(t0)
 *   sw   t1, 12(sp)
 *   lui  a0, 0x12345
 *   jal  ra, 0x8000_0020
 *   ebreak
 */

#include "common.h"
#include "cpu.h"
#include "local-include/inst.h"

static bool name_is(const Disasm_info *d, const char *s) {
    return strcmp(d->name, s) == 0;
}

int disasm(char *buf, size_t n, const Disasm_info *d) {
    const char *rd  = (d->rd  >= 0 && d->rd  < 32) ? reg_name(d->rd ) : "?";
    const char *rs1 = (d->rs1 >= 0 && d->rs1 < 32) ? reg_name(d->rs1) : "?";
    const char *rs2 = (d->rs2 >= 0 && d->rs2 < 32) ? reg_name(d->rs2) : "?";
    int32_t     simm = (int32_t)d->imm;

    /* Loads: "lw rd, imm(rs1)" */
    if (name_is(d, "lb")  || name_is(d, "lh")  || name_is(d, "lw") ||
        name_is(d, "lbu") || name_is(d, "lhu")) {
        return snprintf(buf, n, "%-7s %s, %d(%s)", d->name, rd, simm, rs1);
    }

    /* Stores: "sw rs2, imm(rs1)" — rd field is not a destination. */
    if (name_is(d, "sb") || name_is(d, "sh") || name_is(d, "sw")) {
        return snprintf(buf, n, "%-7s %s, %d(%s)", d->name, rs2, simm, rs1);
    }

    /* I-type shifts: shift amount is the low 5 bits, unsigned. */
    if (name_is(d, "slli") || name_is(d, "srli") || name_is(d, "srai")) {
        return snprintf(buf, n, "%-7s %s, %s, %u",
                        d->name, rd, rs1, (unsigned)(d->imm & 0x1f));
    }

    /* JALR: a regular I-type with the jump target in rs1+imm. */
    if (name_is(d, "jalr")) {
        return snprintf(buf, n, "%-7s %s, %d(%s)", d->name, rd, simm, rs1);
    }

    /* Branches: pc-relative target rendered absolute. */
    if (d->type == TYPE_B) {
        return snprintf(buf, n, "%-7s %s, %s, 0x%08" PRIx32,
                        d->name, rs1, rs2, (uint32_t)(d->pc + d->imm));
    }

    /* JAL: pc-relative target rendered absolute. */
    if (name_is(d, "jal")) {
        return snprintf(buf, n, "%-7s %s, 0x%08" PRIx32,
                        d->name, rd, (uint32_t)(d->pc + d->imm));
    }

    switch (d->type) {
    case TYPE_R:
        return snprintf(buf, n, "%-7s %s, %s, %s",
                        d->name, rd, rs1, rs2);
    case TYPE_I:
        return snprintf(buf, n, "%-7s %s, %s, %d",
                        d->name, rd, rs1, simm);
    case TYPE_U:
        return snprintf(buf, n, "%-7s %s, 0x%x",
                        d->name, rd, (unsigned)(d->imm >> 12));
    case TYPE_N:
        return snprintf(buf, n, "%s", d->name);
    default:
        return snprintf(buf, n, "%-7s <?>", d->name);
    }
}
