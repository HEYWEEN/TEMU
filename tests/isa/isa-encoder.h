/*
 * isa-encoder.h — helpers for hand-building RV32I instruction words
 * from test programs without needing a RISC-V toolchain on the host.
 *
 * Every helper returns a uint32_t. Test cases compose these into an
 * array, write it as a raw binary, and feed the binary to `temu -b`.
 */

#ifndef TEMU_TESTS_ISA_ENCODER_H
#define TEMU_TESTS_ISA_ENCODER_H

#include <stdint.h>

/* Register ABI aliases. Tests are more readable when we write
 * ADDI(A0, ZERO, 5) instead of ADDI(10, 0, 5). */
enum {
    ZERO = 0, RA = 1,  SP = 2,  GP = 3,  TP = 4,  T0 = 5,  T1 = 6,  T2 = 7,
    S0   = 8, S1 = 9,  A0 = 10, A1 = 11, A2 = 12, A3 = 13, A4 = 14, A5 = 15,
    A6  = 16, A7 = 17, S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23,
    S8  = 24, S9 = 25, S10 = 26, S11 = 27, T3 = 28, T4 = 29, T5 = 30, T6 = 31,
};

/* Raw field encoders. The explicit mask on each field protects against
 * a caller accidentally passing a value that does not fit and silently
 * corrupting a neighbouring field. */
static inline uint32_t rtype(uint32_t f7, uint32_t rs2, uint32_t rs1,
                             uint32_t f3, uint32_t rd, uint32_t op) {
    return ((f7  & 0x7f) << 25) | ((rs2 & 0x1f) << 20) |
           ((rs1 & 0x1f) << 15) | ((f3  & 0x07) << 12) |
           ((rd  & 0x1f) <<  7) |  (op  & 0x7f);
}

static inline uint32_t itype(int32_t imm, uint32_t rs1, uint32_t f3,
                             uint32_t rd, uint32_t op) {
    return (((uint32_t)imm & 0xfff) << 20) |
           ((rs1 & 0x1f) << 15) | ((f3 & 0x07) << 12) |
           ((rd  & 0x1f) <<  7) | (op & 0x7f);
}

/* I-type shift immediates: funct7 | shamt[4:0] in the imm field. */
static inline uint32_t ishift(uint32_t f7, uint32_t shamt, uint32_t rs1,
                              uint32_t f3, uint32_t rd, uint32_t op) {
    return ((f7    & 0x7f) << 25) | ((shamt & 0x1f) << 20) |
           ((rs1   & 0x1f) << 15) | ((f3    & 0x07) << 12) |
           ((rd    & 0x1f) <<  7) |  (op    & 0x7f);
}

static inline uint32_t utype(uint32_t imm20, uint32_t rd, uint32_t op) {
    /* imm20 is the upper 20 bits already placed at [31:12]; callers
     * typically pass a value with only those bits set. */
    return (imm20 & 0xfffff000) | ((rd & 0x1f) << 7) | (op & 0x7f);
}

/* --- RV32I mnemonics (only the ones exercised so far) ------------- */

/* I-type arithmetic */
#define ADDI(rd, rs1, imm)   itype(imm, rs1, 0x0, rd, 0x13)
#define SLTI(rd, rs1, imm)   itype(imm, rs1, 0x2, rd, 0x13)
#define SLTIU(rd, rs1, imm)  itype(imm, rs1, 0x3, rd, 0x13)
#define XORI(rd, rs1, imm)   itype(imm, rs1, 0x4, rd, 0x13)
#define ORI(rd, rs1, imm)    itype(imm, rs1, 0x6, rd, 0x13)
#define ANDI(rd, rs1, imm)   itype(imm, rs1, 0x7, rd, 0x13)

/* I-type shifts */
#define SLLI(rd, rs1, sh)    ishift(0x00, sh, rs1, 0x1, rd, 0x13)
#define SRLI(rd, rs1, sh)    ishift(0x00, sh, rs1, 0x5, rd, 0x13)
#define SRAI(rd, rs1, sh)    ishift(0x20, sh, rs1, 0x5, rd, 0x13)

/* R-type arithmetic */
#define ADD(rd, rs1, rs2)    rtype(0x00, rs2, rs1, 0x0, rd, 0x33)
#define SUB(rd, rs1, rs2)    rtype(0x20, rs2, rs1, 0x0, rd, 0x33)
#define SLL(rd, rs1, rs2)    rtype(0x00, rs2, rs1, 0x1, rd, 0x33)
#define SLT(rd, rs1, rs2)    rtype(0x00, rs2, rs1, 0x2, rd, 0x33)
#define SLTU(rd, rs1, rs2)   rtype(0x00, rs2, rs1, 0x3, rd, 0x33)
#define XOR(rd, rs1, rs2)    rtype(0x00, rs2, rs1, 0x4, rd, 0x33)
#define SRL(rd, rs1, rs2)    rtype(0x00, rs2, rs1, 0x5, rd, 0x33)
#define SRA(rd, rs1, rs2)    rtype(0x20, rs2, rs1, 0x5, rd, 0x33)
#define OR(rd, rs1, rs2)     rtype(0x00, rs2, rs1, 0x6, rd, 0x33)
#define AND(rd, rs1, rs2)    rtype(0x00, rs2, rs1, 0x7, rd, 0x33)

/* U-type */
#define LUI(rd, imm20)       utype(imm20, rd, 0x37)
#define AUIPC(rd, imm20)     utype(imm20, rd, 0x17)

/* Pseudoinstructions built from the above. MV is ADDI with imm=0. */
#define MV(rd, rs)           ADDI(rd, rs, 0)
#define LI_SMALL(rd, imm)    ADDI(rd, ZERO, imm)   /* 12-bit signed */
#define NOP                  ADDI(ZERO, ZERO, 0)

#define EBREAK               0x00100073u

#endif /* TEMU_TESTS_ISA_ENCODER_H */
