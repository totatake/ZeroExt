#ifndef ZEROEXT4_WIN_NAMESPACE_H
#define ZEROEXT4_WIN_NAMESPACE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ext4_win_name_issue {
    EXT4_WIN_NAME_OK = 0,
    EXT4_WIN_NAME_INVALID_CHAR = 1,       /* < > : " | ? * \ or 0x01..0x1F */
    EXT4_WIN_NAME_RESERVED_DEVICE = 2,    /* CON, PRN, AUX, NUL, COM1..9, LPT1..9 (with or without .ext) */
    EXT4_WIN_NAME_TRAILING_DOT_SPACE = 3, /* ends with ' ' or '.' (excluding "." and "..") */
} ext4_win_name_issue_t;

/* Inspect a single POSIX filename component for Windows namespace compatibility. */
ext4_win_name_issue_t ext4_win_check_component(const char *name);

/* Inspect every segment of a normalized '/' path for Windows namespace compatibility. */
ext4_win_name_issue_t ext4_win_check_path(const char *norm_path);

const char *ext4_win_name_issue_str(ext4_win_name_issue_t issue);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_WIN_NAMESPACE_H */
