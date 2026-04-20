#include "cpu.h"
#include "local-include/csr.h"
#include "local-include/trap.h"

/* mstatus bit positions — M-mode only for stage 6a. */
#define MSTATUS_MIE_BIT    3
#define MSTATUS_MPIE_BIT   7
#define MSTATUS_MPP_LSB   11
#define MSTATUS_MPP_MASK  (3u << MSTATUS_MPP_LSB)
#define PRIV_M             3u

/* One staging slot. Nested traps before commit are always a bug in
 * stage 6a (we commit once per instruction). */
static struct {
    bool   active;
    word_t cause;
    word_t tval;
    word_t epc;
} pending;

void trap_take(word_t cause, word_t tval, word_t epc) {
    Assert(!pending.active,
           "trap_take: trap already pending (old cause=0x%" PRIx32
           ", new cause=0x%" PRIx32 ")", pending.cause, cause);
    pending.active = true;
    pending.cause  = cause;
    pending.tval   = tval;
    pending.epc    = epc;
}

bool trap_pending(void) { return pending.active; }

void trap_commit(void) {
    Assert(pending.active, "trap_commit: no trap pending");

    csr.mepc   = pending.epc;
    csr.mcause = pending.cause;
    /* mtval lands here in stage 6b when we introduce the register; for
     * now the caller's value is discarded so behaviour is explicit. */
    (void)pending.tval;

    /* mstatus transitions on trap entry (Privileged Spec §3.1.6.1):
     *   MPIE <- MIE
     *   MIE  <- 0
     *   MPP  <- current privilege (always M in stage 6a). */
    word_t s   = csr.mstatus;
    word_t mie = (s >> MSTATUS_MIE_BIT) & 1u;
    s = (s & ~(1u << MSTATUS_MPIE_BIT)) | (mie << MSTATUS_MPIE_BIT);
    s &= ~(1u << MSTATUS_MIE_BIT);
    s = (s & ~MSTATUS_MPP_MASK) | (PRIV_M << MSTATUS_MPP_LSB);
    csr.mstatus = s;

    /* Direct mode only. Low 2 bits of mtvec select vectored vs direct
     * (mode=00 direct); stage 6a ignores them and always jumps to the
     * word-aligned base. */
    cpu.pc = csr.mtvec & ~3u;

    pending.active = false;
}

void trap_mret(word_t *dnpc) {
    *dnpc = csr.mepc;

    /* mstatus transitions on mret (Privileged Spec §3.1.6.1):
     *   MIE  <- MPIE
     *   MPIE <- 1
     *   MPP  <- U in real HW; stage 6a has no U-mode, leave M. */
    word_t s    = csr.mstatus;
    word_t mpie = (s >> MSTATUS_MPIE_BIT) & 1u;
    s = (s & ~(1u << MSTATUS_MIE_BIT)) | (mpie << MSTATUS_MIE_BIT);
    s |= (1u << MSTATUS_MPIE_BIT);
    csr.mstatus = s;
}
