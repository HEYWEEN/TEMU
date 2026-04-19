#include "common.h"
#include "monitor.h"

#define WP_EXPR_MAX 64

typedef struct WP {
    int        no;                 /* 1-based, stable across deletes     */
    char       expr_str[WP_EXPR_MAX];
    word_t     last_value;
    struct WP *next;
} WP;

static WP  *head     = NULL;
static int  next_no  = 1;

void wp_add(const char *e) {
    if (strlen(e) >= WP_EXPR_MAX) {
        printf("w: expression too long (max %d chars)\n", WP_EXPR_MAX - 1);
        return;
    }
    bool ok = false;
    word_t v = expr(e, &ok);
    if (!ok) {
        printf("w: cannot evaluate '%s'\n", e);
        return;
    }

    WP *w = malloc(sizeof(WP));
    Assert(w != NULL, "malloc WP failed");
    w->no         = next_no++;
    w->last_value = v;
    w->next       = NULL;
    strncpy(w->expr_str, e, WP_EXPR_MAX - 1);
    w->expr_str[WP_EXPR_MAX - 1] = '\0';

    /* Append to the end so display order matches creation order. */
    if (head == NULL) {
        head = w;
    } else {
        WP *cur = head;
        while (cur->next) cur = cur->next;
        cur->next = w;
    }
    printf("watchpoint #%d: %s = 0x%08" PRIx32 "\n", w->no, w->expr_str, v);
}

bool wp_del(int no) {
    WP **cur = &head;
    while (*cur) {
        if ((*cur)->no == no) {
            WP *dead = *cur;
            *cur = dead->next;
            free(dead);
            return true;
        }
        cur = &(*cur)->next;
    }
    return false;
}

void wp_display(void) {
    if (head == NULL) {
        printf("(no watchpoints)\n");
        return;
    }
    printf("%-4s %-40s %s\n", "Num", "Expression", "Value");
    for (WP *w = head; w; w = w->next) {
        printf("%-4d %-40s 0x%08" PRIx32 "\n",
               w->no, w->expr_str, w->last_value);
    }
}

bool wp_check(void) {
    bool any = false;
    for (WP *w = head; w; w = w->next) {
        bool ok = false;
        word_t v = expr(w->expr_str, &ok);
        if (!ok) continue;              /* silently skip broken expressions */
        if (v != w->last_value) {
            printf("watchpoint #%d (%s) changed: 0x%08" PRIx32
                   " -> 0x%08" PRIx32 "\n",
                   w->no, w->expr_str, w->last_value, v);
            w->last_value = v;
            any = true;
        }
    }
    return any;
}
