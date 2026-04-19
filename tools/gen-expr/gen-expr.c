/*
 * gen-expr — emit N random C / TEMU-compatible expressions, one per line,
 * for differential testing against the TEMU expression evaluator.
 *
 * Operator set (chosen so host C and TEMU agree bit-for-bit on every input):
 *
 *   - Binary:  + - *   &  |  ^   << >>   == != < > <= >=   && ||
 *   - Unary:   - ! ~
 *   - Primary: decimal or hexadecimal literal, parenthesized expression
 *
 * Excluded on purpose:
 *   /  %   — division-by-zero handling differs (TEMU rejects; C is SIGFPE)
 *   *EXPR  — dereference has no pointer analogue in host C
 *   $reg   — registers have no meaning in host C
 *
 * Two knobs keep host C and TEMU in lock-step:
 *
 *   1. Every numeric literal carries a 'u' suffix so the host side treats
 *      it as unsigned int. The fuzz driver strips 'u' before feeding the
 *      same expression to TEMU; the strip is safe because the operator
 *      set contains no other 'u' characters.
 *
 *   2. Every emitted subexpression is wrapped as "(0u + X)". On the host
 *      side this coerces any int-typed intermediate (comparison / logical
 *      result, or a negative value from ~ or unary -) up to unsigned
 *      before it participates in a bitwise op or mixed-type comparison.
 *      After stripping 'u' TEMU sees "(0 + X)", which is the identity
 *      transform for word_t arithmetic.
 *
 * Unary operands are parenthesized so tokens like "- -5" never degenerate
 * into the C decrement operator "--5". Shift right-hand sides are forced
 * to a literal in [0, 31] so the host never hits undefined-behavior "shift
 * by >= width".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE   65536
#define MAX_DEPTH  6

static char buf[BUF_SIZE];
static int  buf_pos;
static int  depth;

static void emit(const char *s) {
    size_t n = strlen(s);
    if (buf_pos + (int)n + 1 >= BUF_SIZE) {
        fprintf(stderr, "gen-expr: buffer overflow\n");
        exit(2);
    }
    memcpy(buf + buf_pos, s, n);
    buf_pos += (int)n;
    buf[buf_pos] = '\0';
}

static void emit_char(char c) {
    if (buf_pos + 1 >= BUF_SIZE) {
        fprintf(stderr, "gen-expr: buffer overflow\n");
        exit(2);
    }
    buf[buf_pos++] = c;
    buf[buf_pos]   = '\0';
}

static void emit_spaces(void) {
    int n = rand() % 3;
    while (n-- > 0) emit_char(' ');
}

static void emit_num(void) {
    char tmp[32];
    if (rand() % 3 == 0) {
        snprintf(tmp, sizeof tmp, "0x%xu", (unsigned)(rand() & 0xffff));
    } else {
        snprintf(tmp, sizeof tmp, "%uu", (unsigned)(rand() % 100));
    }
    emit(tmp);
}

static void emit_shift_amount(void) {
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%uu", (unsigned)(rand() % 32));
    emit(tmp);
}

static const char *BINOPS[] = {
    "+", "-", "*",
    "&", "|", "^",
    "==", "!=", "<", ">", "<=", ">=",
    "&&", "||",
};
#define NR_BINOPS ((int)(sizeof(BINOPS) / sizeof(BINOPS[0])))

static const char *UNOPS[] = { "-", "!", "~" };
#define NR_UNOPS ((int)(sizeof(UNOPS) / sizeof(UNOPS[0])))

static void gen_expr(void);

static void gen_shift(void) {
    emit_char('(');
    emit_spaces();
    gen_expr();
    emit_spaces();
    emit(rand() % 2 ? "<<" : ">>");
    emit_spaces();
    emit_shift_amount();
    emit_spaces();
    emit_char(')');
}

static void gen_binop_expr(void) {
    emit_char('(');
    emit_spaces();
    gen_expr();
    emit_spaces();
    emit(BINOPS[rand() % NR_BINOPS]);
    emit_spaces();
    gen_expr();
    emit_spaces();
    emit_char(')');
}

static void gen_unop_expr(void) {
    emit(UNOPS[rand() % NR_UNOPS]);
    emit_char('(');
    emit_spaces();
    gen_expr();
    emit_spaces();
    emit_char(')');
}

/* Every gen_expr call emits its output wrapped in "(0u + ...)". See the
 * file-level comment for why. */
static void gen_expr(void) {
    if (depth >= MAX_DEPTH || rand() % 4 == 0) {
        emit("(0u + ");
        emit_num();
        emit_char(')');
        return;
    }
    depth++;
    emit("(0u + ");
    int r = rand() % 10;
    if      (r < 1) emit_num();
    else if (r < 3) gen_unop_expr();
    else if (r < 4) gen_shift();
    else            gen_binop_expr();
    emit_char(')');
    depth--;
}

int main(int argc, char *argv[]) {
    int n = 200;
    if (argc > 1) n = atoi(argv[1]);
    if (n <= 0)   n = 200;

    srand((unsigned)(time(NULL) ^ getpid()));

    for (int i = 0; i < n; i++) {
        buf_pos = 0;
        buf[0]  = '\0';
        depth   = 0;
        gen_expr();
        puts(buf);
    }
    return 0;
}
