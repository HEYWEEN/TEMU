#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

static int         batch_mode = 0;
static const char *log_file   = NULL;
static const char *image_file = NULL;

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTION]... [IMAGE]\n", prog);
    printf("  -b           run in batch mode (no REPL)\n");
    printf("  -l FILE      write log to FILE\n");
    printf("  -h           show this help and exit\n");
}

static int parse_args(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "hbl:")) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 1;
            case 'b': batch_mode = 1; break;
            case 'l': log_file = optarg; break;
            default:  print_usage(argv[0]); return -1;
        }
    }
    if (optind < argc) image_file = argv[optind];
    return 0;
}

typedef struct {
    const char *name;
    const char *desc;
    int (*handler)(char *args);
} cmd_t;

static int do_help(char *args);
static int do_q(char *args);

static cmd_t cmd_table[] = {
    { "help", "Show command list",        do_help },
    { "q",    "Quit TEMU",                do_q    },
};
#define NR_CMD (sizeof(cmd_table) / sizeof(cmd_table[0]))

static int do_help(char *args) {
    (void)args;
    for (size_t i = 0; i < NR_CMD; i++) {
        printf("  %-6s  %s\n", cmd_table[i].name, cmd_table[i].desc);
    }
    return 0;
}

static int do_q(char *args) {
    (void)args;
    return -1;
}

static void banner(void) {
    printf("TEMU — TErminal Machine Emulator (RV32I)\n");
    printf("Stage 0: skeleton REPL. Type 'help' for commands, 'q' to quit.\n");
    if (image_file) printf("  image: %s\n", image_file);
    if (log_file)   printf("  log:   %s\n", log_file);
}

static void repl(void) {
    char line[256];
    banner();
    while (1) {
        printf("(temu) ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';

        char *cmd = strtok(line, " \t");
        if (!cmd) continue;
        char *args = strtok(NULL, "");

        int found = 0;
        for (size_t i = 0; i < NR_CMD; i++) {
            if (strcmp(cmd, cmd_table[i].name) == 0) {
                found = 1;
                if (cmd_table[i].handler(args) < 0) return;
                break;
            }
        }
        if (!found) printf("Unknown command: %s\n", cmd);
    }
}

int main(int argc, char *argv[]) {
    int r = parse_args(argc, argv);
    if (r != 0) return r < 0 ? 1 : 0;

    if (batch_mode) {
        printf("[batch mode] image=%s log=%s (CPU not yet implemented)\n",
               image_file ? image_file : "<none>",
               log_file   ? log_file   : "<none>");
        return 0;
    }

    repl();
    return 0;
}
