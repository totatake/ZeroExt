#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "com_err.h"
#include "error_table.h"

static struct et_list *et_head = NULL;
static void (*com_err_hook)(const char *, long, const char *, va_list) = NULL;

static void default_com_err_proc(const char *whoami, long code, const char *fmt, va_list args) {
    if (whoami) {
        fputs(whoami, stderr);
        fputs(": ", stderr);
    }
    if (code) {
        fputs(error_message(code), stderr);
        fputs(" ", stderr);
    }
    if (fmt) {
        vfprintf(stderr, fmt, args);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

void (*set_com_err_hook(void (*new_proc)(const char *, long, const char *, va_list)))(const char *, long, const char *, va_list) {
    void (*old_proc)(const char *, long, const char *, va_list) = com_err_hook ? com_err_hook : default_com_err_proc;
    com_err_hook = new_proc;
    return old_proc;
}

void (*reset_com_err_hook(void))(const char *, long, const char *, va_list) {
    void (*old_proc)(const char *, long, const char *, va_list) = com_err_hook ? com_err_hook : default_com_err_proc;
    com_err_hook = NULL;
    return old_proc;
}

void com_err_va(const char *whoami, long code, const char *fmt, va_list args) {
    void (*proc)(const char *, long, const char *, va_list) = com_err_hook ? com_err_hook : default_com_err_proc;
    proc(whoami, code, fmt, args);
}

void com_err(const char *whoami, long code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    com_err_va(whoami, code, fmt, args);
    va_end(args);
}

void add_to_error_table(struct et_list *new_table) {
    struct et_list *cur;
    if (!new_table || !new_table->table) {
        return;
    }
    for (cur = et_head; cur != NULL; cur = cur->next) {
        if (cur == new_table || cur->table == new_table->table) {
            return;
        }
    }
    new_table->next = et_head;
    et_head = new_table;
}

errcode_t add_error_table(const struct error_table *et) {
    struct et_list *el;
    if (!et) {
        return -1;
    }
    el = (struct et_list *)malloc(sizeof(struct et_list));
    if (!el) {
        return -1;
    }
    el->table = et;
    el->next = NULL;
    add_to_error_table(el);
    return 0;
}

errcode_t remove_error_table(const struct error_table *et) {
    struct et_list **pp = &et_head;
    while (*pp) {
        if ((*pp)->table == et) {
            struct et_list *victim = *pp;
            *pp = victim->next;
            free(victim);
            return 0;
        }
        pp = &((*pp)->next);
    }
    return -1;
}

const char *error_message(long code) {
    static char unknown_buf[64];
    struct et_list *cur;

    if (code >= 0 && code < 256) {
        const char *sys_msg = strerror((int)code);
        if (sys_msg) {
            return sys_msg;
        }
    }

    for (cur = et_head; cur != NULL; cur = cur->next) {
        const struct error_table *t = cur->table;
        if (t && code >= t->base && code < t->base + t->n_msgs) {
            return t->msgs[code - t->base];
        }
    }

    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown ext2 error code %ld", code);
    return unknown_buf;
}
