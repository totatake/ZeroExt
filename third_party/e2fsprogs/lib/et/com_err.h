#ifndef __COM_ERR_H
#define __COM_ERR_H

#include <stdarg.h>

typedef long errcode_t;

struct error_table {
    char const *const *msgs;
    long base;
    int n_msgs;
};

struct et_list {
    struct et_list *next;
    const struct error_table *table;
};

extern void com_err(const char *, long, const char *, ...);
extern void com_err_va(const char *, long, const char *, va_list);
extern char const *error_message(long);
extern void (*set_com_err_hook(void (*)(const char *, long, const char *, va_list)))(const char *, long, const char *, va_list);
extern void (*reset_com_err_hook(void))(const char *, long, const char *, va_list);
extern errcode_t add_error_table(const struct error_table *);
extern errcode_t remove_error_table(const struct error_table *);
extern void add_to_error_table(struct et_list *);

#endif /* __COM_ERR_H */
