/*
 * prog.c — end-to-end program-level tests for the RV32I CPU.
 *
 * Each test hand-assembles a small program using isa-encoder.h, runs
 * it through temu, and asserts halt_ret == expected. The programs
 * here exercise real instruction *compositions* that the per-
 * instruction tests in isa.c cannot: loops, recursion, stack use.
 *
 * This file intentionally duplicates the tiny check() / TEST harness
 * from tests/isa/isa.c instead of sharing one — the duplication is
 * ten lines and keeps the test binaries self-contained.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../isa/isa-encoder.h"

static int  pass_count = 0;
static int  fail_count = 0;
static bool difftest   = false;
static const char *temu_path;

static void check(const char *name, uint32_t expected_ret,
                  const char *expected_substr,
                  const uint32_t *prog, size_t n) {
    char path[] = "/tmp/temu-prog-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    size_t bytes = n * sizeof(uint32_t);
    if (write(fd, prog, bytes) != (ssize_t)bytes) {
        perror("write"); close(fd); unlink(path); exit(2);
    }
    close(fd);

    /* 2>&1: program stdout (serial putchar) and temu's own status
     * lines are both captured into `pipe`. We search each line for
     * both the halt_ret marker and the optional expected substring. */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s -b%s %s 2>&1",
             temu_path, difftest ? "d" : "", path);
    FILE *pipe = popen(cmd, "r");
    if (pipe == NULL) { perror("popen"); unlink(path); exit(2); }

    char line[512];
    bool     found = false, aborted = false;
    bool     substr_found = (expected_substr == NULL);
    uint32_t got = 0;
    while (fgets(line, sizeof line, pipe)) {
        if (strstr(line, "HIT ABORT"))   aborted = true;
        const char *m = strstr(line, "halt_ret=0x");
        if (m) { got = (uint32_t)strtoul(m + 11, NULL, 16); found = true; }
        if (expected_substr && strstr(line, expected_substr)) {
            substr_found = true;
        }
    }
    int rc = pclose(pipe);
    unlink(path);

    if (aborted) {
        printf("  %-32s FAIL (HIT ABORT)\n", name);
        fail_count++;
    } else if (!found) {
        printf("  %-32s FAIL (no halt_ret, rc=%d)\n", name, rc);
        fail_count++;
    } else if (got != expected_ret) {
        printf("  %-32s FAIL (got 0x%08x, expected 0x%08x)\n",
               name, got, expected_ret);
        fail_count++;
    } else if (!substr_found) {
        printf("  %-32s FAIL (stdout did not contain '%s')\n",
               name, expected_substr);
        fail_count++;
    } else {
        pass_count++;
    }
}

#define RUN(name, expected, ...) do {                        \
    uint32_t prog_[] = { __VA_ARGS__, EBREAK };              \
    check(name, (uint32_t)(expected), NULL,                  \
          prog_, sizeof prog_ / sizeof prog_[0]);            \
} while (0)

#define RUN_OUT(name, expected, out_substr, ...) do {        \
    uint32_t prog_[] = { __VA_ARGS__, EBREAK };              \
    check(name, (uint32_t)(expected), (out_substr),          \
          prog_, sizeof prog_ / sizeof prog_[0]);            \
} while (0)

/* ------------------------------------------------------------------ */
/* Program 1: iterative Fibonacci — fib(10) = 55                       */
/*                                                                     */
/*   addi a0, x0, 10          ; n = 10                                 */
/*   addi t0, x0, 0            ; a = 0                                 */
/*   addi t1, x0, 1            ; b = 1                                 */
/* loop:                                                               */
/*   beq  a0, x0, done         ; if n == 0 jump to done  (+24)         */
/*   add  t2, t0, t1            ; t = a + b                            */
/*   add  t0, x0, t1            ; a = b                                */
/*   add  t1, x0, t2            ; b = t                                */
/*   addi a0, a0, -1            ; n -= 1                               */
/*   jal  x0, loop              ; unconditional (-20)                  */
/* done:                                                               */
/*   add  a0, x0, t0            ; a0 = a (return value)                */
/*   ebreak                                                            */
/* ------------------------------------------------------------------ */
static void test_iterative_fib(void) {
    RUN("iterative fib(10) = 55", 55,
        ADDI(A0, ZERO, 10),
        ADDI(T0, ZERO, 0),
        ADDI(T1, ZERO, 1),
        BEQ(A0, ZERO, 24),          /* loop:  skip to done */
        ADD(T2, T0, T1),
        ADD(T0, ZERO, T1),
        ADD(T1, ZERO, T2),
        ADDI(A0, A0, -1),
        JAL(ZERO, -20),             /* back to loop */
        ADD(A0, ZERO, T0));         /* done: a0 = a */
}

/* ------------------------------------------------------------------ */
/* Program 2: sum 1..N via countdown — verifies loop-carried state     */
/*            through ADD and ADDI, plus BNE backward.                 */
/*   sum(10) = 55                                                      */
/* ------------------------------------------------------------------ */
static void test_sum_1_to_n(void) {
    RUN("sum(1..10) = 55", 55,
        ADDI(T0, ZERO, 10),          /* counter */
        ADDI(A0, ZERO, 0),            /* accumulator */
        ADD(A0, A0, T0),              /* loop: a0 += t0 */
        ADDI(T0, T0, -1),
        BNE(T0, ZERO, -8));           /* jump to ADD */
}

/* ------------------------------------------------------------------ *
 * Program 3: recursive Fibonacci — fib(6) = 8                         *
 *                                                                     *
 * Classic calling convention with a small stack in pmem. sp starts at *
 * pmem's high end (0x88000000 - 16) and grows down. We save ra and    *
 * a0 across the recursive call.                                       *
 *                                                                     *
 *   addi a0, x0, 6            ; initial n                             *
 *   lui  sp, 0x87ffe000        ; sp = 0x87ffe000 (well inside pmem)   *
 *   jal  ra, fib               ;                                      *
 *   jal  x0, end               ; unconditional over the function body *
 * fib:                                                                *
 *   addi t0, x0, 2                                                    *
 *   blt  a0, t0, base_case     ; if n < 2, return n                   *
 *   addi sp, sp, -12                                                  *
 *   sw   ra, 0(sp)                                                    *
 *   sw   a0, 4(sp)             ; save n                               *
 *   addi a0, a0, -1             ; arg = n - 1                         *
 *   jal  ra, fib                                                      *
 *   sw   a0, 8(sp)             ; save fib(n-1)                        *
 *   lw   a0, 4(sp)             ; restore n                            *
 *   addi a0, a0, -2             ; arg = n - 2                         *
 *   jal  ra, fib                                                      *
 *   lw   t1, 8(sp)                                                    *
 *   add  a0, a0, t1             ; a0 = fib(n-1) + fib(n-2)            *
 *   lw   ra, 0(sp)                                                    *
 *   addi sp, sp, 12                                                   *
 *   jalr x0, ra, 0             ; return                               *
 * base_case:                                                          *
 *   jalr x0, ra, 0             ; return n                             *
 * end:                                                                *
 *   ebreak                                                            *
 * ------------------------------------------------------------------ */
static void test_recursive_fib(void) {
    RUN("recursive fib(6) = 8", 8,
        /* [0] 0x00 */ ADDI(A0, ZERO, 6),
        /* [1] 0x04 */ LUI(SP, 0x87ffe000),
        /* [2] 0x08 */ JAL(RA, 8),                 /* call fib at 0x10 */
        /* [3] 0x0c */ JAL(ZERO, 72),              /* end = 0x54; 0x54 - 0x0c = 72 */
        /* --- fib: 0x10 --- */
        /* [4] 0x10 */ ADDI(T0, ZERO, 2),
        /* [5] 0x14 */ BLT(A0, T0, 60),            /* -> base_case at 0x50; 0x50-0x14=60 */
        /* [6] 0x18 */ ADDI(SP, SP, -12),
        /* [7] 0x1c */ SW(RA, SP, 0),
        /* [8] 0x20 */ SW(A0, SP, 4),
        /* [9] 0x24 */ ADDI(A0, A0, -1),
        /* [10] 0x28 */ JAL(RA, -24),              /* recurse; 0x10-0x28 = -24 */
        /* [11] 0x2c */ SW(A0, SP, 8),
        /* [12] 0x30 */ LW(A0, SP, 4),
        /* [13] 0x34 */ ADDI(A0, A0, -2),
        /* [14] 0x38 */ JAL(RA, -40),              /* recurse again; 0x10-0x38 = -40 */
        /* [15] 0x3c */ LW(T1, SP, 8),
        /* [16] 0x40 */ ADD(A0, A0, T1),
        /* [17] 0x44 */ LW(RA, SP, 0),
        /* [18] 0x48 */ ADDI(SP, SP, 12),
        /* [19] 0x4c */ JALR(ZERO, RA, 0),
        /* --- base_case: 0x50 --- */
        /* [20] 0x50 */ JALR(ZERO, RA, 0));
    /* [21] 0x54 = end  — EBREAK appended by RUN */
}

/* ------------------------------------------------------------------ */
/* Program 4: array sum via LW in a loop — verifies load in a loop.    */
/*                                                                     */
/* Program layout: four SW instructions write 10,20,30,40 into scratch *
 * pmem, then a loop sums those four words. Expected: 100.             */
/* ------------------------------------------------------------------ */
static void test_array_sum(void) {
    RUN("array sum 10+20+30+40 = 100", 100,
        LUI(T0, 0x80001000),        /* t0 = base address */
        ADDI(T1, ZERO, 10),
        SW(T1, T0, 0),
        ADDI(T1, ZERO, 20),
        SW(T1, T0, 4),
        ADDI(T1, ZERO, 30),
        SW(T1, T0, 8),
        ADDI(T1, ZERO, 40),
        SW(T1, T0, 12),

        ADDI(T2, ZERO, 4),          /* counter    */
        ADDI(A0, ZERO, 0),          /* sum        */
        ADDI(T1, ZERO, 0),          /* byte offset */

        ADD(T3, T0, T1),            /* loop: addr = base + off */
        LW(T4, T3, 0),
        ADD(A0, A0, T4),
        ADDI(T1, T1, 4),
        ADDI(T2, T2, -1),
        BNE(T2, ZERO, -20));        /* back to `add t3, t0, t1` */
}

/* ------------------------------------------------------------------ */
/* Program 5: serial "hello\n" — exercises MMIO. The loop writes each  *
 * character of the string to the serial register at 0xa00003f8.       *
 *                                                                     */
/* a0 holds the SERIAL address after the LUI+ADDI; subsequent SB       *
 * stores drop the low byte onto stdout. We clear a0 back to zero      *
 * before EBREAK so halt_ret is a tidy 0.                              *
 * ------------------------------------------------------------------ */
static void test_serial_hello(void) {
    RUN_OUT("serial hello", 0, "hello",
        /* a0 = 0xa00003f8 */
        LUI(A0, 0xa0000000),
        ADDI(A0, A0, 0x3f8),
        /* h e l l o \n */
        ADDI(T0, ZERO, 'h'), SB(T0, A0, 0),
        ADDI(T0, ZERO, 'e'), SB(T0, A0, 0),
        ADDI(T0, ZERO, 'l'), SB(T0, A0, 0),
        ADDI(T0, ZERO, 'l'), SB(T0, A0, 0),
        ADDI(T0, ZERO, 'o'), SB(T0, A0, 0),
        ADDI(T0, ZERO, '\n'), SB(T0, A0, 0),
        /* clean halt_ret */
        ADDI(A0, ZERO, 0));
}

/* ------------------------------------------------------------------ */
/* Program 6: timer monotonicity. Read the microsecond counter twice  *
 * and confirm the second read is >= the first. Returns 1 on success, *
 * 0 on failure — so a pass is halt_ret == 1 regardless of how much   *
 * real time actually elapsed (the delta may well be zero on a fast   *
 * host).                                                             */
/* ------------------------------------------------------------------ */
static void test_timer_monotonic(void) {
    RUN("timer monotonic", 1,
        /* t2 = 0xa0000048 (timer low word) */
        LUI(T2, 0xa0000000),
        ADDI(T2, T2, 0x48),

        LW(T0, T2, 0),                 /* first read          */
        LW(T1, T2, 0),                 /* second read         */

        BGEU(T1, T0, 12),              /* if t1 >= t0, skip fail arm */
        ADDI(A0, ZERO, 0),
        JAL(ZERO, 8),
        ADDI(A0, ZERO, 1));
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    temu_path = (argc > 1) ? argv[1] : "./build/temu";
    if (getenv("TEMU_DIFFTEST")) difftest = true;

    test_iterative_fib();
    test_sum_1_to_n();
    test_recursive_fib();
    test_array_sum();
    test_serial_hello();
    test_timer_monotonic();

    printf("program tests: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
