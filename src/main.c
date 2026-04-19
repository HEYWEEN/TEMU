#include <getopt.h>
#include <unistd.h>

#include "common.h"
#include "cpu.h"
#include "difftest.h"
#include "memory.h"
#include "monitor.h"

static int         batch_mode = 0;
static int         diff_mode  = 0;
static const char *log_file   = NULL;
static const char *image_file = NULL;
static const char *test_file  = NULL;

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTION]... [IMAGE]\n", prog);
    printf("  -b           run in batch mode (no REPL)\n");
    printf("  -d           enable differential testing against the reference CPU\n");
    printf("  -l FILE      write log to FILE\n");
    printf("  -t FILE      run expression tests from FILE and exit\n");
    printf("  -h           show this help and exit\n");
}

static int parse_args(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "hbdl:t:")) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 1;
            case 'b': batch_mode = 1; break;
            case 'd': diff_mode  = 1; break;
            case 'l': log_file  = optarg; break;
            case 't': test_file = optarg; break;
            default:  print_usage(argv[0]); return -1;
        }
    }
    if (optind < argc) image_file = argv[optind];
    return 0;
}

/* Read a test file of "<expected>  <expression>" lines. '#' introduces
 * a comment; blank lines are skipped. Returns 0 if all tests passed. */
static int run_expr_tests(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "cannot open test file '%s'\n", path);
        return 2;
    }

    char line[8192];
    int  pass = 0, fail = 0, lineno = 0;

    while (fgets(line, sizeof line, fp)) {
        lineno++;

        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        char *end;
        word_t expected = (word_t)strtoul(p, &end, 0);
        if (end == p) {
            fprintf(stderr, "L%d: FAIL: cannot parse expected value\n", lineno);
            fail++; continue;
        }
        while (*end == ' ' || *end == '\t') end++;
        if (*end == '\0') {
            fprintf(stderr, "L%d: FAIL: missing expression\n", lineno);
            fail++; continue;
        }

        bool ok = false;
        word_t got = expr(end, &ok);
        if (!ok) {
            fprintf(stderr, "L%d: FAIL (eval error): %s\n", lineno, end);
            fail++;
        } else if (got != expected) {
            fprintf(stderr,
                    "L%d: FAIL: %s  => got %" PRIu32 " (0x%" PRIx32
                    "), expected %" PRIu32 " (0x%" PRIx32 ")\n",
                    lineno, end, got, got, expected, expected);
            fail++;
        } else {
            pass++;
        }
    }
    fclose(fp);

    if (fail == 0)
        printf("expr tests: %d passed, 0 failed \xE2\x9C\x93\n", pass);
    else
        printf("expr tests: %d passed, %d FAILED\n", pass, fail);

    return fail == 0 ? 0 : 1;
}

int main(int argc, char *argv[]) {
    int r = parse_args(argc, argv);
    if (r != 0) return r < 0 ? 1 : 0;

    if (log_file != NULL) {
        if (freopen(log_file, "w", stderr) == NULL) {
            fprintf(stderr, "cannot open log file '%s'\n", log_file);
            return 1;
        }
    }

    cpu_init();

    if (test_file != NULL) {
        /* Tests run against the post-cpu_init state: PC = RESET_VECTOR,
         * pmem all-zero. No image is loaded so *EXPR against a valid
         * in-pmem address returns zero. */
        return run_expr_tests(test_file);
    }

    if (image_file != NULL) {
        load_img(image_file);
    } else {
        Log("no image provided; pmem starts as all-zero bytes");
    }

    if (diff_mode) {
        difftest_enable(true);
        difftest_init();
    }

    if (batch_mode) {
        cpu_exec((uint64_t)-1);
        switch (temu_state()) {
            case TEMU_END:   return 0;
            case TEMU_ABORT: return 1;
            default:         return 2;   /* shouldn't reach here after infinite cpu_exec */
        }
    }

    sdb_mainloop();
    return 0;
}
