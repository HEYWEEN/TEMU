/*
 * difftest.c — second-opinion RV32I executor for differential testing.
 *
 * The main implementation in src/isa/riscv32/inst.c uses an INSTPAT
 * pattern-match table. This file implements the same ISA with a
 * deliberately different structure: a nested switch on
 * opcode/funct3/funct7, inline bit extraction, hand-written immediate
 * assembly. Any bug that manifests only in one of the two code styles
 * will cause the CPU states to diverge and be caught by
 * difftest_step() at the first offending instruction.
 *
 * The reference CPU shares pmem with the main CPU — a deliberate
 * trade-off (store bugs that both implementations agree on are
 * invisible), chosen so the difftest path does not double memory
 * usage. In practice a wrong store nearly always manifests as a
 * downstream register divergence, which we do catch.
 */

#include "common.h"
#include "cpu.h"
#include "difftest.h"
#include "memory.h"

#include "../isa/riscv32/local-include/inst.h"
#include "../isa/riscv32/local-include/csr.h"

/* ------------------------------------------------------------------ */
/* Reference-side state                                                */
/* ------------------------------------------------------------------ */

static CPU_state ref_cpu;
static CSR_state ref_csr;
static bool      ref_halted  = false;
static bool      ref_aborted = false;
static bool      enabled     = false;

void difftest_enable(bool on) { enabled = on; }
bool difftest_is_enabled(void) { return enabled; }

/* Second-opinion CSR file. Deliberately implemented with a switch
 * dispatch instead of the table-driven style used by src/isa/riscv32/
 * csr.c, so that a CSR-number typo on one side diverges from the
 * other and is caught by difftest_step. Shares the CSR_state type
 * (it's just seven scalar fields — no behaviour to diverge on) but
 * not the storage. */
static word_t ref_csr_read(uint32_t addr) {
    switch (addr) {
    case CSR_MSTATUS:  return ref_csr.mstatus;
    case CSR_MIE:      return ref_csr.mie;
    case CSR_MTVEC:    return ref_csr.mtvec;
    case CSR_MSCRATCH: return ref_csr.mscratch;
    case CSR_MEPC:     return ref_csr.mepc;
    case CSR_MCAUSE:   return ref_csr.mcause;
    case CSR_MIP:      return ref_csr.mip;
    default:           return 0;
    }
}

static void ref_csr_write(uint32_t addr, word_t val) {
    switch (addr) {
    case CSR_MSTATUS:  ref_csr.mstatus  = val; break;
    case CSR_MIE:      ref_csr.mie      = val; break;
    case CSR_MTVEC:    ref_csr.mtvec    = val; break;
    case CSR_MSCRATCH: ref_csr.mscratch = val; break;
    case CSR_MEPC:     ref_csr.mepc     = val; break;
    case CSR_MCAUSE:   ref_csr.mcause   = val; break;
    case CSR_MIP:      ref_csr.mip      = val; break;
    default:           break;
    }
}

/* ------------------------------------------------------------------ */
/* Bit / immediate helpers — inline shift + mask style, distinct from *
 * the BITS / SEXT macros used by the main implementation.            */
/* ------------------------------------------------------------------ */

static inline uint32_t sext32(uint32_t v, int width) {
    /* Sign-extend a `width`-bit value in v to 32 bits. */
    uint32_t sign_bit = 1u << (width - 1);
    uint32_t mask     = ((uint32_t)-1) << width;
    return (v & sign_bit) ? (v | mask) : v;
}

static inline int32_t imm_i(uint32_t inst) {
    return (int32_t)sext32((inst >> 20) & 0xfff, 12);
}

static inline int32_t imm_s(uint32_t inst) {
    uint32_t raw = (((inst >> 25) & 0x7f) << 5) | ((inst >> 7) & 0x1f);
    return (int32_t)sext32(raw, 12);
}

static inline int32_t imm_b(uint32_t inst) {
    uint32_t raw =
        (((inst >> 31) & 0x1)  << 12) |
        (((inst >> 7)  & 0x1)  << 11) |
        (((inst >> 25) & 0x3f) << 5)  |
        (((inst >> 8)  & 0xf)  << 1);
    return (int32_t)sext32(raw, 13);
}

static inline int32_t imm_u(uint32_t inst) {
    return (int32_t)(inst & 0xfffff000);
}

static inline int32_t imm_j(uint32_t inst) {
    uint32_t raw =
        (((inst >> 31) & 0x1)   << 20) |
        (((inst >> 12) & 0xff)  << 12) |
        (((inst >> 20) & 0x1)   << 11) |
        (((inst >> 21) & 0x3ff) << 1);
    return (int32_t)sext32(raw, 21);
}

#define G(n) (ref_cpu.gpr[n])

/* ------------------------------------------------------------------ */
/* ref_exec_one — one instruction on the reference CPU                 */
/* ------------------------------------------------------------------ */

static void ref_invalid(vaddr_t pc, uint32_t inst) {
    fprintf(stderr,
            "\33[1;31mdifftest: ref rejected instruction\33[0m "
            "at pc=0x%08" PRIx32 " inst=0x%08" PRIx32 "\n",
            pc, inst);
    ref_aborted = true;
}

static void ref_exec_one(void) {
    if (ref_halted || ref_aborted) return;

    uint32_t pc_cur  = ref_cpu.pc;
    uint32_t pc_next = pc_cur + 4;
    uint32_t inst    = (uint32_t)paddr_read(pc_cur, 4);

    uint32_t op  = (inst >>  0) & 0x7f;
    uint32_t rd  = (inst >>  7) & 0x1f;
    uint32_t f3  = (inst >> 12) & 0x07;
    uint32_t rs1 = (inst >> 15) & 0x1f;
    uint32_t rs2 = (inst >> 20) & 0x1f;
    uint32_t f7  = (inst >> 25) & 0x7f;

    switch (op) {

    case 0x37: /* LUI   */
        G(rd) = (uint32_t)imm_u(inst);
        break;

    case 0x17: /* AUIPC */
        G(rd) = pc_cur + (uint32_t)imm_u(inst);
        break;

    case 0x13: { /* OP-IMM */
        uint32_t a      = G(rs1);
        int32_t  sign_i = imm_i(inst);
        uint32_t uns_i  = (uint32_t)sign_i;
        switch (f3) {
        case 0x0: G(rd) = a + uns_i;                                   break;  /* ADDI  */
        case 0x2: G(rd) = ((int32_t)a <  sign_i) ? 1 : 0;              break;  /* SLTI  */
        case 0x3: G(rd) = (a <  uns_i) ? 1 : 0;                        break;  /* SLTIU */
        case 0x4: G(rd) = a ^ uns_i;                                   break;  /* XORI  */
        case 0x6: G(rd) = a | uns_i;                                   break;  /* ORI   */
        case 0x7: G(rd) = a & uns_i;                                   break;  /* ANDI  */
        case 0x1: G(rd) = a << (uns_i & 0x1f);                         break;  /* SLLI  */
        case 0x5:
            if (f7 == 0x00)      G(rd) = a >> (uns_i & 0x1f);                  /* SRLI  */
            else if (f7 == 0x20) G(rd) = (uint32_t)((int32_t)a >> (uns_i & 0x1f)); /* SRAI */
            else { ref_invalid(pc_cur, inst); return; }
            break;
        default:  ref_invalid(pc_cur, inst); return;
        }
        break;
    }

    case 0x33: { /* OP (register-register) */
        uint32_t a = G(rs1), b = G(rs2);
        if (f3 == 0x0 && f7 == 0x00) G(rd) = a + b;
        else if (f3 == 0x0 && f7 == 0x20) G(rd) = a - b;
        else if (f3 == 0x1 && f7 == 0x00) G(rd) = a << (b & 0x1f);
        else if (f3 == 0x2 && f7 == 0x00) G(rd) = ((int32_t)a <  (int32_t)b) ? 1 : 0;
        else if (f3 == 0x3 && f7 == 0x00) G(rd) = (a <  b) ? 1 : 0;
        else if (f3 == 0x4 && f7 == 0x00) G(rd) = a ^ b;
        else if (f3 == 0x5 && f7 == 0x00) G(rd) = a >> (b & 0x1f);
        else if (f3 == 0x5 && f7 == 0x20) G(rd) = (uint32_t)((int32_t)a >> (b & 0x1f));
        else if (f3 == 0x6 && f7 == 0x00) G(rd) = a | b;
        else if (f3 == 0x7 && f7 == 0x00) G(rd) = a & b;
        else { ref_invalid(pc_cur, inst); return; }
        break;
    }

    case 0x63: { /* BRANCH */
        uint32_t a = G(rs1), b = G(rs2);
        bool taken;
        switch (f3) {
        case 0x0: taken = (a == b);                         break;  /* BEQ  */
        case 0x1: taken = (a != b);                         break;  /* BNE  */
        case 0x4: taken = ((int32_t)a <  (int32_t)b);       break;  /* BLT  */
        case 0x5: taken = ((int32_t)a >= (int32_t)b);       break;  /* BGE  */
        case 0x6: taken = (a <  b);                         break;  /* BLTU */
        case 0x7: taken = (a >= b);                         break;  /* BGEU */
        default:  ref_invalid(pc_cur, inst); return;
        }
        if (taken) pc_next = pc_cur + (uint32_t)imm_b(inst);
        break;
    }

    case 0x6f: { /* JAL */
        int32_t off = imm_j(inst);
        G(rd)  = pc_next;
        pc_next = pc_cur + (uint32_t)off;
        break;
    }

    case 0x67: { /* JALR */
        if (f3 != 0x0) { ref_invalid(pc_cur, inst); return; }
        int32_t  off  = imm_i(inst);
        uint32_t a    = G(rs1);
        uint32_t tgt  = (a + (uint32_t)off) & ~1u;
        G(rd)  = pc_next;
        pc_next = tgt;
        break;
    }

    case 0x03: { /* LOAD */
        uint32_t addr = G(rs1) + (uint32_t)imm_i(inst);
        word_t   raw;
        switch (f3) {
        case 0x0: /* LB  */
            raw = paddr_read(addr, 1);
            G(rd) = (raw & 0x80) ? (raw | 0xffffff00u) : raw;
            break;
        case 0x1: /* LH  */
            raw = paddr_read(addr, 2);
            G(rd) = (raw & 0x8000) ? (raw | 0xffff0000u) : raw;
            break;
        case 0x2: /* LW  */
            G(rd) = paddr_read(addr, 4);
            break;
        case 0x4: /* LBU */
            G(rd) = paddr_read(addr, 1);
            break;
        case 0x5: /* LHU */
            G(rd) = paddr_read(addr, 2);
            break;
        default:  ref_invalid(pc_cur, inst); return;
        }
        break;
    }

    case 0x23: { /* STORE */
        uint32_t addr = G(rs1) + (uint32_t)imm_s(inst);
        word_t   val  = G(rs2);
        switch (f3) {
        case 0x0: paddr_write(addr, 1, val); break;  /* SB */
        case 0x1: paddr_write(addr, 2, val); break;  /* SH */
        case 0x2: paddr_write(addr, 4, val); break;  /* SW */
        default:  ref_invalid(pc_cur, inst); return;
        }
        break;
    }

    case 0x0f: /* MISC-MEM — FENCE / FENCE.I treated as NOP */
        break;

    case 0x73: { /* SYSTEM — EBREAK (halt) or Zicsr */
        if (inst == 0x00100073u) {
            ref_halted = true;
            /* Do not advance pc_next — main side doesn't either, and
             * keeping them identical is the whole point. */
            return;
        }
        if (f3 == 0) {
            /* ECALL and other SYSTEM encodings land here in stage 6a-3;
             * for now any non-EBREAK f3=0 is rejected. */
            ref_invalid(pc_cur, inst);
            return;
        }
        /* Zicsr. f3 bit 2 selects immediate vs register form; low 2
         * bits select RW/RS/RC. */
        uint32_t addr    = (inst >> 20) & 0xfff;
        bool     is_imm  = (f3 & 0x4) != 0;
        uint32_t zimm    = rs1;             /* rs1 field as 5-bit imm  */
        uint32_t operand = is_imm ? zimm : G(rs1);
        bool     src_nz  = is_imm ? (zimm != 0) : (rs1 != 0);
        word_t   old     = ref_csr_read(addr);
        switch (f3 & 0x3) {
        case 0x1:                          ref_csr_write(addr, operand);    break;
        case 0x2: if (src_nz)              ref_csr_write(addr, old | operand); break;
        case 0x3: if (src_nz)              ref_csr_write(addr, old & ~operand); break;
        default:  ref_invalid(pc_cur, inst); return;
        }
        G(rd) = old;
        break;
    }

    default:
        ref_invalid(pc_cur, inst);
        return;
    }

    ref_cpu.pc     = pc_next;
    ref_cpu.gpr[0] = 0;
}

/* ------------------------------------------------------------------ */
/* Orchestration                                                       */
/* ------------------------------------------------------------------ */

void difftest_init(void) {
    memcpy(&ref_cpu, &cpu, sizeof(CPU_state));
    memcpy(&ref_csr, &csr, sizeof(CSR_state));
    ref_halted  = false;
    ref_aborted = false;
    Log("difftest: initialized — ref pc=0x%08" PRIx32, ref_cpu.pc);
}

void difftest_step(void) {
    if (!enabled) return;

    /* If the main-side instruction just touched MMIO, the operation
     * had an observable side effect (e.g. putchar) or yielded a time-
     * varying read (timer). Replaying it on the reference CPU would
     * either duplicate the side effect or diverge on a benign
     * difference. Instead, snapshot the main CPU onto ref_cpu so the
     * two stay aligned going forward, and skip this step's compare. */
    if (paddr_touched_mmio) {
        memcpy(&ref_cpu, &cpu, sizeof(CPU_state));
        memcpy(&ref_csr, &csr, sizeof(CSR_state));
        paddr_touched_mmio = false;
        return;
    }

    ref_exec_one();

    bool mismatch = false;
    for (int i = 0; i < 32; i++) {
        if (cpu.gpr[i] != ref_cpu.gpr[i]) {
            fprintf(stderr,
                    "  gpr[%2d] (%-4s): mine=0x%08" PRIx32
                    "  ref=0x%08" PRIx32 "\n",
                    i, reg_name(i), cpu.gpr[i], ref_cpu.gpr[i]);
            mismatch = true;
        }
    }
    if (cpu.pc != ref_cpu.pc) {
        fprintf(stderr,
                "  pc            : mine=0x%08" PRIx32
                "  ref=0x%08" PRIx32 "\n",
                cpu.pc, ref_cpu.pc);
        mismatch = true;
    }

    /* CSR compare. Laid out explicitly — seven fields is few enough
     * that a table offers no win over unrolling, and the unrolled form
     * survives adding new CSRs in 6b/6c with one extra line each. */
    #define CSR_CMP(field) do {                                              \
        if (csr.field != ref_csr.field) {                                    \
            fprintf(stderr,                                                  \
                    "  csr %-8s  : mine=0x%08" PRIx32                        \
                    "  ref=0x%08" PRIx32 "\n",                               \
                    #field, csr.field, ref_csr.field);                       \
            mismatch = true;                                                 \
        }                                                                    \
    } while (0)
    CSR_CMP(mstatus);
    CSR_CMP(mie);
    CSR_CMP(mtvec);
    CSR_CMP(mscratch);
    CSR_CMP(mepc);
    CSR_CMP(mcause);
    CSR_CMP(mip);
    #undef CSR_CMP

    if (ref_aborted) {
        fprintf(stderr,
                "  ref aborted but main kept running — divergence\n");
        mismatch = true;
    }

    if (mismatch) {
        char buf[64];
        disasm(buf, sizeof buf, &g_last_disasm);
        fprintf(stderr,
                "\33[1;31mdifftest: CPU state diverged from reference\33[0m\n"
                "  last instruction: 0x%08" PRIx32 "  %s\n",
                g_last_disasm.inst, buf);
        temu_set_abort();
    }
}
