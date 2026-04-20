/*
 * isa.c — unified RV32I instruction runner.
 *
 * Each test builds a tiny program as a uint32_t array using the
 * encoders in isa-encoder.h, writes it as a raw binary, spawns
 * `temu -b`, and asserts the `halt_ret=` value printed on HIT END.
 *
 * The convention is: arrange the test so the value of interest sits
 * in a0 (x10) just before EBREAK — halt_ret is the a0 value at halt.
 *
 * This test does *not* link against the emulator; it exercises it via
 * a subprocess. That means the tests catch real end-to-end breakage
 * (decode, operand extraction, cpu_exec loop, PC update) rather than
 * narrow unit bugs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "isa-encoder.h"

static int  pass_count = 0;
static int  fail_count = 0;
static bool verbose    = false;
static bool difftest   = false;
static const char *temu_path;

/* Run a hand-assembled program through temu and compare halt_ret. */
static void check(const char *name, uint32_t expected_ret,
                  const uint32_t *prog, size_t n) {
    char path[] = "/tmp/temu-isa-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }

    size_t bytes = n * sizeof(uint32_t);
    if (write(fd, prog, bytes) != (ssize_t)bytes) {
        perror("write"); close(fd); unlink(path); exit(2);
    }
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s -b%s %s 2>/dev/null",
             temu_path, difftest ? "d" : "", path);
    FILE *pipe = popen(cmd, "r");
    if (pipe == NULL) { perror("popen"); unlink(path); exit(2); }

    char line[512];
    bool     found = false;
    bool     aborted = false;
    uint32_t got = 0;
    while (fgets(line, sizeof line, pipe)) {
        if (strstr(line, "HIT ABORT"))     aborted = true;
        const char *m = strstr(line, "halt_ret=0x");
        if (m) { got = (uint32_t)strtoul(m + 11, NULL, 16); found = true; }
    }
    int rc = pclose(pipe);
    unlink(path);

    if (aborted) {
        printf("  %-16s FAIL (HIT ABORT)\n", name);
        fail_count++;
    } else if (!found) {
        printf("  %-16s FAIL (no halt_ret in output, rc=%d)\n", name, rc);
        fail_count++;
    } else if (got != expected_ret) {
        printf("  %-16s FAIL (got 0x%08x, expected 0x%08x)\n",
               name, got, expected_ret);
        fail_count++;
    } else {
        pass_count++;
        if (verbose) printf("  %-16s OK  (a0 = 0x%08x)\n", name, got);
    }
}

/* Convenience: accept a variable-length argument list of encoded words,
 * append EBREAK, and call check(). */
#define TEST(name, expected_ret, ...) do {                  \
    uint32_t prog_[] = { __VA_ARGS__, EBREAK };             \
    check(name, (uint32_t)(expected_ret),                   \
          prog_, sizeof prog_ / sizeof prog_[0]);           \
} while (0)

/* ------------------------------------------------------------------ */
/* Test cases                                                          */
/* ------------------------------------------------------------------ */

static void test_addi(void) {
    TEST("addi positive", 5,
         ADDI(A0, ZERO, 5));
    TEST("addi chain", 15,
         ADDI(A0, ZERO, 5),
         ADDI(A0, A0, 10));
    TEST("addi negative", 0xfffffffb,      /* -5 as uint32 */
         ADDI(A0, ZERO, -5));
    TEST("addi x0 discard", 0,             /* writes to x0 must be ignored */
         ADDI(ZERO, ZERO, 42));
}

static void test_logical_immediate(void) {
    TEST("xori", 0xff,
         ADDI(T0, ZERO, 0x0f),
         XORI(A0, T0, 0xf0));             /* 0x0f ^ 0xfffffff0 = 0xffffffff? */
    /* XORI with positive 12-bit imm 0xf0 = 240; 0x0f ^ 0xf0 = 0xff. OK. */

    TEST("ori",  0xff,
         ADDI(T0, ZERO, 0x0f),
         ORI(A0, T0, 0xf0));

    TEST("andi", 0x0f,
         ADDI(T0, ZERO, 0xff),            /* 0xff as signed 12-bit is -1 -> 0xffffffff */
         ANDI(A0, T0, 0x0f));
    /* careful: ADDI imm 0xff is positive (255); low-12-bit value 0x0ff
     * sign bit (bit 11) is 0. So t0 = 255. Then 255 & 0x0f = 0x0f. OK. */
}

static void test_slt(void) {
    TEST("slti signed lt",   1,
         ADDI(T0, ZERO, -5),
         SLTI(A0, T0, 0));                 /* -5 < 0 signed */
    TEST("slti signed gt",   0,
         ADDI(T0, ZERO, 5),
         SLTI(A0, T0, 0));
    /* SLTIU sign-extends the 12-bit immediate then compares unsigned.
     * imm=-1 becomes 0xffffffff, so 0 < 0xffffffff is 1. */
    TEST("sltiu ne-max", 1,
         SLTIU(A0, ZERO, -1));
    TEST("sltiu eq-zero", 0,
         SLTIU(A0, ZERO, 0));
}

static void test_shift_immediate(void) {
    TEST("slli",  0x80,
         ADDI(T0, ZERO, 1),
         SLLI(A0, T0, 7));                  /* 1 << 7 = 0x80 */
    TEST("srli",  0x01,
         ADDI(T0, ZERO, 0x100),
         SRLI(A0, T0, 8));                  /* 0x100 >> 8 = 1 */
    TEST("srli big", 0x00ffffff,
         ADDI(T0, ZERO, -1),                /* 0xffffffff */
         SRLI(A0, T0, 8));
    TEST("srai neg", 0xffffffff,
         ADDI(T0, ZERO, -1),                /* 0xffffffff */
         SRAI(A0, T0, 8));                  /* arithmetic: still 0xffffffff */
    TEST("srai pos", 0x01,
         ADDI(T0, ZERO, 0x100),
         SRAI(A0, T0, 8));
}

static void test_rtype_arith(void) {
    TEST("add",  5,
         ADDI(T0, ZERO, 2),
         ADDI(T1, ZERO, 3),
         ADD(A0, T0, T1));
    TEST("sub neg-to-unsigned", 0xffffffff,
         ADDI(T0, ZERO, 0),
         ADDI(T1, ZERO, 1),
         SUB(A0, T0, T1));                   /* 0 - 1 = 0xffffffff */
    TEST("sll",  0x80,
         ADDI(T0, ZERO, 1),
         ADDI(T1, ZERO, 7),
         SLL(A0, T0, T1));
    TEST("srl big", 0x00ffffff,
         ADDI(T0, ZERO, -1),
         ADDI(T1, ZERO, 8),
         SRL(A0, T0, T1));
    TEST("sra neg",   0xffffffff,
         ADDI(T0, ZERO, -1),
         ADDI(T1, ZERO, 8),
         SRA(A0, T0, T1));
    TEST("slt signed lt", 1,
         ADDI(T0, ZERO, -5),
         ADDI(T1, ZERO, 5),
         SLT(A0, T0, T1));
    TEST("sltu unsigned",  0,
         ADDI(T0, ZERO, -1),                 /* 0xffffffff */
         ADDI(T1, ZERO, 1),
         SLTU(A0, T0, T1));                  /* 0xffffffff < 1 = 0 */
    TEST("xor", 0xf0,
         ADDI(T0, ZERO, 0x0f),
         ADDI(T1, ZERO, 0xff),
         XOR(A0, T0, T1));
    TEST("or",  0xff,
         ADDI(T0, ZERO, 0x0f),
         ADDI(T1, ZERO, 0xf0),
         OR(A0, T0, T1));
    TEST("and", 0x0f,
         ADDI(T0, ZERO, 0x0f),
         ADDI(T1, ZERO, 0xff),
         AND(A0, T0, T1));
}

static void test_upper_immediate(void) {
    /* LUI loads imm << 12 into rd. */
    TEST("lui",    0x12345000u,
         LUI(A0, 0x12345000));
    /* AUIPC adds pc to imm. First instruction runs at PMEM_BASE =
     * 0x80000000, so auipc a0, 0x12345000 → a0 = 0x80000000 + 0x12345000
     * = 0x92345000. */
    TEST("auipc",  0x92345000u,
         AUIPC(A0, 0x12345000));
}

static void test_x0_hardwired(void) {
    TEST("x0 read is zero", 0,
         ADDI(A0, ZERO, 42),             /* a0 = 42 */
         ADD(A0, ZERO, ZERO));           /* a0 = x0 + x0 = 0 */
}

/* Branch-test template. The pattern after the two register-setup insns
 * is always:
 *   [N-3]  BCC ..., +12          ; if taken, jump 3 insns ahead
 *   [N-2]  ADDI a0, zero, 77     ; fall-through marker
 *   [N-1]  JAL  zero, +8         ; skip the taken marker and halt
 *   [N]    ADDI a0, zero, 88     ; taken marker
 * halt_ret = 88 when taken, 77 when not taken. */
#define BRANCH_BODY(BCC, rs1, rs2)     \
    BCC(rs1, rs2, 12),                 \
    ADDI(A0, ZERO, 77),                \
    JAL(ZERO, 8),                      \
    ADDI(A0, ZERO, 88)

static void test_load_store(void) {
    /* Scratch pmem slot. LUI fills bits [31:12]; low 12 bits are zero,
     * so t0 = 0x80001000 exactly — well inside pmem (PMEM_BASE + 4KB). */
    #define SCRATCH LUI(T0, 0x80001000)

    /* SW then LW: full 32-bit round-trip. Build 0x01020304 with
     * LUI(0x01020000) + ADDI(0x304); bit 11 of 0x304 is zero so the
     * ADDI is a simple positive addition. */
    TEST("sw + lw", 0x01020304u,
         SCRATCH,
         LUI(T1, 0x01020000),
         ADDI(T1, T1, 0x304),
         SW(T1, T0, 0),
         LW(A0, T0, 0));

    /* SB + LB: byte 0xff sign-extends to 0xffffffff. */
    TEST("sb + lb sign ext", 0xffffffffu,
         SCRATCH,
         ADDI(T1, ZERO, -1),        /* t1 = 0xffffffff */
         SB(T1, T0, 0),
         LB(A0, T0, 0));

    /* SB + LBU: same byte, zero-extended stays 0xff. */
    TEST("sb + lbu zero ext", 0xffu,
         SCRATCH,
         ADDI(T1, ZERO, -1),
         SB(T1, T0, 4),             /* different offset; each test
                                        shares the same scratch word */
         LBU(A0, T0, 4));

    /* SH + LH: write 0x8000 (bit 15 set), LH sign-extends to
     * 0xffff8000. */
    TEST("sh + lh sign ext", 0xffff8000u,
         SCRATCH,
         LUI(T1, 0x00008000),       /* t1 = 0x00008000 — bit 15 set */
         SH(T1, T0, 8),
         LH(A0, T0, 8));

    /* SH + LHU: same halfword, zero-extended stays 0x8000. */
    TEST("sh + lhu zero ext", 0x8000u,
         SCRATCH,
         LUI(T1, 0x00008000),
         SH(T1, T0, 12),
         LHU(A0, T0, 12));

    /* SB at offsets 0..3, then LW to confirm little-endian packing.
     * Each byte value fits positively in 12-bit signed so ADDI works
     * directly. */
    TEST("sb little-endian pack", 0xddccbbaau,
         SCRATCH,
         ADDI(T1, ZERO, 0xaa),
         SB(T1, T0, 16),
         ADDI(T1, ZERO, 0xbb),
         SB(T1, T0, 17),
         ADDI(T1, ZERO, 0xcc),
         SB(T1, T0, 18),
         ADDI(T1, ZERO, 0xdd),
         SB(T1, T0, 19),
         LW(A0, T0, 16));

    /* FENCE / FENCE.I — pure NOPs. Put them between an ADDI sequence
     * and verify they don't disturb the result. */
    TEST("fence is nop", 42,
         ADDI(A0, ZERO, 42),
         FENCE,
         FENCE_I);

    #undef SCRATCH
}

static void test_branches(void) {
    TEST("beq taken",     88,
         /* 0==0 → taken */
         BRANCH_BODY(BEQ, ZERO, ZERO));
    TEST("beq not-taken", 77,
         ADDI(T0, ZERO, 1),
         BRANCH_BODY(BEQ, ZERO, T0));

    TEST("bne taken",     88,
         ADDI(T0, ZERO, 1),
         BRANCH_BODY(BNE, ZERO, T0));
    TEST("bne not-taken", 77,
         BRANCH_BODY(BNE, ZERO, ZERO));

    /* Same operands: BLT signed takes, BLTU unsigned does not. */
    TEST("blt signed taken",   88,
         ADDI(T0, ZERO, -5),
         ADDI(T1, ZERO, 5),
         BRANCH_BODY(BLT, T0, T1));
    TEST("bltu unsigned NT",   77,
         ADDI(T0, ZERO, -5),       /* 0xfffffffb > 5 as unsigned */
         ADDI(T1, ZERO, 5),
         BRANCH_BODY(BLTU, T0, T1));

    TEST("bge equal",          88,
         ADDI(T0, ZERO, 5),
         ADDI(T1, ZERO, 5),
         BRANCH_BODY(BGE, T0, T1));
    TEST("bgeu unsigned",      88,
         ADDI(T0, ZERO, -1),       /* 0xffffffff >= 1 as unsigned */
         ADDI(T1, ZERO, 1),
         BRANCH_BODY(BGEU, T0, T1));

    /* Backward branch — sum 3+2+1 = 6 using a loop. */
    TEST("bne loop countdown", 6,
         ADDI(T0, ZERO, 3),                /* counter */
         ADDI(A0, ZERO, 0),                /* accumulator */
         ADD(A0, A0, T0),                  /* loop: a0 += t0 */
         ADDI(T0, T0, -1),
         BNE(T0, ZERO, -8));               /* jump back to ADD */
}

static void test_jumps(void) {
    /* JAL forward: skip the dummy, land at the intended target, check
     * that ra = pc+4 of JAL (so ra = 0x80000004 since JAL is at 0x80000000
     * and snpc = 0x80000004). Actually no — after ADDI A0,ZERO,99 comes
     * JAL at offset 4. JAL snpc = 0x80000008. Put RA into A0 to check. */
    TEST("jal link + jump", 0x80000008u,
         ADDI(A0, ZERO, 99),               /* 0x80000000 — discarded */
         JAL(RA, 8),                       /* 0x80000004 — jumps to 0x8000000C */
         ADDI(A0, ZERO, 7),                /* 0x80000008 — skipped */
         MV(A0, RA));                      /* 0x8000000C — a0 = ra = 0x80000008 */

    /* Function-call pattern: caller hands off to a function via JAL(RA, ...)
     * and the function returns via JALR(ZERO, RA, 0). The callee is
     * placed *before* the caller, so the call offset is negative. */
    TEST("jal backward + jalr return", 10,
         JAL(ZERO, 12),                    /* 0x80000000 — skip callee */
         ADDI(A0, A0, 5),                  /* 0x80000004 — func: a0 += 5 */
         JALR(ZERO, RA, 0),                /* 0x80000008 — return to caller */
         ADDI(A0, ZERO, 5),                /* 0x8000000C — caller: a0 = 5 */
         JAL(RA, -12));                    /* 0x80000010 — call func; target 0x80000004 */

    /* JALR absolute-ish: load base into t0, jump to t0 + imm. Use AUIPC
     * to get current PC. */
    TEST("jalr to computed target", 42,
         AUIPC(T0, 0),                     /* t0 = 0x80000000 */
         JALR(ZERO, T0, 12),               /* pc = 0x80000000 + 12 = 0x8000000C */
         ADDI(A0, ZERO, 7),                /* 0x80000008 — skipped */
         ADDI(A0, ZERO, 42));              /* 0x8000000C — landed */
}

/* Zicsr. All tests funnel the final value into a0 via
 *   csrrs a0, <csr>, zero
 * which is a pure read (rs1=x0 must not write — spec rule that's easy
 * to miss). mscratch is the workhorse since it has no side effects;
 * mtvec and mepc are also exercised so table-driven dispatch is proven
 * correct across CSR numbers, not just on one lucky entry. */
static void test_csr(void) {
    /* Basic write-then-read. Build 0x12345678 with LUI + small ADDI
     * (0x678 = 1656 fits 12-bit signed). */
    TEST("csrrw + csrrs read", 0x12345678u,
         LUI(T0, 0x12345000),
         ADDI(T0, T0, 0x678),
         CSRRW(ZERO, CSR_MSCRATCH, T0),
         CSRRS(A0, CSR_MSCRATCH, ZERO));

    /* csrrw returns the pre-write value in rd. */
    TEST("csrrw returns old", 0xaau,
         ADDI(T0, ZERO, 0xaa),
         CSRRW(ZERO, CSR_MSCRATCH, T0),
         ADDI(T1, ZERO, 0xbb),
         CSRRW(A0, CSR_MSCRATCH, T1));       /* a0 <- old 0xaa */

    /* Aliased rd == rs1 for csrrw. Spec demands the *original* CSR
     * value end up in rd even though rs1 gets reused — equivalent to
     * reading-before-writing. A naive implementation that writes
     * before reading would put the old t0 value in t0. */
    TEST("csrrw aliased rd=rs1", 0x55u,
         ADDI(T0, ZERO, 0x55),
         CSRRW(ZERO, CSR_MSCRATCH, T0),       /* mscratch = 0x55 */
         ADDI(T0, ZERO, 0x66),                /* t0 = 0x66 */
         CSRRW(T0, CSR_MSCRATCH, T0),         /* t0 <- 0x55; mscratch <- 0x66 */
         MV(A0, T0));

    /* csrrs OR-sets bits. Keep operands under 0x7ff so ADDI's 12-bit
     * signed immediate doesn't sign-extend them into negative territory. */
    TEST("csrrs or-set", 0x10fu,
         ADDI(T0, ZERO, 0x100),
         CSRRW(ZERO, CSR_MSCRATCH, T0),
         ADDI(T1, ZERO, 0x00f),
         CSRRS(ZERO, CSR_MSCRATCH, T1),       /* mscratch |= 0x00f */
         CSRRS(A0, CSR_MSCRATCH, ZERO));

    /* csrrc AND-NOTs bits. */
    TEST("csrrc clear-bits", 0x100u,
         ADDI(T0, ZERO, 0x10f),
         CSRRW(ZERO, CSR_MSCRATCH, T0),
         ADDI(T1, ZERO, 0x00f),
         CSRRC(ZERO, CSR_MSCRATCH, T1),
         CSRRS(A0, CSR_MSCRATCH, ZERO));

    /* Immediate variants: the 5-bit rs1 field is zero-extended, so
     * 0x1f is the largest representable operand. */
    TEST("csrrwi sets zimm", 31,
         CSRRWI(ZERO, CSR_MSCRATCH, 31),
         CSRRS(A0, CSR_MSCRATCH, ZERO));

    TEST("csrrsi or-imm", 0xffu,
         ADDI(T0, ZERO, 0xe0),
         CSRRW(ZERO, CSR_MSCRATCH, T0),
         CSRRSI(ZERO, CSR_MSCRATCH, 0x1f),
         CSRRS(A0, CSR_MSCRATCH, ZERO));

    TEST("csrrci clear-imm", 0xe0u,
         ADDI(T0, ZERO, 0xff),
         CSRRW(ZERO, CSR_MSCRATCH, T0),
         CSRRCI(ZERO, CSR_MSCRATCH, 0x1f),
         CSRRS(A0, CSR_MSCRATCH, ZERO));

    /* Separate CSRs should not alias. Write 0x80001000 to mtvec and
     * 0xdead0000 to mscratch; reading mtvec must return its own value,
     * not mscratch's. */
    TEST("mtvec separate from mscratch", 0x80001000u,
         LUI(T0, 0x80001000),
         LUI(T1, 0xdead0000),
         CSRRW(ZERO, CSR_MTVEC,    T0),
         CSRRW(ZERO, CSR_MSCRATCH, T1),
         CSRRS(A0,   CSR_MTVEC,    ZERO));

    TEST("mepc round-trip", 0x80002000u,
         LUI(T0, 0x80002000),
         CSRRW(ZERO, CSR_MEPC, T0),
         CSRRS(A0, CSR_MEPC, ZERO));

    /* Unknown CSR number — stage 6a policy is read-zero / write-drop
     * (not trap). Writing 0xff then reading back must give 0. */
    TEST("unknown csr reads zero", 0,
         ADDI(T0, ZERO, 0xff),
         CSRRW(ZERO, 0x7c0, T0),
         CSRRS(A0, 0x7c0, ZERO));
}

/* Trap + mret round-trips. Each program skips over its handler, sets
 * mtvec to the handler's absolute address, then executes ecall. The
 * handler does its work, bumps mepc by 4 (so mret returns to the
 * instruction AFTER the ecall, not the ecall itself — the spec's
 * rule that's easy to miss), and returns via mret. The caller lands
 * on the auto-appended EBREAK and `halt_ret = a0`. */
static void test_trap(void) {
    /* Round-trip sanity: ecall reaches the handler, which sets a0=42
     * and returns; halt_ret should be 42. Handler lives at pmem
     * offset 0x04; main starts at 0x18 after `J 24`. */
    TEST("ecall trap round-trip", 42,
         /* 0x00 */ J(24),                          /* skip to main at 0x18 */
         /* 0x04 */ ADDI(A0, ZERO, 42),             /* handler */
         /* 0x08 */ CSRRS(T1, CSR_MEPC, ZERO),
         /* 0x0c */ ADDI(T1, T1, 4),
         /* 0x10 */ CSRRW(ZERO, CSR_MEPC, T1),
         /* 0x14 */ MRET,
         /* 0x18 */ LUI(T0, 0x80000000),            /* main: build handler addr */
         /* 0x1c */ ADDI(T0, T0, 4),
         /* 0x20 */ CSRRW(ZERO, CSR_MTVEC, T0),
         /* 0x24 */ ECALL                           /* trap; mret returns to 0x28 = EBREAK */
    );

    /* mcause is 11 for an ECALL-from-M. Handler loads mcause into a0
     * so the test observes the value that was written on trap entry. */
    TEST("ecall sets mcause=11", 11,
         /* 0x00 */ J(24),
         /* 0x04 */ CSRRS(A0, CSR_MCAUSE, ZERO),    /* handler: a0 = mcause */
         /* 0x08 */ CSRRS(T1, CSR_MEPC, ZERO),
         /* 0x0c */ ADDI(T1, T1, 4),
         /* 0x10 */ CSRRW(ZERO, CSR_MEPC, T1),
         /* 0x14 */ MRET,
         /* 0x18 */ LUI(T0, 0x80000000),
         /* 0x1c */ ADDI(T0, T0, 4),
         /* 0x20 */ CSRRW(ZERO, CSR_MTVEC, T0),
         /* 0x24 */ ECALL
    );

    /* mepc is set to the PC of the ECALL itself (0x80000024 in this
     * layout), NOT the PC of the following instruction. Handler
     * reads it back into a0 without bumping, then fixes mepc and
     * returns. */
    TEST("ecall saves mepc = pc_of_ecall", 0x80000024u,
         /* 0x00 */ J(24),
         /* 0x04 */ CSRRS(A0, CSR_MEPC, ZERO),      /* handler: a0 = mepc */
         /* 0x08 */ ADDI(T1, A0, 4),                /* advance past ecall */
         /* 0x0c */ CSRRW(ZERO, CSR_MEPC, T1),
         /* 0x10 */ MRET,
         /* 0x14 */ NOP,                            /* pad so main still starts at 0x18 */
         /* 0x18 */ LUI(T0, 0x80000000),
         /* 0x1c */ ADDI(T0, T0, 4),
         /* 0x20 */ CSRRW(ZERO, CSR_MTVEC, T0),
         /* 0x24 */ ECALL
    );

    /* mstatus invariant: set MIE=1 before the trap; after mret, MIE
     * must be 1 again (restored from MPIE). Mask to the MIE bit so
     * unrelated mstatus bits don't leak into halt_ret. */
    TEST("mret restores MIE", 8,                     /* bit 3 = 8 */
         /* 0x00 */ J(20),                           /* skip handler (4 insns) to main at 0x14 */
         /* 0x04 */ CSRRS(T1, CSR_MEPC, ZERO),       /* handler */
         /* 0x08 */ ADDI(T1, T1, 4),
         /* 0x0c */ CSRRW(ZERO, CSR_MEPC, T1),
         /* 0x10 */ MRET,
         /* 0x14 */ LUI(T0, 0x80000000),             /* main */
         /* 0x18 */ ADDI(T0, T0, 4),                 /* handler = 0x80000004 */
         /* 0x1c */ CSRRW(ZERO, CSR_MTVEC, T0),
         /* 0x20 */ ADDI(T1, ZERO, 8),               /* t1 = MIE bit */
         /* 0x24 */ CSRRS(ZERO, CSR_MSTATUS, T1),    /* set MIE=1 */
         /* 0x28 */ ECALL,
         /* 0x2c */ CSRRS(A0, CSR_MSTATUS, ZERO),    /* read mstatus back */
         /* 0x30 */ ANDI(A0, A0, 8)                  /* isolate MIE */
    );
}

/* Machine timer interrupt demo. The program:
 *   1. Points mtvec at a handler.
 *   2. Arms mtimecmp = 0 so the timer compare fires immediately once
 *      interrupts are globally enabled.
 *   3. Enables mstatus.MIE and mie.MTIE.
 *   4. Spins on a counter (s0). The handler bumps s0 and rearms the
 *      timer unless s0 reached 3, in which case it pushes mtimecmp to
 *      UINT64_MAX so no further interrupts fire.
 *
 * After three interrupts the loop falls through and EBREAK halts
 * with a0 = 3. This exercises the whole trap-delivery path: timer
 * poll, MIE gating, trap staging, mepc bookkeeping, mret restore.
 *
 * mtimecmp MMIO lives at 0xa0000050..0xa0000058; t1 holds its base
 * throughout so both main and handler reuse the same pointer. */
static void test_interrupt(void) {
    TEST("timer interrupt fires 3 times", 3,
         /* 0x00 */ J(44),                          /* skip handler → main at 0x2c */
         /* 0x04: handler */
         /* 0x04 */ ADDI(S0, S0, 1),                /* counter++ */
         /* 0x08 */ ADDI(T3, ZERO, 3),
         /* 0x0c */ BGE(S0, T3, 16),                /* if counter >= 3: disable-block @ 0x1c */
         /* 0x10 */ SW(ZERO, T1, 0),                /* else: re-arm mtimecmp = 0 (lo) */
         /* 0x14 */ SW(ZERO, T1, 4),                /*              mtimecmp = 0 (hi) */
         /* 0x18 */ JAL(ZERO, 16),                  /* jump over disable-block → 0x28 mret */
         /* 0x1c */ ADDI(T3, ZERO, -1),             /* disable: t3 = 0xffffffff */
         /* 0x20 */ SW(T3, T1, 0),
         /* 0x24 */ SW(T3, T1, 4),
         /* 0x28 */ MRET,
         /* 0x2c: main */
         /* 0x2c */ LUI(T0, 0x80000000),
         /* 0x30 */ ADDI(T0, T0, 4),                /* t0 = handler = 0x80000004 */
         /* 0x34 */ CSRRW(ZERO, CSR_MTVEC, T0),
         /* 0x38 */ ADDI(S0, ZERO, 0),              /* counter = 0 */
         /* 0x3c */ LUI(T1, 0xa0000000),
         /* 0x40 */ ADDI(T1, T1, 0x50),             /* t1 = &mtimecmp */
         /* 0x44 */ SW(ZERO, T1, 0),                /* mtimecmp = 0 → fires on next poll */
         /* 0x48 */ SW(ZERO, T1, 4),
         /* 0x4c */ ADDI(T2, ZERO, 8),              /* MIE bit */
         /* 0x50 */ CSRRS(ZERO, CSR_MSTATUS, T2),   /* mstatus.MIE = 1 */
         /* 0x54 */ ADDI(T2, ZERO, 0x80),           /* MTIE bit */
         /* 0x58 */ CSRRS(ZERO, CSR_MIE, T2),       /* mie.MTIE = 1 — interrupt delivery starts */
         /* 0x5c: loop */
         /* 0x5c */ ADDI(A0, S0, 0),                /* a0 = counter (will become halt_ret) */
         /* 0x60 */ ADDI(T3, ZERO, 3),
         /* 0x64 */ BLT(S0, T3, -8)                 /* while counter < 3: back to 0x5c */
    );
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    temu_path = (argc > 1) ? argv[1] : "./build/temu";
    if (getenv("ISA_VERBOSE"))   verbose = true;
    if (getenv("TEMU_DIFFTEST")) difftest = true;

    test_addi();
    test_logical_immediate();
    test_slt();
    test_shift_immediate();
    test_rtype_arith();
    test_upper_immediate();
    test_x0_hardwired();
    test_branches();
    test_jumps();
    test_load_store();
    test_csr();
    test_trap();
    test_interrupt();

    printf("isa tests: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
