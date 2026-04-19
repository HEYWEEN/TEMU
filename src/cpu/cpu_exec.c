#include "common.h"
#include "cpu.h"
#include "difftest.h"
#include "memory.h"
#include "monitor.h"

/* ------------------------------------------------------------------ */
/* Execution state                                                     */
/* ------------------------------------------------------------------ */

static struct {
    temu_state_t state;
    vaddr_t      halt_pc;
    word_t       halt_ret;
} g = { .state = TEMU_STOP };

temu_state_t temu_state(void) { return g.state; }

void temu_set_end(vaddr_t pc, word_t halt_ret) {
    g.state    = TEMU_END;
    g.halt_pc  = pc;
    g.halt_ret = halt_ret;
}

void temu_set_abort(void) {
    g.state = TEMU_ABORT;
}

/* ------------------------------------------------------------------ */
/* Instruction trace: last N {pc, inst} pairs, dumped on abort / end. */
/* ------------------------------------------------------------------ */

#define ITRACE_SIZE 16

static struct {
    vaddr_t  pc;
    uint32_t inst;
} itrace_ring[ITRACE_SIZE];
static uint64_t itrace_head = 0;

static void itrace_record(vaddr_t pc, uint32_t inst) {
    size_t idx = (size_t)(itrace_head % ITRACE_SIZE);
    itrace_ring[idx].pc   = pc;
    itrace_ring[idx].inst = inst;
    itrace_head++;
}

static void itrace_dump(void) {
    uint64_t count = (itrace_head < ITRACE_SIZE) ? itrace_head : ITRACE_SIZE;
    uint64_t start = itrace_head - count;
    fprintf(stderr, "--- itrace (last %" PRIu64 " instructions) ---\n", count);
    for (uint64_t i = 0; i < count; i++) {
        size_t idx = (size_t)((start + i) % ITRACE_SIZE);
        fprintf(stderr, "  0x%08" PRIx32 ":  %08" PRIx32 "\n",
                itrace_ring[idx].pc, itrace_ring[idx].inst);
    }
}

/* ------------------------------------------------------------------ */
/* Main execution loop                                                 */
/* ------------------------------------------------------------------ */

static void exec_once(void) {
    Decode s;
    s.pc   = cpu.pc;
    s.snpc = cpu.pc + 4;
    s.dnpc = s.snpc;
    s.inst = (uint32_t)paddr_read(s.pc, 4);

    itrace_record(s.pc, s.inst);

    isa_exec_once(&s);

    /* Only advance PC if the instruction completed normally. On END or
     * ABORT we leave cpu.pc pointing at the offending / halt
     * instruction so `info r` is useful to the user. */
    if (g.state == TEMU_RUNNING) {
        cpu.pc = s.dnpc;
    }
    cpu.gpr[0] = 0;   /* x0 is hard-wired to zero */

    /* Difftest compares against the reference CPU. Skip on abort so
     * the ref does not re-emit confusing errors for the same
     * instruction the main side already rejected. */
    if (difftest_is_enabled() && g.state != TEMU_ABORT) {
        difftest_step();
    }
}

void cpu_exec(uint64_t n) {
    switch (g.state) {
        case TEMU_END:
        case TEMU_ABORT:
            printf("cpu is already halted (state=%s). Quit and restart to run again.\n",
                   g.state == TEMU_END ? "END" : "ABORT");
            return;
        default:
            break;
    }

    g.state = TEMU_RUNNING;

    for (uint64_t i = 0; i < n; i++) {
        exec_once();
        if (g.state != TEMU_RUNNING) break;
        if (wp_check()) {
            g.state = TEMU_STOP;
            break;
        }
    }

    if (g.state == TEMU_RUNNING) {
        /* step count exhausted but no termination */
        g.state = TEMU_STOP;
    }

    switch (g.state) {
        case TEMU_END:
            printf("\33[1;32mHIT END\33[0m  pc=0x%08" PRIx32
                   "  halt_ret=0x%08" PRIx32 " (%" PRId32 ")\n",
                   g.halt_pc, g.halt_ret, (sword_t)g.halt_ret);
            break;
        case TEMU_ABORT:
            printf("\33[1;31mHIT ABORT\33[0m at pc=0x%08" PRIx32 "\n", cpu.pc);
            itrace_dump();
            break;
        case TEMU_STOP:
            /* quiet — the REPL will reprompt the user */
            break;
        default:
            break;
    }
}
