#include "common.h"
#include "cpu.h"
#include "memory.h"
#include "local-include/inst.h"
#include "local-include/csr.h"

/* ------------------------------------------------------------------ */
/* Runtime pattern matcher                                             */
/* ------------------------------------------------------------------ */

/* Scan a bit-pattern string like "0000000 ????? ????? 000 ????? 0110011"
 * from MSB to LSB. '0' / '1' are mandatory; '?' is a don't-care. Spaces
 * and tabs are ignored. Returns true if `inst` matches. */
bool pattern_match(uint32_t inst, const char *pat) {
    uint32_t key = 0, mask = 0;
    int bit = 31;
    for (const char *p = pat; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t') continue;
        Assert(bit >= 0, "pattern too long: '%s'", pat);
        if (c == '?') {
            /* don't care */
        } else if (c == '0') {
            mask |= (1u << bit);
        } else if (c == '1') {
            mask |= (1u << bit);
            key  |= (1u << bit);
        } else {
            panic("bad character '%c' in pattern '%s'", c, pat);
        }
        bit--;
    }
    Assert(bit == -1, "pattern has %d bits, expected 32: '%s'", 31 - bit, pat);
    return (inst & mask) == key;
}

/* ------------------------------------------------------------------ */
/* Operand decoding                                                    */
/* ------------------------------------------------------------------ */

void decode_operand(Decode *s, operand_type_t type,
                    int *rd, word_t *src1, word_t *src2, word_t *imm) {
    uint32_t i = s->inst;
    int rs1 = (int)BITS(i, 19, 15);
    int rs2 = (int)BITS(i, 24, 20);
    *rd   = (int)BITS(i, 11, 7);
    *src1 = 0; *src2 = 0; *imm = 0;

    switch (type) {
        case TYPE_R: *src1 = R(rs1); *src2 = R(rs2);                   break;
        case TYPE_I: *src1 = R(rs1); *imm  = immI(i);                  break;
        case TYPE_S: *src1 = R(rs1); *src2 = R(rs2); *imm = immS(i);   break;
        case TYPE_B: *src1 = R(rs1); *src2 = R(rs2); *imm = immB(i);   break;
        case TYPE_U: *imm  = immU(i);                                  break;
        case TYPE_J: *imm  = immJ(i);                                  break;
        case TYPE_N:                                                   break;
    }
}

/* ------------------------------------------------------------------ */
/* Invalid-instruction fallback                                        */
/* ------------------------------------------------------------------ */

void invalid_inst(Decode *s) {
    fprintf(stderr,
            "\33[1;31minvalid instruction\33[0m at pc=0x%08" PRIx32
            " inst=0x%08" PRIx32 "\n",
            s->pc, s->inst);
    temu_set_abort();
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                      */
/* ------------------------------------------------------------------ */

Disasm_info g_last_disasm = { .name = "invalid" };

int isa_exec_once(Decode *s) {
    uint32_t inst = s->inst;
    int    rd   = 0;
    word_t src1 = 0, src2 = 0, imm = 0;
    bool   matched = false;

    /* The table is scanned top-to-bottom; the first match wins. More
     * specific patterns (e.g. SUB with funct7 = 0100000) must come
     * before any superset that would also match. Each INSTPAT also
     * records a human-readable mnemonic into g_last_disasm so the
     * disassembler can format itrace + difftest reports without
     * re-running the dispatch. */
    /* Note: use `ty` rather than `type` for the macro parameter —
     * `type` is also a struct-field name on g_last_disasm, and the
     * preprocessor would happily substitute it there too. */
    #define INSTPAT(pat, mnem, ty, body)                                     \
        if (!matched && pattern_match(inst, pat)) {                          \
            decode_operand(s, TYPE_##ty, &rd, &src1, &src2, &imm);           \
            g_last_disasm.name = (mnem);                                     \
            g_last_disasm.type = TYPE_##ty;                                  \
            g_last_disasm.inst = inst;                                       \
            g_last_disasm.pc   = s->pc;                                      \
            g_last_disasm.rd   = rd;                                         \
            g_last_disasm.rs1  = (int)BITS(inst, 19, 15);                    \
            g_last_disasm.rs2  = (int)BITS(inst, 24, 20);                    \
            g_last_disasm.imm  = imm;                                        \
            body;                                                            \
            matched = true;                                                  \
        }

    /* --- upper-immediate ------------------------------------------- */
    INSTPAT("??????? ????? ????? ??? ????? 0110111", "lui",   U,
            R(rd) = imm);
    INSTPAT("??????? ????? ????? ??? ????? 0010111", "auipc", U,
            R(rd) = s->pc + imm);

    /* --- I-type arithmetic ----------------------------------------- */
    INSTPAT("??????? ????? ????? 000 ????? 0010011", "addi",  I,
            R(rd) = src1 + imm);
    INSTPAT("??????? ????? ????? 010 ????? 0010011", "slti",  I,
            R(rd) = (sword_t)src1 <  (sword_t)imm ? 1 : 0);
    INSTPAT("??????? ????? ????? 011 ????? 0010011", "sltiu", I,
            R(rd) = src1 < imm ? 1 : 0);
    INSTPAT("??????? ????? ????? 100 ????? 0010011", "xori",  I,
            R(rd) = src1 ^ imm);
    INSTPAT("??????? ????? ????? 110 ????? 0010011", "ori",   I,
            R(rd) = src1 | imm);
    INSTPAT("??????? ????? ????? 111 ????? 0010011", "andi",  I,
            R(rd) = src1 & imm);

    /* I-type shifts — shamt is imm[4:0]; funct7 distinguishes the
     * three variants. SRAI uses arithmetic right shift, relying on
     * signed-shift behaviour that is implementation-defined in C
     * but arithmetic on every compiler / target we ship on. */
    INSTPAT("0000000 ????? ????? 001 ????? 0010011", "slli", I,
            R(rd) = src1 << BITS(imm, 4, 0));
    INSTPAT("0000000 ????? ????? 101 ????? 0010011", "srli", I,
            R(rd) = src1 >> BITS(imm, 4, 0));
    INSTPAT("0100000 ????? ????? 101 ????? 0010011", "srai", I,
            R(rd) = (word_t)((sword_t)src1 >> BITS(imm, 4, 0)));

    /* --- R-type arithmetic ----------------------------------------- */
    INSTPAT("0000000 ????? ????? 000 ????? 0110011", "add",  R,
            R(rd) = src1 + src2);
    INSTPAT("0100000 ????? ????? 000 ????? 0110011", "sub",  R,
            R(rd) = src1 - src2);
    INSTPAT("0000000 ????? ????? 001 ????? 0110011", "sll",  R,
            R(rd) = src1 << (src2 & 31));
    INSTPAT("0000000 ????? ????? 010 ????? 0110011", "slt",  R,
            R(rd) = (sword_t)src1 <  (sword_t)src2 ? 1 : 0);
    INSTPAT("0000000 ????? ????? 011 ????? 0110011", "sltu", R,
            R(rd) = src1 < src2 ? 1 : 0);
    INSTPAT("0000000 ????? ????? 100 ????? 0110011", "xor",  R,
            R(rd) = src1 ^ src2);
    INSTPAT("0000000 ????? ????? 101 ????? 0110011", "srl",  R,
            R(rd) = src1 >> (src2 & 31));
    INSTPAT("0100000 ????? ????? 101 ????? 0110011", "sra",  R,
            R(rd) = (word_t)((sword_t)src1 >> (src2 & 31)));
    INSTPAT("0000000 ????? ????? 110 ????? 0110011", "or",   R,
            R(rd) = src1 | src2);
    INSTPAT("0000000 ????? ????? 111 ????? 0110011", "and",  R,
            R(rd) = src1 & src2);

    /* --- branches (B-type) ----------------------------------------- *
     * dnpc defaults to snpc; only overwrite when the condition holds. */
    INSTPAT("??????? ????? ????? 000 ????? 1100011", "beq",  B,
            if (src1 == src2) s->dnpc = s->pc + imm);
    INSTPAT("??????? ????? ????? 001 ????? 1100011", "bne",  B,
            if (src1 != src2) s->dnpc = s->pc + imm);
    INSTPAT("??????? ????? ????? 100 ????? 1100011", "blt",  B,
            if ((sword_t)src1 <  (sword_t)src2) s->dnpc = s->pc + imm);
    INSTPAT("??????? ????? ????? 101 ????? 1100011", "bge",  B,
            if ((sword_t)src1 >= (sword_t)src2) s->dnpc = s->pc + imm);
    INSTPAT("??????? ????? ????? 110 ????? 1100011", "bltu", B,
            if (src1 <  src2) s->dnpc = s->pc + imm);
    INSTPAT("??????? ????? ????? 111 ????? 1100011", "bgeu", B,
            if (src1 >= src2) s->dnpc = s->pc + imm);

    /* --- loads (sign-/zero-extended) ------------------------------- */
    INSTPAT("??????? ????? ????? 000 ????? 0000011", "lb",  I,
            R(rd) = SEXT(paddr_read(src1 + imm, 1),  8));
    INSTPAT("??????? ????? ????? 001 ????? 0000011", "lh",  I,
            R(rd) = SEXT(paddr_read(src1 + imm, 2), 16));
    INSTPAT("??????? ????? ????? 010 ????? 0000011", "lw",  I,
            R(rd) = paddr_read(src1 + imm, 4));
    INSTPAT("??????? ????? ????? 100 ????? 0000011", "lbu", I,
            R(rd) = paddr_read(src1 + imm, 1));
    INSTPAT("??????? ????? ????? 101 ????? 0000011", "lhu", I,
            R(rd) = paddr_read(src1 + imm, 2));

    /* --- stores ---------------------------------------------------- */
    INSTPAT("??????? ????? ????? 000 ????? 0100011", "sb", S,
            paddr_write(src1 + imm, 1, src2));
    INSTPAT("??????? ????? ????? 001 ????? 0100011", "sh", S,
            paddr_write(src1 + imm, 2, src2));
    INSTPAT("??????? ????? ????? 010 ????? 0100011", "sw", S,
            paddr_write(src1 + imm, 4, src2));

    /* --- FENCE / FENCE.I — NOP in our sequentially-consistent model. */
    INSTPAT("??????? ????? ????? 000 ????? 0001111", "fence",   I,
            (void)0);
    INSTPAT("??????? ????? ????? 001 ????? 0001111", "fence.i", I,
            (void)0);

    /* --- jumps ----------------------------------------------------- */
    INSTPAT("??????? ????? ????? ??? ????? 1101111", "jal",  J, {
        word_t link = s->snpc;
        s->dnpc = s->pc + imm;
        R(rd) = link;
    });
    INSTPAT("??????? ????? ????? 000 ????? 1100111", "jalr", I, {
        word_t link = s->snpc;
        s->dnpc = (src1 + imm) & ~(word_t)1;
        R(rd) = link;
    });

    /* --- Zicsr (CSR read-modify-write; opcode 0x73, funct3 != 0) --- *
     * Pull the CSR number straight out of inst[31:20]; TYPE_I decoded
     * `src1` already holds R(rs1). For the immediate variants we reuse
     * the rs1 field as a 5-bit zero-extended literal.
     *
     * Subtle rule from the Privileged Spec: CSRRS/CSRRC with rs1=x0
     * (zimm=0 for the immediate variants) must not write the CSR at
     * all — our fields have no side effects yet, but encoding the rule
     * now avoids surprise later. CSRRW always writes.
     *
     * Save the original CSR value before writing so that aliasing like
     * `csrrw t0, mscratch, t0` returns the pre-write value in rd. */
    INSTPAT("??????? ????? ????? 001 ????? 1110011", "csrrw",  I, {
        uint32_t addr = (uint32_t)BITS(inst, 31, 20);
        word_t   old  = csr_read(addr);
        csr_write(addr, src1);
        R(rd) = old;
    });
    INSTPAT("??????? ????? ????? 010 ????? 1110011", "csrrs",  I, {
        uint32_t addr = (uint32_t)BITS(inst, 31, 20);
        word_t   old  = csr_read(addr);
        if (BITS(inst, 19, 15) != 0) csr_write(addr, old | src1);
        R(rd) = old;
    });
    INSTPAT("??????? ????? ????? 011 ????? 1110011", "csrrc",  I, {
        uint32_t addr = (uint32_t)BITS(inst, 31, 20);
        word_t   old  = csr_read(addr);
        if (BITS(inst, 19, 15) != 0) csr_write(addr, old & ~src1);
        R(rd) = old;
    });
    INSTPAT("??????? ????? ????? 101 ????? 1110011", "csrrwi", I, {
        uint32_t addr = (uint32_t)BITS(inst, 31, 20);
        word_t   zimm = (word_t)BITS(inst, 19, 15);
        word_t   old  = csr_read(addr);
        csr_write(addr, zimm);
        R(rd) = old;
    });
    INSTPAT("??????? ????? ????? 110 ????? 1110011", "csrrsi", I, {
        uint32_t addr = (uint32_t)BITS(inst, 31, 20);
        word_t   zimm = (word_t)BITS(inst, 19, 15);
        word_t   old  = csr_read(addr);
        if (zimm != 0) csr_write(addr, old | zimm);
        R(rd) = old;
    });
    INSTPAT("??????? ????? ????? 111 ????? 1110011", "csrrci", I, {
        uint32_t addr = (uint32_t)BITS(inst, 31, 20);
        word_t   zimm = (word_t)BITS(inst, 19, 15);
        word_t   old  = csr_read(addr);
        if (zimm != 0) csr_write(addr, old & ~zimm);
        R(rd) = old;
    });

    /* --- system ---------------------------------------------------- */
    INSTPAT("0000000 00001 00000 000 00000 1110011", "ebreak", N,
            temu_set_end(s->pc, R(10)));

    /* --- catch-all: anything not matched above is illegal ---------- */
    INSTPAT("??????? ????? ????? ??? ????? ???????", "invalid", N,
            invalid_inst(s));

    #undef INSTPAT

    return 0;
}
