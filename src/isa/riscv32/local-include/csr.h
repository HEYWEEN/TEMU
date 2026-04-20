#ifndef RISCV32_CSR_H
#define RISCV32_CSR_H

#include "common.h"

/* --- M-mode CSR numbers (Privileged Spec v1.12, Table 2.2) ---------- */
#define CSR_MSTATUS   0x300
#define CSR_MIE       0x304
#define CSR_MTVEC     0x305
#define CSR_MSCRATCH  0x340
#define CSR_MEPC      0x341
#define CSR_MCAUSE    0x342
#define CSR_MIP       0x344

/* Bit masks. MIP / MIE share field positions — MTIP and MTIE are
 * both bit 7 — so one constant serves both registers. */
#define MSTATUS_MIE   (1u << 3)
#define MSTATUS_MPIE  (1u << 7)
#define MIP_MTIP      (1u << 7)
#define MIE_MTIE      (1u << 7)

/* Seven Machine-mode CSRs implemented in stage 6a. Other CSR numbers
 * are not rejected — reads return zero, writes are silently discarded —
 * so software that probes for optional CSRs does not trap. Stage 6b
 * will tighten this to trap on truly invalid numbers. */
typedef struct {
    word_t mstatus;
    word_t mie;
    word_t mtvec;
    word_t mscratch;
    word_t mepc;
    word_t mcause;
    word_t mip;
} CSR_state;

extern CSR_state csr;

void csr_init(void);

word_t csr_read (uint32_t addr);
void   csr_write(uint32_t addr, word_t val);

/* Returns the printable name ("mstatus", "mepc", ...) for a CSR
 * number, or NULL if unknown. Used by the disassembler. */
const char *csr_name(uint32_t addr);

#endif /* RISCV32_CSR_H */
