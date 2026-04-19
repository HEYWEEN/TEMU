#include "common.h"
#include "cpu.h"
#include "memory.h"
#include "local-include/inst.h"

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
/* Dispatch table — grows per sub-stage                                */
/* ------------------------------------------------------------------ */

int isa_exec_once(Decode *s) {
    uint32_t inst = s->inst;
    int    rd   = 0;
    word_t src1 = 0, src2 = 0, imm = 0;
    bool   matched = false;

    /* The table is scanned top-to-bottom; the first match wins. More
     * specific patterns (e.g. SUB with funct7 = 0100000) must come
     * before any superset that would also match. */
    #define INSTPAT(pat, type, body)                                  \
        if (!matched && pattern_match(inst, pat)) {                   \
            decode_operand(s, TYPE_##type, &rd, &src1, &src2, &imm);  \
            body;                                                     \
            matched = true;                                           \
        }

    /* --- upper-immediate ------------------------------------------- */
    INSTPAT("??????? ????? ????? ??? ????? 0110111", U,
            R(rd) = imm);                                /* LUI    */
    INSTPAT("??????? ????? ????? ??? ????? 0010111", U,
            R(rd) = s->pc + imm);                        /* AUIPC  */

    /* --- I-type arithmetic ----------------------------------------- */
    INSTPAT("??????? ????? ????? 000 ????? 0010011", I,
            R(rd) = src1 + imm);                         /* ADDI   */
    INSTPAT("??????? ????? ????? 010 ????? 0010011", I,
            R(rd) = (sword_t)src1 <  (sword_t)imm ? 1 : 0); /* SLTI  */
    INSTPAT("??????? ????? ????? 011 ????? 0010011", I,
            R(rd) = src1 < imm ? 1 : 0);                 /* SLTIU  */
    INSTPAT("??????? ????? ????? 100 ????? 0010011", I,
            R(rd) = src1 ^ imm);                         /* XORI   */
    INSTPAT("??????? ????? ????? 110 ????? 0010011", I,
            R(rd) = src1 | imm);                         /* ORI    */
    INSTPAT("??????? ????? ????? 111 ????? 0010011", I,
            R(rd) = src1 & imm);                         /* ANDI   */

    /* I-type shifts — shamt is imm[4:0]; funct7 distinguishes the
     * three variants. SRAI uses arithmetic right shift, relying on
     * signed-shift behaviour that is implementation-defined in C
     * but arithmetic on every compiler / target we ship on. */
    INSTPAT("0000000 ????? ????? 001 ????? 0010011", I,
            R(rd) = src1 << BITS(imm, 4, 0));            /* SLLI   */
    INSTPAT("0000000 ????? ????? 101 ????? 0010011", I,
            R(rd) = src1 >> BITS(imm, 4, 0));            /* SRLI   */
    INSTPAT("0100000 ????? ????? 101 ????? 0010011", I,
            R(rd) = (word_t)((sword_t)src1 >> BITS(imm, 4, 0))); /* SRAI */

    /* --- R-type arithmetic ----------------------------------------- */
    INSTPAT("0000000 ????? ????? 000 ????? 0110011", R,
            R(rd) = src1 + src2);                        /* ADD    */
    INSTPAT("0100000 ????? ????? 000 ????? 0110011", R,
            R(rd) = src1 - src2);                        /* SUB    */
    INSTPAT("0000000 ????? ????? 001 ????? 0110011", R,
            R(rd) = src1 << (src2 & 31));                /* SLL    */
    INSTPAT("0000000 ????? ????? 010 ????? 0110011", R,
            R(rd) = (sword_t)src1 <  (sword_t)src2 ? 1 : 0); /* SLT */
    INSTPAT("0000000 ????? ????? 011 ????? 0110011", R,
            R(rd) = src1 < src2 ? 1 : 0);                /* SLTU   */
    INSTPAT("0000000 ????? ????? 100 ????? 0110011", R,
            R(rd) = src1 ^ src2);                        /* XOR    */
    INSTPAT("0000000 ????? ????? 101 ????? 0110011", R,
            R(rd) = src1 >> (src2 & 31));                /* SRL    */
    INSTPAT("0100000 ????? ????? 101 ????? 0110011", R,
            R(rd) = (word_t)((sword_t)src1 >> (src2 & 31))); /* SRA */
    INSTPAT("0000000 ????? ????? 110 ????? 0110011", R,
            R(rd) = src1 | src2);                        /* OR     */
    INSTPAT("0000000 ????? ????? 111 ????? 0110011", R,
            R(rd) = src1 & src2);                        /* AND    */

    /* --- branches (B-type) ----------------------------------------- *
     * dnpc defaults to snpc; only overwrite when the condition holds. */
    INSTPAT("??????? ????? ????? 000 ????? 1100011", B,
            if (src1 == src2) s->dnpc = s->pc + imm);    /* BEQ  */
    INSTPAT("??????? ????? ????? 001 ????? 1100011", B,
            if (src1 != src2) s->dnpc = s->pc + imm);    /* BNE  */
    INSTPAT("??????? ????? ????? 100 ????? 1100011", B,
            if ((sword_t)src1 <  (sword_t)src2) s->dnpc = s->pc + imm); /* BLT  */
    INSTPAT("??????? ????? ????? 101 ????? 1100011", B,
            if ((sword_t)src1 >= (sword_t)src2) s->dnpc = s->pc + imm); /* BGE  */
    INSTPAT("??????? ????? ????? 110 ????? 1100011", B,
            if (src1 <  src2) s->dnpc = s->pc + imm);    /* BLTU */
    INSTPAT("??????? ????? ????? 111 ????? 1100011", B,
            if (src1 >= src2) s->dnpc = s->pc + imm);    /* BGEU */

    /* --- jumps ----------------------------------------------------- */
    /* JAL writes pc+4 to rd and jumps; JALR does the same but from a
     * register base. For both, save snpc before computing dnpc so the
     * rd == rs1 corner case works. */
    INSTPAT("??????? ????? ????? ??? ????? 1101111", J, {
        word_t link = s->snpc;
        s->dnpc = s->pc + imm;
        R(rd) = link;
    });                                                  /* JAL  */
    INSTPAT("??????? ????? ????? 000 ????? 1100111", I, {
        word_t link = s->snpc;
        s->dnpc = (src1 + imm) & ~(word_t)1;
        R(rd) = link;
    });                                                  /* JALR */

    /* --- system ---------------------------------------------------- */
    INSTPAT("0000000 00001 00000 000 00000 1110011", N,
            temu_set_end(s->pc, R(10)));   /* ebreak: halt with a0 */

    /* --- catch-all: anything not matched above is illegal ---------- */
    INSTPAT("??????? ????? ????? ??? ????? ???????", N,
            invalid_inst(s));

    #undef INSTPAT

    return 0;
}
