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

/* B-type: imm[12|10:5] rs2 rs1 funct3 imm[4:1|11] opcode
 * imm[0] is implicit zero. The caller gives the full byte offset. */
static inline uint32_t btype(int32_t imm, uint32_t rs2, uint32_t rs1,
                             uint32_t f3, uint32_t op) {
    uint32_t i = (uint32_t)imm;
    return (((i >> 12) & 0x1)  << 31) |
           (((i >> 5)  & 0x3f) << 25) |
           ((rs2 & 0x1f)       << 20) |
           ((rs1 & 0x1f)       << 15) |
           ((f3  & 0x07)       << 12) |
           (((i >> 1)  & 0xf)  <<  8) |
           (((i >> 11) & 0x1)  <<  7) |
            (op  & 0x7f);
}

/* S-type: imm[11:5] rs2 rs1 f3 imm[4:0] opcode. */
static inline uint32_t stype(int32_t imm, uint32_t rs2, uint32_t rs1,
                             uint32_t f3, uint32_t op) {
    uint32_t i = (uint32_t)imm;
    return (((i >> 5) & 0x7f) << 25) |
           ((rs2 & 0x1f)      << 20) |
           ((rs1 & 0x1f)      << 15) |
           ((f3  & 0x07)      << 12) |
           ((i   & 0x1f)      <<  7) |
            (op  & 0x7f);
}

/* J-type: imm[20|10:1|11|19:12] rd opcode. imm[0] implicit zero. */
static inline uint32_t jtype(int32_t imm, uint32_t rd, uint32_t op) {
    uint32_t i = (uint32_t)imm;
    return (((i >> 20) & 0x1)   << 31) |
           (((i >> 1)  & 0x3ff) << 21) |
           (((i >> 11) & 0x1)   << 20) |
           (((i >> 12) & 0xff)  << 12) |
           ((rd & 0x1f)         <<  7) |
            (op  & 0x7f);
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

/* B-type branches */
#define BEQ(rs1, rs2, imm)   btype(imm, rs2, rs1, 0x0, 0x63)
#define BNE(rs1, rs2, imm)   btype(imm, rs2, rs1, 0x1, 0x63)
#define BLT(rs1, rs2, imm)   btype(imm, rs2, rs1, 0x4, 0x63)
#define BGE(rs1, rs2, imm)   btype(imm, rs2, rs1, 0x5, 0x63)
#define BLTU(rs1, rs2, imm)  btype(imm, rs2, rs1, 0x6, 0x63)
#define BGEU(rs1, rs2, imm)  btype(imm, rs2, rs1, 0x7, 0x63)

/* Jumps */
#define JAL(rd, imm)         jtype(imm, rd, 0x6f)
#define JALR(rd, rs1, imm)   itype(imm, rs1, 0x0, rd, 0x67)
#define J(imm)               JAL(ZERO, imm)     /* plain jump */

/* Loads (I-type, opcode 0x03). rd <- mem[rs1 + imm]. */
#define LB(rd, rs1, imm)     itype(imm, rs1, 0x0, rd, 0x03)
#define LH(rd, rs1, imm)     itype(imm, rs1, 0x1, rd, 0x03)
#define LW(rd, rs1, imm)     itype(imm, rs1, 0x2, rd, 0x03)
#define LBU(rd, rs1, imm)    itype(imm, rs1, 0x4, rd, 0x03)
#define LHU(rd, rs1, imm)    itype(imm, rs1, 0x5, rd, 0x03)

/* Stores (S-type, opcode 0x23). mem[rs1 + imm] <- rs2. */
#define SB(rs2, rs1, imm)    stype(imm, rs2, rs1, 0x0, 0x23)
#define SH(rs2, rs1, imm)    stype(imm, rs2, rs1, 0x1, 0x23)
#define SW(rs2, rs1, imm)    stype(imm, rs2, rs1, 0x2, 0x23)

/* FENCE / FENCE.I — opcode 0x0f. Treated as NOP by TEMU. */
#define FENCE                itype(0, 0, 0x0, 0, 0x0f)
#define FENCE_I              itype(0, 0, 0x1, 0, 0x0f)

/* Pseudoinstructions built from the above. MV is ADDI with imm=0. */
#define MV(rd, rs)           ADDI(rd, rs, 0)
#define LI_SMALL(rd, imm)    ADDI(rd, ZERO, imm)   /* 12-bit signed */
#define NOP                  ADDI(ZERO, ZERO, 0)

#define EBREAK               0x00100073u

/* --- Zicsr — CSR access (opcode 0x73) ----------------------------- *
 * itype() masks imm to 12 bits, which is exactly what the CSR number
 * field occupies ([31:20]). For the "immediate" variants the rs1
 * field carries a 5-bit unsigned immediate instead of a register. */
#define CSRRW(rd, csr, rs1)   itype((int32_t)(csr), rs1, 0x1, rd, 0x73)
#define CSRRS(rd, csr, rs1)   itype((int32_t)(csr), rs1, 0x2, rd, 0x73)
#define CSRRC(rd, csr, rs1)   itype((int32_t)(csr), rs1, 0x3, rd, 0x73)
#define CSRRWI(rd, csr, zimm) itype((int32_t)(csr), (zimm) & 0x1f, 0x5, rd, 0x73)
#define CSRRSI(rd, csr, zimm) itype((int32_t)(csr), (zimm) & 0x1f, 0x6, rd, 0x73)
#define CSRRCI(rd, csr, zimm) itype((int32_t)(csr), (zimm) & 0x1f, 0x7, rd, 0x73)

/* CSR numbers matching src/isa/riscv32/local-include/csr.h. */
#define CSR_MSTATUS   0x300
#define CSR_MIE       0x304
#define CSR_MTVEC     0x305
#define CSR_MSCRATCH  0x340
#define CSR_MEPC      0x341
#define CSR_MCAUSE    0x342
#define CSR_MIP       0x344

#endif /* TEMU_TESTS_ISA_ENCODER_H */
