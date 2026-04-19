#include "common.h"
#include "cpu.h"
#include "memory.h"
#include "monitor.h"

/* ------------------------------------------------------------------ */
/* Command handlers (forward declared so cmd_table can point to them) */
/* ------------------------------------------------------------------ */

static int cmd_help(char *args);
static int cmd_c   (char *args);
static int cmd_si  (char *args);
static int cmd_info(char *args);
static int cmd_x   (char *args);
static int cmd_p   (char *args);
static int cmd_w   (char *args);
static int cmd_d   (char *args);
static int cmd_q   (char *args);

static struct {
    const char *name;
    const char *desc;
    int (*handler)(char *args);
} cmd_table[] = {
    { "help", "Show command list",                         cmd_help },
    { "c",    "Continue execution  (stage 3+ only)",       cmd_c    },
    { "si",   "si [N] : single-step N (stage 3+ only)",    cmd_si   },
    { "info", "info r | info w : regs or watchpoints",     cmd_info },
    { "x",    "x N EXPR : examine N words starting at EXPR", cmd_x  },
    { "p",    "p EXPR   : evaluate and print expression",  cmd_p    },
    { "w",    "w EXPR   : add watchpoint on EXPR",         cmd_w    },
    { "d",    "d N      : delete watchpoint N",            cmd_d    },
    { "q",    "Quit TEMU",                                 cmd_q    },
};
#define NR_CMD ((int)(sizeof(cmd_table) / sizeof(cmd_table[0])))

/* ------------------------------------------------------------------ */
/* Individual commands                                                 */
/* ------------------------------------------------------------------ */

static int cmd_help(char *args) {
    (void)args;
    for (int i = 0; i < NR_CMD; i++) {
        printf("  %-5s  %s\n", cmd_table[i].name, cmd_table[i].desc);
    }
    return 0;
}

static int cmd_q(char *args) {
    (void)args;
    return -1;
}

static int cmd_c(char *args) {
    (void)args;
    cpu_exec((uint64_t)-1);
    return 0;
}

static int cmd_si(char *args) {
    uint64_t n = 1;
    if (args != NULL) {
        char *tok = strtok(args, " \t");
        if (tok != NULL) {
            char *end;
            long v = strtol(tok, &end, 0);
            if (*end != '\0' || v <= 0) {
                printf("si: bad count '%s'\n", tok);
                return 0;
            }
            n = (uint64_t)v;
        }
    }
    cpu_exec(n);
    return 0;
}

static void print_regs(void) {
    for (int i = 0; i < 32; i++) {
        printf("  %-4s (x%-2d) = 0x%08" PRIx32 "  %" PRId32 "\n",
               reg_name(i), i, cpu.gpr[i], (sword_t)cpu.gpr[i]);
    }
    printf("  pc         = 0x%08" PRIx32 "\n", cpu.pc);
}

static int cmd_info(char *args) {
    if (args == NULL) {
        printf("usage: info r | info w\n");
        return 0;
    }
    char *sub = strtok(args, " \t");
    if (sub == NULL) {
        printf("usage: info r | info w\n");
    } else if (strcmp(sub, "r") == 0) {
        print_regs();
    } else if (strcmp(sub, "w") == 0) {
        wp_display();
    } else {
        printf("info: unknown subcommand '%s'\n", sub);
    }
    return 0;
}

static int cmd_p(char *args) {
    if (args == NULL) {
        printf("usage: p EXPR\n");
        return 0;
    }
    bool ok = false;
    word_t v = expr(args, &ok);
    if (!ok) {
        printf("p: cannot evaluate expression\n");
    } else {
        printf("  = %" PRIu32 " (0x%08" PRIx32 ")\n", v, v);
    }
    return 0;
}

static int cmd_x(char *args) {
    if (args == NULL) {
        printf("usage: x N EXPR\n");
        return 0;
    }
    char *n_str = strtok(args, " \t");
    char *e_str = strtok(NULL, "");
    if (n_str == NULL || e_str == NULL) {
        printf("usage: x N EXPR\n");
        return 0;
    }
    char *end;
    long n = strtol(n_str, &end, 0);
    if (*end != '\0' || n <= 0) {
        printf("x: bad count '%s'\n", n_str);
        return 0;
    }

    bool ok = false;
    word_t base = expr(e_str, &ok);
    if (!ok) {
        printf("x: cannot evaluate '%s'\n", e_str);
        return 0;
    }
    for (long i = 0; i < n; i++) {
        paddr_t addr = base + (paddr_t)(i * 4);
        if (i % 4 == 0) {
            if (i != 0) printf("\n");
            printf("0x%08" PRIx32 ":", addr);
        }
        printf(" 0x%08" PRIx32, paddr_read(addr, 4));
    }
    printf("\n");
    return 0;
}

static int cmd_w(char *args) {
    if (args == NULL || *args == '\0') {
        printf("usage: w EXPR\n");
        return 0;
    }
    wp_add(args);
    return 0;
}

static int cmd_d(char *args) {
    if (args == NULL) {
        printf("usage: d N\n");
        return 0;
    }
    char *end;
    long no = strtol(args, &end, 0);
    if (*end != '\0' && *end != ' ' && *end != '\t') {
        printf("d: bad id '%s'\n", args);
        return 0;
    }
    if (!wp_del((int)no)) {
        printf("d: no watchpoint #%ld\n", no);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* REPL                                                                */
/* ------------------------------------------------------------------ */

static void banner(void) {
    printf("TEMU — TErminal Machine Emulator (RV32I)\n");
    printf("pmem: 0x%08" PRIx32 " .. 0x%08" PRIx32 " (%d MB)\n",
           (paddr_t)PMEM_BASE,
           (paddr_t)(PMEM_BASE + PMEM_SIZE),
           PMEM_SIZE / (1024 * 1024));
    printf("pc:   0x%08" PRIx32 "\n", cpu.pc);
    printf("Type 'help' for commands, 'q' to quit.\n");
}

void sdb_mainloop(void) {
    char line[256];
    banner();
    while (1) {
        printf("(temu) ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            printf("\n");
            break;
        }

        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';

        char *cmd = strtok(line, " \t");
        if (cmd == NULL) continue;
        char *args = strtok(NULL, "");

        int found = 0;
        for (int i = 0; i < NR_CMD; i++) {
            if (strcmp(cmd, cmd_table[i].name) == 0) {
                found = 1;
                if (cmd_table[i].handler(args) < 0) return;
                break;
            }
        }
        if (!found) printf("Unknown command: %s (try 'help')\n", cmd);
    }
}
