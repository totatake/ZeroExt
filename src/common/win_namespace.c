#include "win_namespace.h"
#include <string.h>

static int is_ascii_reserved_device_base(const char *base, size_t len) {
    char up[8];
    if (!base || len < 3 || len > 4) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        up[i] = c;
    }
    up[len] = '\0';

    if (len == 3) {
        return (strcmp(up, "CON") == 0 ||
                strcmp(up, "PRN") == 0 ||
                strcmp(up, "AUX") == 0 ||
                strcmp(up, "NUL") == 0) ? 1 : 0;
    }
    if (len == 4) {
        if ((strncmp(up, "COM", 3) == 0 || strncmp(up, "LPT", 3) == 0) &&
            up[3] >= '1' && up[3] <= '9') {
            return 1;
        }
    }
    return 0;
}

ext4_win_name_issue_t ext4_win_check_component(const char *name) {
    size_t len;
    const char *dot;
    size_t base_len;

    if (!name || name[0] == '\0') return EXT4_WIN_NAME_INVALID_CHAR;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return EXT4_WIN_NAME_OK;
    }

    len = strlen(name);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20U ||
            c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '|' || c == '?' || c == '*' || c == '\\') {
            return EXT4_WIN_NAME_INVALID_CHAR;
        }
    }

    if (name[len - 1] == ' ' || name[len - 1] == '.') {
        return EXT4_WIN_NAME_TRAILING_DOT_SPACE;
    }

    /* Win32 treats "CON.txt", "nul   .dat", etc. as reserved DOS devices. */
    dot = strchr(name, '.');
    base_len = dot ? (size_t)(dot - name) : len;
    while (base_len > 0 && name[base_len - 1] == ' ') {
        base_len--;
    }
    if (is_ascii_reserved_device_base(name, base_len)) {
        return EXT4_WIN_NAME_RESERVED_DEVICE;
    }

    return EXT4_WIN_NAME_OK;
}

ext4_win_name_issue_t ext4_win_check_path(const char *norm_path) {
    const char *p;
    if (!norm_path || norm_path[0] == '\0') return EXT4_WIN_NAME_INVALID_CHAR;
    p = norm_path;
    while (*p) {
        char comp[300];
        size_t len = 0;
        if (*p == '/') {
            p++;
            continue;
        }
        while (p[len] && p[len] != '/') {
            if (len + 1 >= sizeof(comp)) return EXT4_WIN_NAME_INVALID_CHAR;
            comp[len] = p[len];
            len++;
        }
        comp[len] = '\0';
        {
            ext4_win_name_issue_t issue = ext4_win_check_component(comp);
            if (issue != EXT4_WIN_NAME_OK) return issue;
        }
        p += len;
    }
    return EXT4_WIN_NAME_OK;
}

const char *ext4_win_name_issue_str(ext4_win_name_issue_t issue) {
    switch (issue) {
    case EXT4_WIN_NAME_OK:                 return "ok";
    case EXT4_WIN_NAME_INVALID_CHAR:       return "invalid_char";
    case EXT4_WIN_NAME_RESERVED_DEVICE:    return "reserved_device";
    case EXT4_WIN_NAME_TRAILING_DOT_SPACE: return "trailing_dot_or_space";
    default:                               return "unknown";
    }
}
