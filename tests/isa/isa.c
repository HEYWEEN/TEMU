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
    snprintf(cmd, sizeof cmd, "%s -b %s 2>/dev/null", temu_path, path);
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

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    temu_path = (argc > 1) ? argv[1] : "./build/temu";
    if (getenv("ISA_VERBOSE")) verbose = true;

    test_addi();
    test_logical_immediate();
    test_slt();
    test_shift_immediate();
    test_rtype_arith();
    test_upper_immediate();
    test_x0_hardwired();
    test_branches();
    test_jumps();

    printf("isa tests: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
