#include <getopt.h>
#include <unistd.h>

#include "common.h"
#include "monitor.h"

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

int main(int argc, char *argv[]) {
    int r = parse_args(argc, argv);
    if (r != 0) return r < 0 ? 1 : 0;

    if (batch_mode) {
        printf("[batch mode] image=%s log=%s (CPU not yet implemented)\n",
               image_file ? image_file : "<none>",
               log_file   ? log_file   : "<none>");
        return 0;
    }

    sdb_mainloop();
    return 0;
}
