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

    /* --- system ---------------------------------------------------- */
    INSTPAT("0000000 00001 00000 000 00000 1110011", N,
            temu_set_end(s->pc, R(10)));   /* ebreak: halt with a0 */

    /* --- catch-all: anything not matched above is illegal ---------- */
    INSTPAT("??????? ????? ????? ??? ????? ???????", N,
            invalid_inst(s));

    #undef INSTPAT

    (void)rd; (void)src1; (void)src2; (void)imm;   /* silence unused-for-now */
    return 0;
}
