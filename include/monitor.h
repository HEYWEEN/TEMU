#ifndef MONITOR_H
#define MONITOR_H

#include "common.h"

/* Enter the Simple DeBugger (sdb) REPL. Returns when the user types 'q'
 * or EOF is reached. */
void sdb_mainloop(void);

/* Evaluate an expression string. On success sets *success = true and
 * returns the computed word_t; on any parse/eval error sets *success =
 * false. Does not print anything. */
word_t expr(const char *s, bool *success);

/* Watchpoints. NO is the external 1-based identifier shown to the user. */
void wp_add(const char *e);
bool wp_del(int no);
void wp_display(void);
bool wp_check(void);      /* returns true if any watchpoint changed value */

#endif /* MONITOR_H */
