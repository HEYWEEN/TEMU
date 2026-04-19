#include <regex.h>

#include "common.h"
#include "cpu.h"
#include "memory.h"
#include "monitor.h"

/* ------------------------------------------------------------------ */
/* 1. Lexer: regex rules + fix pass for unary '*' / '-'                */
/* ------------------------------------------------------------------ */

enum {
    TK_NOTYPE = 256,
    TK_NUM, TK_HEXNUM, TK_REG,
    TK_EQ, TK_NEQ, TK_LE, TK_GE,
    TK_AND, TK_OR,
    TK_SHL, TK_SHR,
    TK_DEREF, TK_NEG,
};

#define MAX_TOKENS     512
#define MAX_TOKEN_STR  32

typedef struct {
    int  type;
    char str[MAX_TOKEN_STR];
} Token;

static Token tokens[MAX_TOKENS];
static int   nr_token;

/* Order matters: longer / more-specific rules come first so the regex
 * engine picks them before a shorter prefix match. */
static struct rule {
    const char *re;
    int         type;
} rules[] = {
    { "[ \t]+",                    TK_NOTYPE },
    { "0[xX][0-9a-fA-F]+",         TK_HEXNUM },   /* must precede NUM  */
    { "[0-9]+",                    TK_NUM    },
    { "\\$[a-zA-Z_][a-zA-Z_0-9]*", TK_REG    },

    { "==", TK_EQ  }, { "!=", TK_NEQ },           /* must precede '!'  */
    { "<=", TK_LE  }, { ">=", TK_GE  },           /* must precede <,>  */
    { "&&", TK_AND }, { "\\|\\|", TK_OR },
    { "<<", TK_SHL }, { ">>", TK_SHR },           /* must precede <,>  */

    { "\\+", '+' }, { "-", '-' },
    { "\\*", '*' }, { "/", '/' }, { "%", '%' },
    { "<",  '<' }, { ">",  '>' },
    { "&",  '&' }, { "\\|", '|' }, { "\\^", '^' },
    { "~",  '~' }, { "!",  '!' },
    { "\\(", '(' }, { "\\)", ')' },
};
#define NR_RULES ((int)(sizeof(rules) / sizeof(rules[0])))

static regex_t re_compiled[NR_RULES];

static void init_regex(void) {
    static bool done = false;
    if (done) return;
    for (int i = 0; i < NR_RULES; i++) {
        int ret = regcomp(&re_compiled[i], rules[i].re, REG_EXTENDED);
        if (ret != 0) {
            char buf[128];
            regerror(ret, &re_compiled[i], buf, sizeof buf);
            panic("expr: cannot compile regex '%s': %s", rules[i].re, buf);
        }
    }
    done = true;
}

static bool is_operand_tok(int t) {
    return t == TK_NUM || t == TK_HEXNUM || t == TK_REG || t == ')';
}

static bool make_tokens(const char *s) {
    nr_token = 0;
    int pos = 0;
    while (s[pos] != '\0') {
        bool matched = false;
        for (int i = 0; i < NR_RULES; i++) {
            regmatch_t m;
            if (regexec(&re_compiled[i], s + pos, 1, &m, 0) == 0 &&
                m.rm_so == 0) {
                int len = (int)m.rm_eo;
                int type = rules[i].type;
                if (type != TK_NOTYPE) {
                    if (nr_token >= MAX_TOKENS) {
                        Log("too many tokens"); return false;
                    }
                    if (len >= MAX_TOKEN_STR) {
                        Log("token too long: '%.*s'", len, s + pos);
                        return false;
                    }
                    tokens[nr_token].type = type;
                    memcpy(tokens[nr_token].str, s + pos, len);
                    tokens[nr_token].str[len] = '\0';
                    nr_token++;
                }
                pos += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            Log("unexpected character at: '%s'", s + pos);
            return false;
        }
    }

    /* Disambiguate unary operators: if '*' or '-' does not follow an
     * operand token, treat it as dereference / negation. */
    for (int i = 0; i < nr_token; i++) {
        bool prev_is_operand =
            (i > 0) && is_operand_tok(tokens[i - 1].type);
        if (tokens[i].type == '*' && !prev_is_operand) tokens[i].type = TK_DEREF;
        if (tokens[i].type == '-' && !prev_is_operand) tokens[i].type = TK_NEG;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 2. Parser: recursive descent matching C precedence                  */
/* ------------------------------------------------------------------ */

static int  p_pos;
static bool p_err;

static Token *peek(void) {
    return (p_pos < nr_token) ? &tokens[p_pos] : NULL;
}
static Token *consume(void) {
    Token *t = peek();
    if (t) p_pos++;
    return t;
}
static bool eat(int type) {
    Token *t = peek();
    if (t && t->type == type) { p_pos++; return true; }
    return false;
}
static int peek_type(void) {
    Token *t = peek();
    return t ? t->type : 0;
}

static word_t parse_expr(void);

static word_t parse_primary(void) {
    Token *t = consume();
    if (!t) { Log("unexpected end of expression"); p_err = true; return 0; }
    switch (t->type) {
        case TK_NUM:
            return (word_t)strtoul(t->str, NULL, 10);
        case TK_HEXNUM:
            return (word_t)strtoul(t->str, NULL, 16);
        case TK_REG: {
            bool ok = false;
            word_t v = isa_reg_val(t->str + 1, &ok);   /* skip '$' */
            if (!ok) { Log("unknown register '%s'", t->str); p_err = true; }
            return v;
        }
        case '(': {
            word_t v = parse_expr();
            if (!eat(')')) { Log("expected ')'"); p_err = true; }
            return v;
        }
        default:
            Log("unexpected token '%s'", t->str);
            p_err = true;
            return 0;
    }
}

static word_t parse_unary(void) {
    int t = peek_type();
    if (t == TK_NEG)   { consume(); return (word_t)(-(sword_t)parse_unary()); }
    if (t == '!')      { consume(); return parse_unary() ? 0 : 1; }
    if (t == '~')      { consume(); return ~parse_unary(); }
    if (t == TK_DEREF) { consume(); return paddr_read(parse_unary(), 4); }
    return parse_primary();
}

static word_t parse_mul(void) {
    word_t v = parse_unary();
    while (1) {
        int op = peek_type();
        if (op != '*' && op != '/' && op != '%') break;
        consume();
        word_t r = parse_unary();
        if ((op == '/' || op == '%') && r == 0) {
            Log("division by zero");
            p_err = true;
            return 0;
        }
        if      (op == '*') v = v * r;
        else if (op == '/') v = v / r;
        else                v = v % r;
    }
    return v;
}

static word_t parse_add(void) {
    word_t v = parse_mul();
    while (1) {
        int op = peek_type();
        if (op != '+' && op != '-') break;
        consume();
        word_t r = parse_mul();
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

static word_t parse_shift(void) {
    word_t v = parse_add();
    while (1) {
        int op = peek_type();
        if (op != TK_SHL && op != TK_SHR) break;
        consume();
        word_t r = parse_add();
        /* shift by >=32 is UB in C; mask to low 5 bits, matching RV32 */
        v = (op == TK_SHL) ? (v << (r & 31)) : (v >> (r & 31));
    }
    return v;
}

static word_t parse_rel(void) {
    word_t v = parse_shift();
    while (1) {
        int op = peek_type();
        if (op != '<' && op != '>' && op != TK_LE && op != TK_GE) break;
        consume();
        word_t r = parse_shift();
        switch (op) {
            case '<':   v = (v <  r) ? 1 : 0; break;
            case '>':   v = (v >  r) ? 1 : 0; break;
            case TK_LE: v = (v <= r) ? 1 : 0; break;
            case TK_GE: v = (v >= r) ? 1 : 0; break;
        }
    }
    return v;
}

static word_t parse_eq(void) {
    word_t v = parse_rel();
    while (1) {
        int op = peek_type();
        if (op != TK_EQ && op != TK_NEQ) break;
        consume();
        word_t r = parse_rel();
        v = (op == TK_EQ) ? (v == r) : (v != r);
    }
    return v;
}

static word_t parse_band(void) {
    word_t v = parse_eq();
    while (peek_type() == '&') { consume(); v &= parse_eq(); }
    return v;
}

static word_t parse_bxor(void) {
    word_t v = parse_band();
    while (peek_type() == '^') { consume(); v ^= parse_band(); }
    return v;
}

static word_t parse_bor(void) {
    word_t v = parse_bxor();
    while (peek_type() == '|') { consume(); v |= parse_bxor(); }
    return v;
}

static word_t parse_logand(void) {
    word_t v = parse_bor();
    while (peek_type() == TK_AND) {
        consume();
        word_t r = parse_bor();
        v = (v && r) ? 1 : 0;
    }
    return v;
}

static word_t parse_logor(void) {
    word_t v = parse_logand();
    while (peek_type() == TK_OR) {
        consume();
        word_t r = parse_logand();
        v = (v || r) ? 1 : 0;
    }
    return v;
}

static word_t parse_expr(void) {
    return parse_logor();
}

/* ------------------------------------------------------------------ */
/* 3. Public entry                                                     */
/* ------------------------------------------------------------------ */

word_t expr(const char *s, bool *success) {
    init_regex();
    if (!make_tokens(s)) { *success = false; return 0; }
    if (nr_token == 0)   { *success = false; return 0; }

    p_pos = 0;
    p_err = false;
    word_t v = parse_expr();

    if (p_err) {
        *success = false;
        return 0;
    }
    if (p_pos != nr_token) {
        Log("trailing garbage starting at token #%d ('%s')",
            p_pos, tokens[p_pos].str);
        *success = false;
        return 0;
    }
    *success = true;
    return v;
}
