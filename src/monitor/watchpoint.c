#include "common.h"
#include "monitor.h"

/* Stage 1 commit 1 stub. The linked-list implementation lands in the
 * watchpoint commit. */

void wp_add(const char *e) {
    (void)e;
    printf("watchpoints: not implemented yet\n");
}

bool wp_del(int no) {
    (void)no;
    return false;
}

void wp_display(void) {
    printf("(no watchpoints)\n");
}

bool wp_check(void) {
    return false;
}
