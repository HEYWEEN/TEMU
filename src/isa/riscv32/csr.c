#include "local-include/csr.h"

CSR_state csr;

/* Single source of truth: CSR number, printable name, and storage
 * pointer. csr_read / csr_write / csr_dump / csr_lookup all index into
 * this table so adding a new CSR means adding one row. */
typedef struct {
    uint32_t    addr;
    const char *name;
    word_t     *field;
} csr_entry_t;

static csr_entry_t csr_table[] = {
    { CSR_MSTATUS,  "mstatus",  &csr.mstatus  },
    { CSR_MIE,      "mie",      &csr.mie      },
    { CSR_MTVEC,    "mtvec",    &csr.mtvec    },
    { CSR_MSCRATCH, "mscratch", &csr.mscratch },
    { CSR_MEPC,     "mepc",     &csr.mepc     },
    { CSR_MCAUSE,   "mcause",   &csr.mcause   },
    { CSR_MIP,      "mip",      &csr.mip      },
};
#define NR_CSR ((int)(sizeof csr_table / sizeof csr_table[0]))

static csr_entry_t *find_by_addr(uint32_t addr) {
    for (int i = 0; i < NR_CSR; i++) {
        if (csr_table[i].addr == addr) return &csr_table[i];
    }
    return NULL;
}

void csr_init(void) {
    memset(&csr, 0, sizeof csr);
}

word_t csr_read(uint32_t addr) {
    csr_entry_t *e = find_by_addr(addr);
    return e ? *e->field : 0;
}

void csr_write(uint32_t addr, word_t val) {
    csr_entry_t *e = find_by_addr(addr);
    if (e != NULL) *e->field = val;
}

const char *csr_name(uint32_t addr) {
    csr_entry_t *e = find_by_addr(addr);
    return e ? e->name : NULL;
}

void csr_dump(void) {
    for (int i = 0; i < NR_CSR; i++) {
        printf("  %-8s (0x%03" PRIx32 ") = 0x%08" PRIx32 "\n",
               csr_table[i].name,
               csr_table[i].addr,
               *csr_table[i].field);
    }
}

bool csr_lookup(const char *name, word_t *out) {
    for (int i = 0; i < NR_CSR; i++) {
        if (strcmp(name, csr_table[i].name) == 0) {
            *out = *csr_table[i].field;
            return true;
        }
    }
    return false;
}
