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
typedef uint32_t paddr_t;
typedef uint32_t vaddr_t;

#define FMT_WORD  "0x%08" PRIx32
#define FMT_PADDR "0x%08" PRIx32

#define ANSI_FG_RED    "\33[1;31m"
#define ANSI_FG_GREEN  "\33[1;32m"
#define ANSI_FG_YELLOW "\33[1;33m"
#define ANSI_NONE      "\33[0m"

#define Log(fmt, ...)                                                   \
    fprintf(stderr, ANSI_FG_YELLOW "[%s:%d %s] " fmt ANSI_NONE "\n",    \
            __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#define Assert(cond, fmt, ...)                                          \
    do {                                                                \
        if (!(cond)) {                                                  \
            fflush(stdout);                                             \
            fprintf(stderr, ANSI_FG_RED "Assertion failed at %s:%d: "   \
                    fmt ANSI_NONE "\n",                                 \
                    __FILE__, __LINE__, ##__VA_ARGS__);                 \
            assert(cond);                                               \
        }                                                               \
    } while (0)

#define panic(fmt, ...) Assert(0, fmt, ##__VA_ARGS__)
#define TODO()          panic("please implement me")

#endif /* COMMON_H */
