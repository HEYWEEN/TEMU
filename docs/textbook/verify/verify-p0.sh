#!/bin/bash
# verify-p0.sh — 抽取并验证 P0 章节里给出的全部代码片段
#
# 覆盖：
#   §2  toy-cpu.c 能编译、输出 "r1=5 r2=7"
#   §5  common.h + main.c + sdb.c + Makefile 能整体 build
#   §5  REPL 行为：help / q / unknown / 空行 / -h / -b / -l / EOF
set -euo pipefail

WORK=/tmp/textbook-verify/p0
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

# ---------- §2: toy-cpu ----------
echo "[§2] toy-cpu ..."
cat > toy-cpu.c << 'EOF'
#include <stdio.h>
#include <stdint.h>
typedef struct { uint32_t gpr[3]; uint32_t pc; } CPU_state;
#define OP_ADDI 0x01
#define OP_HALT 0xFF
int main(void) {
    CPU_state cpu = {0};
    uint32_t mem[] = { 0x01010005, 0x01020003, 0x01020102, 0xFF000000 };
    while (1) {
        uint32_t inst = mem[cpu.pc / 4];
        uint8_t op = inst >> 24;
        cpu.pc += 4;
        if (op == OP_HALT) break;
        if (op == OP_ADDI) {
            uint8_t dst = (inst >> 16) & 0xff;
            uint8_t src = (inst >>  8) & 0xff;
            uint8_t imm =  inst        & 0xff;
            cpu.gpr[dst] = cpu.gpr[src] + imm;
        }
    }
    printf("r1=%u r2=%u\n", cpu.gpr[1], cpu.gpr[2]);
    return 0;
}
EOF
cc -std=c11 -Wall -Wextra -Werror toy-cpu.c -o toy
out=$(./toy)
[[ "$out" == "r1=5 r2=7" ]] || { echo "FAIL §2: got '$out'"; exit 1; }
echo "  ok"

# ---------- §5: skeleton ----------
echo "[§5] skeleton build ..."
mkdir -p include src/monitor

cat > include/common.h << 'EOF'
#ifndef COMMON_H
#define COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
typedef uint32_t word_t;
typedef int32_t  sword_t;
#define ANSI_FG_RED    "\33[1;31m"
#define ANSI_FG_YELLOW "\33[1;33m"
#define ANSI_NONE      "\33[0m"
#define Log(fmt, ...) \
    fprintf(stderr, ANSI_FG_YELLOW "[%s:%d %s] " fmt ANSI_NONE "\n", \
            __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define Assert(cond, fmt, ...) \
    do { if (!(cond)) { \
        fflush(stdout); \
        fprintf(stderr, ANSI_FG_RED "Assertion failed at %s:%d: " fmt ANSI_NONE "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        assert(cond); \
    }} while (0)
#define panic(fmt, ...) Assert(0, fmt, ##__VA_ARGS__)
#endif
EOF

cat > src/main.c << 'EOF'
#include <getopt.h>
#include "common.h"
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
            case 'b': batch_mode = 1;       break;
            case 'l': log_file   = optarg;  break;
            default:  print_usage(argv[0]); return -1;
        }
    }
    if (optind < argc) image_file = argv[optind];
    return 0;
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
    if (image_file != NULL) Log("would load image from '%s' here", image_file);
    else                    Log("no image provided; starting with empty state");
    if (batch_mode) { Log("batch mode: nothing to run yet (CPU comes in P3)"); return 0; }
    void sdb_mainloop(void);
    sdb_mainloop();
    return 0;
}
EOF

cat > src/monitor/sdb.c << 'EOF'
#include "common.h"
static int cmd_help(char *args);
static int cmd_q   (char *args);
static struct { const char *name; const char *desc; int (*handler)(char *args); }
cmd_table[] = {
    { "help", "Show command list", cmd_help },
    { "q",    "Quit TEMU",         cmd_q    },
};
#define NR_CMD ((int)(sizeof(cmd_table) / sizeof(cmd_table[0])))
static int cmd_help(char *args) {
    (void)args;
    for (int i = 0; i < NR_CMD; i++)
        printf("  %-5s  %s\n", cmd_table[i].name, cmd_table[i].desc);
    return 0;
}
static int cmd_q(char *args) { (void)args; return -1; }
void sdb_mainloop(void) {
    char line[256];
    printf("TEMU — TErminal Machine Emulator\n");
    printf("Type 'help' for commands, 'q' to quit.\n");
    while (1) {
        printf("(temu) "); fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { printf("\n"); break; }
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        char *cmd  = strtok(line, " \t");
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
EOF

cat > Makefile << 'EOF'
CC      ?= clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O0 -Iinclude
SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/temu
SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)
.PHONY: all clean
all: $(TARGET)
$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
-include $(DEPS)
clean:
	rm -rf $(BUILD_DIR)
EOF

make -s 2>&1
echo "  build ok"

# ---------- §5: REPL behavior tests ----------
echo "[§5] REPL behavior ..."
BIN=./build/temu

# help 命令列出 q
out=$(printf 'help\nq\n' | $BIN 2>/dev/null)
echo "$out" | grep -q "Quit TEMU" || { echo "FAIL: help missing 'Quit TEMU'"; exit 1; }

# unknown 命令友好提示
out=$(printf 'foo\nq\n' | $BIN 2>/dev/null)
echo "$out" | grep -q "Unknown command: foo" || { echo "FAIL: no friendly unknown error"; exit 1; }

# 空行不产生 unknown
out=$(printf '\n\nq\n' | $BIN 2>/dev/null)
echo "$out" | grep -q "Unknown" && { echo "FAIL: empty line produced Unknown"; exit 1; } || true

# -h 退出码 0
$BIN -h > /dev/null
[[ $? -eq 0 ]] || { echo "FAIL: -h non-zero exit"; exit 1; }

# -b 不进 REPL
out=$($BIN -b 2>&1)
echo "$out" | grep -q "batch mode" || { echo "FAIL: -b didn't print batch msg"; exit 1; }
echo "$out" | grep -q "TEMU — TErm" && { echo "FAIL: -b still entered REPL"; exit 1; } || true

# -l FILE 把日志写到文件而不是 stderr
rm -f /tmp/textbook-verify/p0.log
$BIN -l /tmp/textbook-verify/p0.log < /dev/null 2>/dev/null
grep -q "no image provided" /tmp/textbook-verify/p0.log || { echo "FAIL: -l didn't redirect log"; exit 1; }

# Ctrl+D clean exit
$BIN < /dev/null 2>/dev/null
[[ $? -eq 0 ]] || { echo "FAIL: EOF non-zero exit"; exit 1; }

echo "  all REPL behaviors ok"
echo ""
echo "P0 verification: PASS"
