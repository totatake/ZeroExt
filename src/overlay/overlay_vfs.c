#include "overlay_vfs.h"
#include "../common/ext4_errno.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OV_SYNTHETIC_INO_BASE 0x80000000U
#define OV_WHITEOUT_PREFIX    ".wh."
#define OV_OPAQUE_NAME        ".wh.__dir_opaque"
#define OV_OPAQUE_NAME_W      L".wh.__dir_opaque"
#define OV_LOWER_ID_NAME      ".wh.__ext4_lower_id"
#define OV_LOWER_ID_NAME_W    L".wh.__ext4_lower_id"

typedef struct ov_ino_entry {
    ext2_ino_t           ino;
    ext2_ino_t           lower_ino;
    char                 path[1024];
    struct ov_ino_entry *next;
} ov_ino_entry_t;

struct ext4_overlay_ctx {
    wchar_t          upper_dir_w[1024];
    int              case_sensitive;
    SRWLOCK          map_lock;
    CRITICAL_SECTION mut_cs;
    ov_ino_entry_t  *entries;
    uint32_t         next_synth_ino;
    LONG             tmp_seq;
    volatile LONG64  copyup_bytes;
};

static int win32_err_to_errno(DWORD err) {
    switch (err) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NO_MORE_FILES: return -ENOENT;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: return -EEXIST;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: return -EACCES;
    case ERROR_WRITE_PROTECT: return -EROFS;
    case ERROR_DIR_NOT_EMPTY: return -ENOTEMPTY;
    case ERROR_DIRECTORY: return -ENOTDIR;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return -ENOSPC;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE: return -EINVAL;
    default: return -EIO;
    }
}

static uint64_t ft_to_u64(const FILETIME *ft) {
    ULARGE_INTEGER ui;
    ui.LowPart = ft->dwLowDateTime;
    ui.HighPart = ft->dwHighDateTime;
    return ui.QuadPart;
}

static FILETIME u64_to_ft(uint64_t v) {
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(v & 0xFFFFFFFFULL);
    ft.dwHighDateTime = (DWORD)(v >> 32);
    return ft;
}

static int is_reserved_wh_name(const char *name) {
    return name && strncmp(name, OV_WHITEOUT_PREFIX, 4) == 0;
}

static int path_has_reserved_wh(const char *norm_path) {
    const char *p = norm_path;
    if (!p) return 0;
    while (*p) {
        if (*p == '/') {
            p++;
            continue;
        }
        if (strncmp(p, OV_WHITEOUT_PREFIX, 4) == 0) {
            return 1;
        }
        while (*p && *p != '/') p++;
    }
    return 0;
}

static int join_upper_path_w(const ext4_overlay_ctx_t *ov, const char *norm_path,
                             wchar_t *out_w, size_t out_cch) {
    size_t ulen;
    wchar_t rel_w[1024];

    if (!ov || !norm_path || !out_w || out_cch == 0) return -EINVAL;
    ulen = wcslen(ov->upper_dir_w);

    if (strcmp(norm_path, "/") == 0) {
        if (ulen + 1 > out_cch) return -ENAMETOOLONG;
        wcscpy_s(out_w, out_cch, ov->upper_dir_w);
        return 0;
    }

    if (ext4_utf8_to_wide(norm_path, rel_w, _countof(rel_w)) != 0) {
        return -EINVAL;
    }
    for (size_t i = 0; rel_w[i]; i++) {
        if (rel_w[i] == L'/') rel_w[i] = L'\\';
    }
    if (ulen + wcslen(rel_w) + 1 > out_cch) return -ENAMETOOLONG;
    _snwprintf_s(out_w, out_cch, _TRUNCATE, L"%ls%ls", ov->upper_dir_w, rel_w);
    return 0;
}

static int make_whiteout_path_w(const ext4_overlay_ctx_t *ov, const char *norm_path,
                                wchar_t *out_w, size_t out_cch) {
    char parent[1024];
    char leaf[EXT2_NAME_LEN + 1];
    char wh_utf8[1024];
    int rc;

    if (strcmp(norm_path, "/") == 0) return -EINVAL;
    rc = ext4_vfs_split_parent(norm_path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != 0) return rc;

    if (strcmp(parent, "/") == 0) {
        _snprintf_s(wh_utf8, sizeof(wh_utf8), _TRUNCATE, "/%s%s", OV_WHITEOUT_PREFIX, leaf);
    } else {
        _snprintf_s(wh_utf8, sizeof(wh_utf8), _TRUNCATE, "%s/%s%s", parent, OV_WHITEOUT_PREFIX, leaf);
    }
    return join_upper_path_w(ov, wh_utf8, out_w, out_cch);
}

static int make_opaque_path_w(const ext4_overlay_ctx_t *ov, const char *norm_dir,
                              wchar_t *out_w, size_t out_cch) {
    wchar_t dir_w[1024];
    int rc = join_upper_path_w(ov, norm_dir, dir_w, _countof(dir_w));
    if (rc != 0) return rc;
    if (wcslen(dir_w) + 1 + wcslen(OV_OPAQUE_NAME_W) + 1 > out_cch) return -ENAMETOOLONG;
    _snwprintf_s(out_w, out_cch, _TRUNCATE, L"%ls\\%ls", dir_w, OV_OPAQUE_NAME_W);
    return 0;
}

static int file_exists_w(const wchar_t *path_w, DWORD *out_attrs) {
    DWORD a = GetFileAttributesW(path_w);
    if (out_attrs) *out_attrs = a;
    return (a != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

#ifndef FILE_CS_FLAG_CASE_SENSITIVE_DIR
#define FILE_CS_FLAG_CASE_SENSITIVE_DIR 0x00000001
#endif

static int ensure_dir_case_sensitive_w(const wchar_t *dir_w) {
    FILE_CASE_SENSITIVE_INFO cs;
    HANDLE h;

    /* Query first with read-only attributes handle (works on already-enabled non-empty dirs) */
    h = CreateFileW(dir_w, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return win32_err_to_errno(GetLastError());
    }
    memset(&cs, 0, sizeof(cs));
    if (GetFileInformationByHandleEx(h, FileCaseSensitiveInfo, &cs, sizeof(cs)) &&
        (cs.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);

    /* Enable NTFS per-directory case sensitivity (requires write/attribute/delete rights) */
    h = CreateFileW(dir_w,
                    FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        ext4_log(EXT4_LOG_ERROR,
                 "Cannot open overlay directory '%ls' to enable NTFS case sensitivity (winerr=%lu).",
                 dir_w, (unsigned long)err);
        return win32_err_to_errno(err);
    }
    cs.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    if (!SetFileInformationByHandle(h, FileCaseSensitiveInfo, &cs, sizeof(cs))) {
        DWORD err = GetLastError();
        CloseHandle(h);
        ext4_log(EXT4_LOG_ERROR,
                 "Failed to enable NTFS case sensitivity on '%ls' (winerr=%lu). "
                 "Requires an empty NTFS directory and sufficient privileges "
                 "(or pre-enable as Administrator via 'fsutil file setCaseSensitiveInfo <dir> enable').",
                 dir_w, (unsigned long)err);
        return (err == ERROR_NOT_SUPPORTED || err == ERROR_INVALID_FUNCTION)
                   ? -ENOTSUP
                   : win32_err_to_errno(err);
    }
    CloseHandle(h);
    return 0;
}

static int path_is_within_root_w(const wchar_t *path_w, size_t path_len,
                                 const wchar_t *root_w, size_t root_len) {
    if (!root_w || path_len < root_len) return 0;
    if (_wcsnicmp(path_w, root_w, root_len) != 0) return 0;
    return (path_len == root_len || path_w[root_len] == L'\\' || path_w[root_len] == L'/') ? 1 : 0;
}

static int create_dir_recursive_w(const wchar_t *dir_w, const wchar_t *cs_root_w) {
    DWORD attrs = GetFileAttributesW(dir_w);
    size_t cs_root_len = cs_root_w ? wcslen(cs_root_w) : 0;
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -ENOTDIR;
        if (path_is_within_root_w(dir_w, wcslen(dir_w), cs_root_w, cs_root_len)) {
            return ensure_dir_case_sensitive_w(dir_w);
        }
        return 0;
    }

    {
        wchar_t tmp[1024];
        size_t len = wcslen(dir_w);
        size_t i = 0;
        if (len >= _countof(tmp)) return -ENAMETOOLONG;
        wcscpy_s(tmp, _countof(tmp), dir_w);

        /* Skip drive prefix "C:\" or UNC "\\server\share\" */
        if (len >= 3 && tmp[1] == L':' && (tmp[2] == L'\\' || tmp[2] == L'/')) {
            i = 3;
        } else if (len >= 2 && tmp[0] == L'\\' && tmp[1] == L'\\') {
            i = 2;
        }

        for (; i <= len; i++) {
            if (tmp[i] == L'\\' || tmp[i] == L'/' || tmp[i] == 0) {
                wchar_t saved = tmp[i];
                if (i > 0) {
                    int created_now = 0;
                    tmp[i] = 0;
                    attrs = GetFileAttributesW(tmp);
                    if (attrs == INVALID_FILE_ATTRIBUTES) {
                        if (!CreateDirectoryW(tmp, NULL)) {
                            DWORD err = GetLastError();
                            if (err != ERROR_ALREADY_EXISTS) {
                                return win32_err_to_errno(err);
                            }
                        } else {
                            created_now = 1;
                        }
                    } else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                        return -ENOTDIR;
                    }
                    if (path_is_within_root_w(tmp, i, cs_root_w, cs_root_len)) {
                        int csrc = ensure_dir_case_sensitive_w(tmp);
                        if (csrc != 0) {
                            if (created_now) RemoveDirectoryW(tmp);
                            return csrc;
                        }
                    }
                    tmp[i] = saved;
                }
            }
        }
    }
    return 0;
}

static int ensure_upper_parents_w(const ext4_overlay_ctx_t *ov, const char *norm_path) {
    char parent[1024];
    char leaf[EXT2_NAME_LEN + 1];
    wchar_t parent_w[1024];
    int rc;

    if (strcmp(norm_path, "/") == 0) return 0;
    rc = ext4_vfs_split_parent(norm_path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != 0) return rc;
    rc = join_upper_path_w(ov, parent, parent_w, _countof(parent_w));
    if (rc != 0) return rc;
    return create_dir_recursive_w(parent_w, ov->case_sensitive ? ov->upper_dir_w : NULL);
}

static int create_marker_file_w(const wchar_t *path_w) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    HANDLE h = CreateFileW(path_w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    int rc = 0;
    if (h == INVALID_HANDLE_VALUE) {
        rc = win32_err_to_errno(GetLastError());
    } else {
        CloseHandle(h);
    }
    if (_zt0) {
        ztrace_record_complete("overlay", "overlay_whiteout", _zt0, ztrace_now_qpc() - _zt0,
                               0, 1, (int64_t)rc, ZTRACE_ARGS_OVERLAY);
    }
    return rc;
}

static int delete_tree_w(const wchar_t *path_w) {
    DWORD attrs = GetFileAttributesW(path_w);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(path_w, attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!DeleteFileW(path_w)) return win32_err_to_errno(GetLastError());
        return 0;
    }

    {
        wchar_t pattern[1040];
        WIN32_FIND_DATAW fd;
        HANDLE hfind;

        _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%ls\\*", path_w);
        hfind = FindFirstFileW(pattern, &fd);
        if (hfind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                    continue;
                }
                {
                    wchar_t child[1040];
                    _snwprintf_s(child, _countof(child), _TRUNCATE, L"%ls\\%ls", path_w, fd.cFileName);
                    int rc = delete_tree_w(child);
                    if (rc != 0) {
                        FindClose(hfind);
                        return rc;
                    }
                }
            } while (FindNextFileW(hfind, &fd));
            FindClose(hfind);
        }
    }

    if (!RemoveDirectoryW(path_w)) {
        return win32_err_to_errno(GetLastError());
    }
    return 0;
}

/* ---- Bidirectional inode <-> path registry ---- */

static int same_path(const ext4_vfs_ctx_t *ctx, const char *a, const char *b) {
    return (ctx && ctx->case_insensitive) ? (_stricmp(a, b) == 0) : (strcmp(a, b) == 0);
}

static ext2_ino_t overlay_assign_ino(ext4_vfs_ctx_t *ctx, const char *norm_path, ext2_ino_t lower_ino) {
    ext4_overlay_ctx_t *ov = ctx->overlay;
    ov_ino_entry_t *e;
    int lower_ino_used = 0;
    ext2_ino_t assigned = 0;

    if (strcmp(norm_path, "/") == 0) {
        return EXT2_ROOT_INO;
    }

    AcquireSRWLockExclusive(&ov->map_lock);
    for (e = ov->entries; e; e = e->next) {
        if (same_path(ctx, e->path, norm_path)) {
            if (lower_ino != 0 && e->lower_ino == 0) {
                e->lower_ino = lower_ino;
            }
            assigned = e->ino;
            ReleaseSRWLockExclusive(&ov->map_lock);
            return assigned;
        }
        if (lower_ino != 0 && e->ino == lower_ino) {
            lower_ino_used = 1;
        }
    }

    e = (ov_ino_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        ReleaseSRWLockExclusive(&ov->map_lock);
        return lower_ino ? lower_ino : ov->next_synth_ino++;
    }

    if (lower_ino != 0 && !lower_ino_used) {
        assigned = lower_ino;
    } else {
        assigned = ov->next_synth_ino++;
        if (assigned < OV_SYNTHETIC_INO_BASE) assigned = OV_SYNTHETIC_INO_BASE;
    }

    e->ino = assigned;
    e->lower_ino = lower_ino;
    strcpy_s(e->path, sizeof(e->path), norm_path);
    e->next = ov->entries;
    ov->entries = e;
    ReleaseSRWLockExclusive(&ov->map_lock);
    return assigned;
}

static int overlay_lookup_ino(ext4_vfs_ctx_t *ctx, ext2_ino_t ino,
                              char *out_path, size_t out_sz, ext2_ino_t *out_lower_ino) {
    ext4_overlay_ctx_t *ov = ctx->overlay;
    ov_ino_entry_t *e;

    if (ino == EXT2_ROOT_INO) {
        strcpy_s(out_path, out_sz, "/");
        if (out_lower_ino) *out_lower_ino = EXT2_ROOT_INO;
        return 0;
    }

    AcquireSRWLockShared(&ov->map_lock);
    for (e = ov->entries; e; e = e->next) {
        if (e->ino == ino) {
            strcpy_s(out_path, out_sz, e->path);
            if (out_lower_ino) *out_lower_ino = e->lower_ino;
            ReleaseSRWLockShared(&ov->map_lock);
            return 0;
        }
    }
    ReleaseSRWLockShared(&ov->map_lock);
    return -ENOENT;
}

static void overlay_rename_map(ext4_vfs_ctx_t *ctx, const char *old_norm, const char *new_norm) {
    ext4_overlay_ctx_t *ov = ctx->overlay;
    size_t old_len = strlen(old_norm);
    ov_ino_entry_t *e;

    AcquireSRWLockExclusive(&ov->map_lock);
    for (e = ov->entries; e; e = e->next) {
        if (same_path(ctx, e->path, old_norm)) {
            strcpy_s(e->path, sizeof(e->path), new_norm);
        } else {
            size_t plen = strlen(e->path);
            if (plen > old_len && e->path[old_len] == '/' &&
                ((ctx->case_insensitive && _strnicmp(e->path, old_norm, old_len) == 0) ||
                 (!ctx->case_insensitive && strncmp(e->path, old_norm, old_len) == 0))) {
                char updated[1024];
                _snprintf_s(updated, sizeof(updated), _TRUNCATE, "%s%s", new_norm, e->path + old_len);
                strcpy_s(e->path, sizeof(e->path), updated);
            }
        }
    }
    ReleaseSRWLockExclusive(&ov->map_lock);
}

/* ---- Overlay path resolution (Upper / Lower / Whiteout / Opaque) ---- */

typedef struct ov_resolution {
    int         exists;
    int         in_upper;
    int         in_lower;
    int         is_dir;
    int         lower_scope_active; /* 1 if lower merges into this node (if dir) */
    int         deleted_by_wh;
    int         blocked_by_upper_file;
    DWORD       upper_attrs;
    wchar_t     upper_path_w[1024];
    ext4_stat_t lower_st;
} ov_resolution_t;

static int overlay_resolve_impl(ext4_vfs_ctx_t *ctx, const char *raw_path,
                                char *out_norm, size_t out_norm_sz, ov_resolution_t *res) {
    ext4_overlay_ctx_t *ov = ctx->overlay;
    char norm[1024];
    int rc;

    memset(res, 0, sizeof(*res));
    rc = ext4_vfs_normalize_path(raw_path, norm, sizeof(norm));
    if (rc != 0) return rc;
    if (out_norm && out_norm_sz > 0) {
        strcpy_s(out_norm, out_norm_sz, norm);
    }

    if (path_has_reserved_wh(norm)) {
        return -ENOENT;
    }

    if (strcmp(norm, "/") == 0) {
        wchar_t opq_w[1024];
        res->exists = 1;
        res->in_upper = file_exists_w(ov->upper_dir_w, &res->upper_attrs);
        wcscpy_s(res->upper_path_w, _countof(res->upper_path_w), ov->upper_dir_w);
        if (make_opaque_path_w(ov, "/", opq_w, _countof(opq_w)) == 0 && file_exists_w(opq_w, NULL)) {
            res->lower_scope_active = 0;
        } else {
            res->lower_scope_active = 1;
        }
        if (res->lower_scope_active && ext4_vfs_lower_stat(ctx, "/", &res->lower_st) == 0) {
            res->in_lower = 1;
        }
        res->is_dir = 1;
        return 0;
    }

    /* Walk segment by segment from "/" so ancestor whiteouts / opaque dirs / upper non-dirs are honored. */
    {
        char cur[1024];
        const char *p = norm + 1;
        int lower_active = 1;
        wchar_t opq_w[1024];

        strcpy_s(cur, sizeof(cur), "/");
        if (make_opaque_path_w(ov, "/", opq_w, _countof(opq_w)) == 0 && file_exists_w(opq_w, NULL)) {
            lower_active = 0;
        }

        while (*p) {
            char seg[EXT2_NAME_LEN + 1];
            size_t slen = 0;
            wchar_t wh_w[1024];
            wchar_t up_w[1024];
            DWORD u_attrs = INVALID_FILE_ATTRIBUTES;
            int u_exists = 0;
            int l_exists = 0;
            ext4_stat_t l_st;

            while (p[slen] && p[slen] != '/') {
                if (slen + 1 >= sizeof(seg)) return -ENAMETOOLONG;
                seg[slen] = p[slen];
                slen++;
            }
            seg[slen] = '\0';
            p += slen;
            if (*p == '/') p++;

            if (strcmp(cur, "/") == 0) {
                _snprintf_s(cur, sizeof(cur), _TRUNCATE, "/%s", seg);
            } else {
                size_t clen = strlen(cur);
                if (clen + 1 + slen + 1 > sizeof(cur)) return -ENAMETOOLONG;
                _snprintf_s(cur + clen, sizeof(cur) - clen, _TRUNCATE, "/%s", seg);
            }

            if (make_whiteout_path_w(ov, cur, wh_w, _countof(wh_w)) == 0 && file_exists_w(wh_w, NULL)) {
                memset(res, 0, sizeof(*res));
                join_upper_path_w(ov, norm, res->upper_path_w, _countof(res->upper_path_w));
                if (*p == '\0') res->deleted_by_wh = 1;
                return 0;
            }

            if (join_upper_path_w(ov, cur, up_w, _countof(up_w)) != 0) return -ENAMETOOLONG;
            u_exists = file_exists_w(up_w, &u_attrs);

            if (lower_active) {
                memset(&l_st, 0, sizeof(l_st));
                if (ext4_vfs_lower_stat(ctx, cur, &l_st) == 0) {
                    l_exists = 1;
                } else {
                    lower_active = 0;
                }
            } else {
                memset(&l_st, 0, sizeof(l_st));
            }

            if (*p != '\0') {
                /* Intermediate directory component */
                if (u_exists) {
                    if (!(u_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                        memset(res, 0, sizeof(*res));
                        join_upper_path_w(ov, norm, res->upper_path_w, _countof(res->upper_path_w));
                        return 0; /* Blocked by upper regular file */
                    }
                    if (make_opaque_path_w(ov, cur, opq_w, _countof(opq_w)) == 0 &&
                        file_exists_w(opq_w, NULL)) {
                        lower_active = 0;
                    }
                } else if (!l_exists || !l_st.is_dir) {
                    memset(res, 0, sizeof(*res));
                    join_upper_path_w(ov, norm, res->upper_path_w, _countof(res->upper_path_w));
                    return 0;
                }
            } else {
                /* Target leaf */
                res->in_upper = u_exists;
                res->upper_attrs = u_attrs;
                wcscpy_s(res->upper_path_w, _countof(res->upper_path_w), up_w);

                if (u_exists && !(u_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    /* Upper regular file completely shadows any lower entry (including a dir) */
                    res->in_lower = 0;
                    res->is_dir = 0;
                    res->lower_scope_active = 0;
                    res->blocked_by_upper_file = l_exists ? 1 : 0;
                } else if (u_exists && (u_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    res->is_dir = 1;
                    if (lower_active && l_exists && l_st.is_dir) {
                        if (make_opaque_path_w(ov, cur, opq_w, _countof(opq_w)) == 0 &&
                            file_exists_w(opq_w, NULL)) {
                            lower_active = 0;
                        }
                    } else {
                        lower_active = 0;
                    }
                    res->lower_scope_active = lower_active;
                    res->in_lower = (lower_active && l_exists) ? 1 : 0;
                    if (res->in_lower) res->lower_st = l_st;
                } else if (l_exists) {
                    res->in_lower = 1;
                    res->lower_st = l_st;
                    res->is_dir = l_st.is_dir;
                    res->lower_scope_active = l_st.is_dir ? lower_active : 0;
                }
                res->exists = (res->in_upper || res->in_lower) ? 1 : 0;
                return 0;
            }
        }
    }

    return 0;
}

static int overlay_resolve(ext4_vfs_ctx_t *ctx, const char *raw_path,
                           char *out_norm, size_t out_norm_sz, ov_resolution_t *res) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    int rc = overlay_resolve_impl(ctx, raw_path, out_norm, out_norm_sz, res);
    if (_zt0) {
        ztrace_record_complete("overlay", "overlay_resolve", _zt0, ztrace_now_qpc() - _zt0,
                               0, (rc == 0 && res) ? (uint64_t)res->in_upper : 0ULL,
                               (int64_t)rc, ZTRACE_ARGS_OVERLAY);
    }
    return rc;
}

static void fill_stat_from_upper(ext4_vfs_ctx_t *ctx, const char *norm_path,
                                 const wchar_t *upper_path_w, ext2_ino_t lower_ino,
                                 ext4_stat_t *out_st) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    memset(out_st, 0, sizeof(*out_st));
    if (!GetFileAttributesExW(upper_path_w, GetFileExInfoStandard, &fad)) {
        return;
    }

    out_st->ino = overlay_assign_ino(ctx, norm_path, lower_ino);
    out_st->is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    out_st->mode = (uint16_t)(out_st->is_dir ? 0040755 : 0100644);
    out_st->file_attributes = fad.dwFileAttributes;
    if (out_st->is_dir) {
        out_st->size = 0;
        out_st->alloc_size = 0;
        out_st->hard_links = 2;
    } else {
        out_st->size = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
        out_st->alloc_size = ((out_st->size + 4095ULL) / 4096ULL) * 4096ULL;
        out_st->hard_links = 1;
    }
    if (ctx->read_only) {
        out_st->file_attributes |= FILE_ATTRIBUTE_READONLY;
    }
    out_st->creation_time = ft_to_u64(&fad.ftCreationTime);
    out_st->last_access_time = ft_to_u64(&fad.ftLastAccessTime);
    out_st->last_write_time = ft_to_u64(&fad.ftLastWriteTime);
    out_st->change_time = out_st->last_write_time;
}

/* ---- Copy-up (Lower RO ext4 -> Upper RW NTFS) ---- */

static int ensure_upper_copy(ext4_vfs_ctx_t *ctx, const char *norm_path, ov_resolution_t *res) {
    ext4_overlay_ctx_t *ov = ctx->overlay;
    int rc;

    rc = overlay_resolve(ctx, norm_path, NULL, 0, res);
    if (rc != 0) return rc;
    if (!res->exists) return -ENOENT;
    if (res->in_upper) return 0;

    rc = ensure_upper_parents_w(ov, norm_path);
    if (rc != 0) return rc;

    if (res->is_dir) {
        if (!CreateDirectoryW(res->upper_path_w, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) return win32_err_to_errno(err);
        }
        if (ov->case_sensitive) {
            rc = ensure_dir_case_sensitive_w(res->upper_path_w);
            if (rc != 0) return rc;
        }
        return overlay_resolve(ctx, norm_path, NULL, 0, res);
    }

    /* Stream regular file content from Lower (Direct Extent I/O) into a temp file on Upper, then atomically replace */
    {
        uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
        wchar_t tmp_w[1040];
        HANDLE hf;
        uint8_t *buf;
        const uint32_t chunk_sz = 256U * 1024U;
        uint64_t off = 0;
        uint64_t total = res->lower_st.size;
        LONG seq = InterlockedIncrement(&ov->tmp_seq);

        _snwprintf_s(tmp_w, _countof(tmp_w), _TRUNCATE, L"%ls.ext4tmp_%lu_%ld",
                     res->upper_path_w, GetCurrentThreadId(), seq);

        hf = CreateFileW(tmp_w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            rc = win32_err_to_errno(GetLastError());
            if (_zt0) {
                ztrace_record_complete("overlay", "overlay_copy_up", _zt0, ztrace_now_qpc() - _zt0,
                                       total, 0, (int64_t)rc, ZTRACE_ARGS_OVERLAY);
            }
            return rc;
        }

        buf = (uint8_t *)malloc(chunk_sz);
        if (!buf) {
            CloseHandle(hf);
            DeleteFileW(tmp_w);
            if (_zt0) {
                ztrace_record_complete("overlay", "overlay_copy_up", _zt0, ztrace_now_qpc() - _zt0,
                                       total, 0, (int64_t)-ENOMEM, ZTRACE_ARGS_OVERLAY);
            }
            return -ENOMEM;
        }

        rc = 0;
        while (off < total) {
            uint32_t want = (total - off < chunk_sz) ? (uint32_t)(total - off) : chunk_sz;
            uint32_t got = 0;
            DWORD put = 0;
            rc = ext4_vfs_lower_read(ctx, res->lower_st.ino, buf, off, want, &got);
            if (rc != 0) break;
            if (got == 0) break;
            if (!WriteFile(hf, buf, got, &put, NULL) || put != got) {
                rc = win32_err_to_errno(GetLastError());
                break;
            }
            off += got;
        }

        free(buf);
        if (rc == 0) {
            FILETIME ct = u64_to_ft(res->lower_st.creation_time);
            FILETIME at = u64_to_ft(res->lower_st.last_access_time);
            FILETIME wt = u64_to_ft(res->lower_st.last_write_time);
            SetFileTime(hf, &ct, &at, &wt);
        }
        CloseHandle(hf);

        if (rc != 0) {
            DeleteFileW(tmp_w);
            if (_zt0) {
                ztrace_record_complete("overlay", "overlay_copy_up", _zt0, ztrace_now_qpc() - _zt0,
                                       off, 0, (int64_t)rc, ZTRACE_ARGS_OVERLAY);
            }
            return rc;
        }

        if (!MoveFileExW(tmp_w, res->upper_path_w, MOVEFILE_REPLACE_EXISTING)) {
            DWORD err = GetLastError();
            DeleteFileW(tmp_w);
            rc = win32_err_to_errno(err);
            if (_zt0) {
                ztrace_record_complete("overlay", "overlay_copy_up", _zt0, ztrace_now_qpc() - _zt0,
                                       total, 0, (int64_t)rc, ZTRACE_ARGS_OVERLAY);
            }
            return rc;
        }

        /* Ensure upper copy is writable when copy-up happened for modification, preserving hidden bit */
        DWORD dst_attrs = res->lower_st.file_attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
        SetFileAttributesW(res->upper_path_w, dst_attrs ? dst_attrs : FILE_ATTRIBUTE_ARCHIVE);

        if (_zt0) {
            LONG64 cum = InterlockedAdd64(&ov->copyup_bytes, (LONG64)total);
            ztrace_record_complete("overlay", "overlay_copy_up", _zt0, ztrace_now_qpc() - _zt0,
                                   total, 0, 0, ZTRACE_ARGS_OVERLAY);
            ztrace_record_counter("overlay", "Overlay CopyUp Bytes", (uint64_t)cum, 0, 0, ZTRACE_ARGS_COUNTER);
        }
    }

    return overlay_resolve(ctx, norm_path, NULL, 0, res);
}

static int upper_dir_has_entries_w(const wchar_t *dir_w) {
    wchar_t pattern[1040];
    WIN32_FIND_DATAW fd;
    HANDLE hfind;
    int non_empty = 0;

    _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%ls\\*", dir_w);
    hfind = FindFirstFileW(pattern, &fd);
    if (hfind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                non_empty = 1;
                break;
            }
        } while (FindNextFileW(hfind, &fd));
        FindClose(hfind);
    }
    return non_empty;
}

static int overlay_validate_lower_id(const ext4_overlay_ctx_t *ov, ext4_vfs_ctx_t *lower_ctx) {
    wchar_t id_w[1040];
    char expected[128];
    const __u8 *u;
    HANDLE hr;

    if (!ov || !lower_ctx || !lower_ctx->fs || !lower_ctx->fs->super) return 0;

    u = lower_ctx->fs->super->s_uuid;
    _snprintf_s(expected, sizeof(expected), _TRUNCATE,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x:%llu:%u%s",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
                (unsigned long long)ext2fs_blocks_count(lower_ctx->fs->super),
                (unsigned)lower_ctx->fs->blocksize,
                ov->case_sensitive ? ":cs" : "");

    _snwprintf_s(id_w, _countof(id_w), _TRUNCATE, L"%ls\\%ls", ov->upper_dir_w, OV_LOWER_ID_NAME_W);
    hr = CreateFileW(id_w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hr != INVALID_HANDLE_VALUE) {
        char stored[128] = {0};
        DWORD got = 0;
        BOOL ok = ReadFile(hr, stored, sizeof(stored) - 1, &got, NULL);
        CloseHandle(hr);
        if (!ok || got == 0 || strcmp(stored, expected) != 0) {
            ext4_log(EXT4_LOG_ERROR,
                     "Overlay upperdir '%ls' belongs to a different lower ext4 filesystem "
                     "(expected='%s', found='%s'). Clean or choose a separate upperdir.",
                     ov->upper_dir_w, expected, stored);
            return -EINVAL;
        }
        return 0;
    }

    if (upper_dir_has_entries_w(ov->upper_dir_w)) {
        ext4_log(EXT4_LOG_WARN,
                 "Overlay upperdir '%ls' is not empty and has no %s marker; "
                 "existing files will shadow lower ext4 entries.",
                 ov->upper_dir_w, OV_LOWER_ID_NAME);
    }

    {
        HANDLE hw = CreateFileW(id_w, GENERIC_WRITE, FILE_SHARE_READ,
                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (hw != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(hw, expected, (DWORD)strlen(expected), &put, NULL);
            CloseHandle(hw);
        }
    }
    return 0;
}

/* ---- Lifecycle ---- */

int overlay_vfs_create(ext4_vfs_ctx_t *lower_ctx, const char *upper_dir_utf8,
                       ext4_overlay_ctx_t **out_ov) {
    ext4_overlay_ctx_t *ov;
    wchar_t wdir[1024];
    wchar_t full_w[1024];
    DWORD full_len;
    size_t len;
    int rc;

    if (!upper_dir_utf8 || !upper_dir_utf8[0] || !out_ov) return -EINVAL;
    *out_ov = NULL;

    if (ext4_utf8_to_wide(upper_dir_utf8, wdir, _countof(wdir)) != 0) {
        return -EINVAL;
    }

    full_len = GetFullPathNameW(wdir, _countof(full_w), full_w, NULL);
    if (full_len == 0 || full_len >= _countof(full_w)) {
        return -EINVAL;
    }

    len = wcslen(full_w);
    while (len > 3 && (full_w[len - 1] == L'\\' || full_w[len - 1] == L'/')) {
        full_w[--len] = 0;
    }
    if (len == 3 && full_w[1] == L':' && full_w[2] == L'\\') {
        /* Root of drive (rare); normalize without trailing slash would break C:, disallow drive root */
        return -EINVAL;
    }

    int case_sensitive = (lower_ctx && !lower_ctx->case_insensitive) ? 1 : 0;

    rc = create_dir_recursive_w(full_w, case_sensitive ? full_w : NULL);
    if (rc != 0) return rc;

    ov = (ext4_overlay_ctx_t *)calloc(1, sizeof(*ov));
    if (!ov) return -ENOMEM;

    wcscpy_s(ov->upper_dir_w, _countof(ov->upper_dir_w), full_w);
    ov->case_sensitive = case_sensitive;
    InitializeSRWLock(&ov->map_lock);
    InitializeCriticalSection(&ov->mut_cs);
    ov->next_synth_ino = OV_SYNTHETIC_INO_BASE;

    rc = overlay_validate_lower_id(ov, lower_ctx);
    if (rc != 0) {
        overlay_vfs_destroy(ov);
        return rc;
    }

    *out_ov = ov;
    return 0;
}

void overlay_vfs_destroy(ext4_overlay_ctx_t *ov) {
    ov_ino_entry_t *e;
    if (!ov) return;
    e = ov->entries;
    while (e) {
        ov_ino_entry_t *next = e->next;
        free(e);
        e = next;
    }
    DeleteCriticalSection(&ov->mut_cs);
    free(ov);
}

/* ---- VFS Operations ---- */

int overlay_vfs_stat(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st) {
    char norm[1024];
    ov_resolution_t res;
    int rc;

    if (!ctx || !ctx->overlay || !path || !out_st) return -EINVAL;
    rc = overlay_resolve(ctx, path, norm, sizeof(norm), &res);
    if (rc != 0) return rc;
    if (!res.exists) return -ENOENT;

    if (res.in_upper) {
        fill_stat_from_upper(ctx, norm, res.upper_path_w,
                             res.in_lower ? res.lower_st.ino : 0, out_st);
    } else {
        *out_st = res.lower_st;
        out_st->ino = overlay_assign_ino(ctx, norm, res.lower_st.ino);
        /* In an active RW overlay, lower-only files are logically writable via copy-up
         * unless the POSIX mode lacks write bits or the context is configured read-only. */
        if (ctx->read_only) {
            out_st->file_attributes |= FILE_ATTRIBUTE_READONLY;
        } else if (out_st->mode & 0222) {
            out_st->file_attributes &= ~FILE_ATTRIBUTE_READONLY;
        }
    }
    return 0;
}

int overlay_vfs_stat_ino(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const char *leaf_name, ext4_stat_t *out_st) {
    char norm[1024];
    int rc;
    (void)leaf_name;

    if (!ctx || !ctx->overlay || !out_st || ino == 0) return -EINVAL;
    rc = overlay_lookup_ino(ctx, ino, norm, sizeof(norm), NULL);
    if (rc != 0) return rc;
    return overlay_vfs_stat(ctx, norm, out_st);
}

int overlay_vfs_set_times(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint32_t win_attrs,
                          uint64_t ctime_ft, uint64_t atime_ft, uint64_t mtime_ft, uint64_t chgtime_ft) {
    char norm[1024];
    ov_resolution_t res;
    HANDLE hf;
    int rc;
    (void)chgtime_ft;

    if (!ctx || !ctx->overlay || ino == 0) return -EINVAL;
    if (ctx->read_only) return -EROFS;

    rc = overlay_lookup_ino(ctx, ino, norm, sizeof(norm), NULL);
    if (rc != 0) return rc;

    EnterCriticalSection(&ctx->overlay->mut_cs);
    rc = ensure_upper_copy(ctx, norm, &res);
    if (rc == 0) {
        DWORD flags = res.is_dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
        hf = CreateFileW(res.upper_path_w, FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, flags, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            rc = win32_err_to_errno(GetLastError());
        } else {
            FILETIME ct = u64_to_ft(ctime_ft);
            FILETIME at = u64_to_ft(atime_ft);
            FILETIME wt = u64_to_ft(mtime_ft);
            SetFileTime(hf, ctime_ft ? &ct : NULL, atime_ft ? &at : NULL, mtime_ft ? &wt : NULL);
            CloseHandle(hf);
            if (win_attrs != INVALID_FILE_ATTRIBUTES) {
                DWORD final_attrs = win_attrs;
                if (res.is_dir) final_attrs |= FILE_ATTRIBUTE_DIRECTORY;
                else final_attrs &= ~FILE_ATTRIBUTE_DIRECTORY;
                SetFileAttributesW(res.upper_path_w, final_attrs);
            }
        }
    }
    LeaveCriticalSection(&ctx->overlay->mut_cs);
    return rc;
}

typedef struct ov_name_list {
    char  **names;
    size_t  count;
    size_t  cap;
} ov_name_list_t;

static int ov_name_list_add(ov_name_list_t *list, const char *name) {
    char *dup;
    if (list->count == list->cap) {
        size_t ncap = list->cap ? list->cap * 2 : 32;
        char **grown = (char **)realloc(list->names, ncap * sizeof(char *));
        if (!grown) return -ENOMEM;
        list->names = grown;
        list->cap = ncap;
    }
    dup = _strdup(name);
    if (!dup) return -ENOMEM;
    list->names[list->count++] = dup;
    return 0;
}

static int ov_name_list_contains(const ext4_vfs_ctx_t *ctx, const ov_name_list_t *list, const char *name) {
    for (size_t i = 0; i < list->count; i++) {
        if (ctx->case_insensitive ? (_stricmp(list->names[i], name) == 0)
                                  : (strcmp(list->names[i], name) == 0)) {
            return 1;
        }
    }
    return 0;
}

static void ov_name_list_free(ov_name_list_t *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->names[i]);
    }
    free(list->names);
    memset(list, 0, sizeof(*list));
}

typedef struct ov_lower_readdir_ctx {
    ext4_vfs_ctx_t *ctx;
    const char     *dir_norm;
    ov_name_list_t *seen_upper;
    ov_name_list_t *whiteouts;
    ext4_readdir_cb cb;
    void           *user_ctx;
    int             stopped;
} ov_lower_readdir_ctx_t;

static void join_child_norm(const char *parent_norm, const char *name, char *out, size_t out_sz) {
    if (strcmp(parent_norm, "/") == 0) {
        _snprintf_s(out, out_sz, _TRUNCATE, "/%s", name);
    } else {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s/%s", parent_norm, name);
    }
}

static int ov_lower_readdir_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    ov_lower_readdir_ctx_t *lrc = (ov_lower_readdir_ctx_t *)user_ctx;
    ext4_stat_t adjusted;
    char child_norm[1024];

    if (lrc->stopped) return 1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (is_reserved_wh_name(name)) return 0;
    if (ov_name_list_contains(lrc->ctx, lrc->whiteouts, name)) return 0;
    if (ov_name_list_contains(lrc->ctx, lrc->seen_upper, name)) return 0;

    join_child_norm(lrc->dir_norm, name, child_norm, sizeof(child_norm));
    adjusted = *st;
    adjusted.ino = overlay_assign_ino(lrc->ctx, child_norm, st->ino);
    if (lrc->ctx->read_only) {
        adjusted.file_attributes |= FILE_ATTRIBUTE_READONLY;
    } else if (adjusted.mode & 0222) {
        adjusted.file_attributes &= ~FILE_ATTRIBUTE_READONLY;
    }

    if (lrc->cb(name, &adjusted, lrc->user_ctx) != 0) {
        lrc->stopped = 1;
        return 1;
    }
    return 0;
}

int overlay_vfs_readdir(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, ext4_readdir_cb cb, void *user_ctx) {
    char dir_norm[1024];
    ov_resolution_t res;
    ov_name_list_t seen_upper;
    ov_name_list_t whiteouts;
    int rc;
    int stopped = 0;

    if (!ctx || !ctx->overlay || !cb || dir_ino == 0) return -EINVAL;

    rc = overlay_lookup_ino(ctx, dir_ino, dir_norm, sizeof(dir_norm), NULL);
    if (rc != 0) return rc;

    rc = overlay_resolve(ctx, dir_norm, NULL, 0, &res);
    if (rc != 0) return rc;
    if (!res.exists) return -ENOENT;
    if (!res.is_dir) return -ENOTDIR;

    memset(&seen_upper, 0, sizeof(seen_upper));
    memset(&whiteouts, 0, sizeof(whiteouts));

    /* Emit "." and ".." */
    {
        ext4_stat_t dot_st, dotdot_st;
        char parent[1024], leaf[EXT2_NAME_LEN + 1];

        rc = overlay_vfs_stat(ctx, dir_norm, &dot_st);
        if (rc != 0) return rc;
        if (cb(".", &dot_st, user_ctx) != 0) return 0;

        if (strcmp(dir_norm, "/") == 0) {
            dotdot_st = dot_st;
        } else {
            ext4_vfs_split_parent(dir_norm, parent, sizeof(parent), leaf, sizeof(leaf));
            if (overlay_vfs_stat(ctx, parent, &dotdot_st) != 0) {
                dotdot_st = dot_st;
            }
        }
        if (cb("..", &dotdot_st, user_ctx) != 0) return 0;
    }

    /* 1. Scan Upper NTFS directory if present */
    if (res.in_upper) {
        wchar_t pattern[1040];
        WIN32_FIND_DATAW fd;
        HANDLE hfind;

        _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%ls\\*", res.upper_path_w);
        hfind = FindFirstFileW(pattern, &fd);
        if (hfind != INVALID_HANDLE_VALUE) {
            do {
                char utf8_name[EXT2_NAME_LEN * 4 + 1];
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                    continue;
                }
                if (ext4_wide_to_utf8(fd.cFileName, utf8_name, sizeof(utf8_name)) != 0) {
                    continue;
                }
                if (strncmp(utf8_name, OV_WHITEOUT_PREFIX, 4) == 0) {
                    if (strcmp(utf8_name, OV_OPAQUE_NAME) != 0 &&
                        strcmp(utf8_name, OV_LOWER_ID_NAME) != 0) {
                        ov_name_list_add(&whiteouts, utf8_name + 4);
                    }
                    continue;
                }
                if (strlen(utf8_name) > EXT2_NAME_LEN) continue;

                {
                    char child_norm[1024];
                    wchar_t child_w[1040];
                    ext4_stat_t cst;
                    ext2_ino_t lower_child_ino = 0;

                    join_child_norm(dir_norm, utf8_name, child_norm, sizeof(child_norm));
                    _snwprintf_s(child_w, _countof(child_w), _TRUNCATE, L"%ls\\%ls",
                                 res.upper_path_w, fd.cFileName);
                    if (res.lower_scope_active && res.in_lower &&
                        !ov_name_list_contains(ctx, &whiteouts, utf8_name)) {
                        ext4_stat_t lst;
                        if (ext4_vfs_lower_stat(ctx, child_norm, &lst) == 0) {
                            lower_child_ino = lst.ino;
                        }
                    }
                    fill_stat_from_upper(ctx, child_norm, child_w, lower_child_ino, &cst);
                    ov_name_list_add(&seen_upper, utf8_name);
                    if (cb(utf8_name, &cst, user_ctx) != 0) {
                        stopped = 1;
                        break;
                    }
                }
            } while (FindNextFileW(hfind, &fd));
            FindClose(hfind);
        }
    }

    /* 2. Merge Lower ext4 directory entries if not opaque or shadowed */
    if (!stopped && res.lower_scope_active && res.in_lower && res.lower_st.ino != 0) {
        ov_lower_readdir_ctx_t lrc;
        lrc.ctx = ctx;
        lrc.dir_norm = dir_norm;
        lrc.seen_upper = &seen_upper;
        lrc.whiteouts = &whiteouts;
        lrc.cb = cb;
        lrc.user_ctx = user_ctx;
        lrc.stopped = 0;
        rc = ext4_vfs_lower_readdir(ctx, res.lower_st.ino, ov_lower_readdir_cb, &lrc);
    }

    ov_name_list_free(&seen_upper);
    ov_name_list_free(&whiteouts);
    return rc;
}

int overlay_vfs_open(ext4_vfs_ctx_t *ctx, const char *path, int writable, ext4_file_handle_t **out_handle) {
    char norm[1024];
    ov_resolution_t res;
    ext4_stat_t st;
    ext4_file_handle_t *h;
    int rc;

    if (!ctx || !ctx->overlay || !path || !out_handle) return -EINVAL;
    *out_handle = NULL;
    if (writable && ctx->read_only) return -EROFS;

    rc = overlay_resolve(ctx, path, norm, sizeof(norm), &res);
    if (rc != 0) return rc;
    if (!res.exists) return -ENOENT;

    if (writable && !res.in_upper && !res.is_dir) {
        EnterCriticalSection(&ctx->overlay->mut_cs);
        rc = ensure_upper_copy(ctx, norm, &res);
        LeaveCriticalSection(&ctx->overlay->mut_cs);
        if (rc != 0) return rc;
    }

    rc = overlay_vfs_stat(ctx, norm, &st);
    if (rc != 0) return rc;

    h = (ext4_file_handle_t *)calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    h->ino = st.ino;
    h->is_dir = st.is_dir;
    h->open_writable = writable;
    strcpy_s(h->path, sizeof(h->path), norm);
    *out_handle = h;
    return 0;
}

int overlay_vfs_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                     uint32_t length, uint32_t *out_read) {
    char norm[1024];
    ov_resolution_t res;
    int rc;

    if (!ctx || !ctx->overlay || !out_read || ino == 0) return -EINVAL;
    *out_read = 0;
    if (length == 0) return 0;
    if (!buf) return -EINVAL;

    rc = overlay_lookup_ino(ctx, ino, norm, sizeof(norm), NULL);
    if (rc != 0) return rc;

    rc = overlay_resolve(ctx, norm, NULL, 0, &res);
    if (rc != 0) return rc;
    if (!res.exists) return -ENOENT;
    if (res.is_dir) return -EISDIR;

    if (res.in_upper) {
        uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
        DWORD got_bytes = 0;
        int urc = 0;
        HANDLE hf = CreateFileW(res.upper_path_w, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            urc = win32_err_to_errno(GetLastError());
        } else {
            LARGE_INTEGER fsz;
            if (!GetFileSizeEx(hf, &fsz)) {
                urc = win32_err_to_errno(GetLastError());
            } else if (offset < (uint64_t)fsz.QuadPart) {
                uint64_t avail = (uint64_t)fsz.QuadPart - offset;
                DWORD want = (avail < length) ? (DWORD)avail : length;
                OVERLAPPED ovp;
                memset(&ovp, 0, sizeof(ovp));
                ovp.Offset = (DWORD)(offset & 0xFFFFFFFFULL);
                ovp.OffsetHigh = (DWORD)(offset >> 32);
                if (!ReadFile(hf, buf, want, &got_bytes, &ovp)) {
                    DWORD err = GetLastError();
                    if (err != ERROR_HANDLE_EOF) {
                        urc = win32_err_to_errno(err);
                    }
                }
            }
            CloseHandle(hf);
        }
        if (_zt0) {
            ztrace_record_complete("overlay", "overlay_read_upper", _zt0, ztrace_now_qpc() - _zt0,
                                   (uint64_t)got_bytes, 1, (int64_t)urc, ZTRACE_ARGS_OVERLAY);
        }
        if (urc == 0) *out_read = got_bytes;
        return urc;
    }

    return ext4_vfs_lower_read(ctx, res.lower_st.ino, buf, offset, length, out_read);
}

int overlay_vfs_write(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                      uint32_t length, int write_to_eof, uint32_t *out_written, uint64_t *out_new_size) {
    char norm[1024];
    ov_resolution_t res;
    int rc;

    if (!ctx || !ctx->overlay || !out_written || ino == 0) return -EINVAL;
    *out_written = 0;
    if (out_new_size) *out_new_size = 0;
    if (ctx->read_only) return -EROFS;
    if (length > 0 && !buf) return -EINVAL;

    rc = overlay_lookup_ino(ctx, ino, norm, sizeof(norm), NULL);
    if (rc != 0) return rc;

    EnterCriticalSection(&ctx->overlay->mut_cs);
    rc = ensure_upper_copy(ctx, norm, &res);
    if (rc == 0) {
        if (res.is_dir) {
            rc = -EISDIR;
        } else {
            uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
            HANDLE hf = CreateFileW(res.upper_path_w, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf == INVALID_HANDLE_VALUE) {
                rc = win32_err_to_errno(GetLastError());
            } else {
                LARGE_INTEGER cur_sz;
                uint64_t write_off = offset;
                if (!GetFileSizeEx(hf, &cur_sz)) {
                    cur_sz.QuadPart = 0;
                }
                if (write_to_eof) {
                    write_off = (uint64_t)cur_sz.QuadPart;
                }
                if (length > 0) {
                    DWORD put = 0;
                    OVERLAPPED ovp;
                    memset(&ovp, 0, sizeof(ovp));
                    ovp.Offset = (DWORD)(write_off & 0xFFFFFFFFULL);
                    ovp.OffsetHigh = (DWORD)(write_off >> 32);
                    if (!WriteFile(hf, buf, length, &put, &ovp)) {
                        rc = win32_err_to_errno(GetLastError());
                    } else {
                        *out_written = put;
                    }
                }
                if (rc == 0 && out_new_size) {
                    LARGE_INTEGER final_sz;
                    if (GetFileSizeEx(hf, &final_sz)) {
                        *out_new_size = (uint64_t)final_sz.QuadPart;
                    }
                }
                CloseHandle(hf);
            }
            if (_zt0) {
                ztrace_record_complete("overlay", "overlay_write_upper", _zt0, ztrace_now_qpc() - _zt0,
                                       (uint64_t)(*out_written), 1, (int64_t)rc, ZTRACE_ARGS_OVERLAY);
            }
        }
    }
    LeaveCriticalSection(&ctx->overlay->mut_cs);
    return rc;
}

int overlay_vfs_create_file(ext4_vfs_ctx_t *ctx, const char *path, int is_dir, uint16_t mode,
                            uint32_t win_attrs, ext2_ino_t *out_ino) {
    char norm[1024];
    char parent[1024], leaf[EXT2_NAME_LEN + 1];
    ov_resolution_t parent_res, target_res;
    wchar_t wh_w[1024];
    int rc;
    (void)mode;

    if (!ctx || !ctx->overlay || !path) return -EINVAL;
    if (ctx->read_only) return -EROFS;

    rc = overlay_resolve(ctx, path, norm, sizeof(norm), &target_res);
    if (rc != 0) return rc;
    if (strcmp(norm, "/") == 0) return -EEXIST;
    if (target_res.exists) return -EEXIST;

    rc = ext4_vfs_split_parent(norm, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != 0) return rc;
    if (is_reserved_wh_name(leaf)) return -EACCES;

    EnterCriticalSection(&ctx->overlay->mut_cs);
    rc = overlay_resolve(ctx, parent, NULL, 0, &parent_res);
    if (rc != 0) goto out_unlock;
    if (!parent_res.exists) { rc = -ENOENT; goto out_unlock; }
    if (!parent_res.is_dir) { rc = -ENOTDIR; goto out_unlock; }

    rc = ensure_upper_parents_w(ctx->overlay, norm);
    if (rc != 0) goto out_unlock;

    if (make_whiteout_path_w(ctx->overlay, norm, wh_w, _countof(wh_w)) == 0 &&
        file_exists_w(wh_w, NULL)) {
        DeleteFileW(wh_w);
    }

    if (is_dir) {
        if (!CreateDirectoryW(target_res.upper_path_w, NULL)) {
            rc = win32_err_to_errno(GetLastError());
            goto out_unlock;
        }
        if (ctx->overlay->case_sensitive) {
            rc = ensure_dir_case_sensitive_w(target_res.upper_path_w);
            if (rc != 0) {
                RemoveDirectoryW(target_res.upper_path_w);
                goto out_unlock;
            }
        }
        /* If a lower directory of the same path existed before deletion/blocking, mark new upper
         * directory as opaque so old lower contents never reappear. */
        if (parent_res.lower_scope_active && parent_res.in_lower) {
            ext4_stat_t lower_old;
            if (ext4_vfs_lower_stat(ctx, norm, &lower_old) == 0 && lower_old.is_dir) {
                wchar_t opq_w[1024];
                if (make_opaque_path_w(ctx->overlay, norm, opq_w, _countof(opq_w)) == 0) {
                    create_marker_file_w(opq_w);
                }
            }
        }
    } else {
        DWORD attrs = (win_attrs & FILE_ATTRIBUTE_HIDDEN)
                          ? (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN)
                          : FILE_ATTRIBUTE_ARCHIVE;
        HANDLE hf = CreateFileW(target_res.upper_path_w, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                CREATE_NEW, attrs, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            rc = win32_err_to_errno(GetLastError());
            goto out_unlock;
        }
        CloseHandle(hf);
    }

    if (out_ino) {
        *out_ino = overlay_assign_ino(ctx, norm, 0);
    }
    rc = 0;

out_unlock:
    LeaveCriticalSection(&ctx->overlay->mut_cs);
    return rc;
}

int overlay_vfs_mkdir(ext4_vfs_ctx_t *ctx, const char *path, uint16_t mode, ext2_ino_t *out_ino) {
    return overlay_vfs_create_file(ctx, path, 1, mode, FILE_ATTRIBUTE_DIRECTORY, out_ino);
}

typedef struct ov_empty_check {
    int has_entry;
} ov_empty_check_t;

static int ov_empty_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    ov_empty_check_t *chk = (ov_empty_check_t *)user_ctx;
    (void)st;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    chk->has_entry = 1;
    return 1;
}

int overlay_vfs_can_delete(ext4_vfs_ctx_t *ctx, ext2_ino_t ino) {
    char norm[1024];
    ov_resolution_t res;
    int rc;

    if (!ctx || !ctx->overlay || ino == 0) return -EINVAL;
    if (ino == EXT2_ROOT_INO) return -EACCES;

    rc = overlay_lookup_ino(ctx, ino, norm, sizeof(norm), NULL);
    if (rc != 0) return rc;

    rc = overlay_resolve(ctx, norm, NULL, 0, &res);
    if (rc != 0) return rc;
    if (!res.exists) return -ENOENT;

    if (res.is_dir) {
        ov_empty_check_t chk = {0};
        rc = overlay_vfs_readdir(ctx, ino, ov_empty_cb, &chk);
        if (rc != 0) return rc;
        if (chk.has_entry) return -ENOTEMPTY;
    }
    return 0;
}

int overlay_vfs_unlink(ext4_vfs_ctx_t *ctx, const char *path) {
    char norm[1024];
    char parent[1024], leaf[EXT2_NAME_LEN + 1];
    ov_resolution_t res, parent_res;
    int rc;

    if (!ctx || !ctx->overlay || !path) return -EINVAL;
    if (ctx->read_only) return -EROFS;

    rc = overlay_resolve(ctx, path, norm, sizeof(norm), &res);
    if (rc != 0) return rc;
    if (strcmp(norm, "/") == 0) return -EACCES;
    if (!res.exists) return -ENOENT;

    if (res.is_dir) {
        ov_empty_check_t chk = {0};
        ext2_ino_t dir_ino = overlay_assign_ino(ctx, norm, res.in_lower ? res.lower_st.ino : 0);
        rc = overlay_vfs_readdir(ctx, dir_ino, ov_empty_cb, &chk);
        if (rc != 0) return rc;
        if (chk.has_entry) return -ENOTEMPTY;
    }

    rc = ext4_vfs_split_parent(norm, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != 0) return rc;

    EnterCriticalSection(&ctx->overlay->mut_cs);
    rc = overlay_resolve(ctx, parent, NULL, 0, &parent_res);
    if (rc != 0) goto out_unlock;

    if (res.in_upper) {
        rc = delete_tree_w(res.upper_path_w);
        if (rc != 0) goto out_unlock;
    }

    /* Check if the lower directory itself has a visible entry with this name */
    if (parent_res.lower_scope_active && parent_res.in_lower) {
        ext4_stat_t lower_old;
        if (ext4_vfs_lower_stat(ctx, norm, &lower_old) == 0) {
            wchar_t wh_w[1024];
            rc = ensure_upper_parents_w(ctx->overlay, norm);
            if (rc != 0) goto out_unlock;
            rc = make_whiteout_path_w(ctx->overlay, norm, wh_w, _countof(wh_w));
            if (rc != 0) goto out_unlock;
            rc = create_marker_file_w(wh_w);
            if (rc != 0) goto out_unlock;
        }
    }
    rc = 0;

out_unlock:
    LeaveCriticalSection(&ctx->overlay->mut_cs);
    return rc;
}

static int copy_lower_tree_to_upper(ext4_vfs_ctx_t *ctx, const char *norm_dir);

typedef struct ov_copy_tree_ctx {
    ext4_vfs_ctx_t *ctx;
    const char     *src_dir_norm;
    int             err;
} ov_copy_tree_ctx_t;

static int ov_copy_tree_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    ov_copy_tree_ctx_t *ctc = (ov_copy_tree_ctx_t *)user_ctx;
    char child_norm[1024];
    ov_resolution_t child_res;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (is_reserved_wh_name(name)) return 0;

    join_child_norm(ctc->src_dir_norm, name, child_norm, sizeof(child_norm));
    if (st->is_dir) {
        ctc->err = ensure_upper_copy(ctc->ctx, child_norm, &child_res);
        if (ctc->err == 0) {
            ctc->err = copy_lower_tree_to_upper(ctc->ctx, child_norm);
        }
    } else {
        ctc->err = ensure_upper_copy(ctc->ctx, child_norm, &child_res);
    }
    return ctc->err ? 1 : 0;
}

static int copy_lower_tree_to_upper(ext4_vfs_ctx_t *ctx, const char *norm_dir) {
    ov_resolution_t res;
    ov_copy_tree_ctx_t ctc;
    int rc = ensure_upper_copy(ctx, norm_dir, &res);
    if (rc != 0) return rc;
    if (!res.lower_scope_active || !res.in_lower) return 0;

    ctc.ctx = ctx;
    ctc.src_dir_norm = norm_dir;
    ctc.err = 0;
    rc = ext4_vfs_lower_readdir(ctx, res.lower_st.ino, ov_copy_tree_cb, &ctc);
    if (ctc.err != 0) return ctc.err;
    return rc;
}

int overlay_vfs_rename(ext4_vfs_ctx_t *ctx, const char *old_path, const char *new_path, int replace_if_exists) {
    char old_norm[1024], new_norm[1024];
    ov_resolution_t old_res, new_res, old_parent_res, new_parent_res;
    char new_parent[1024], new_leaf[EXT2_NAME_LEN + 1];
    char old_parent[1024], old_leaf[EXT2_NAME_LEN + 1];
    wchar_t new_wh_w[1024];
    int rc;

    if (!ctx || !ctx->overlay || !old_path || !new_path) return -EINVAL;
    if (ctx->read_only) return -EROFS;

    rc = overlay_resolve(ctx, old_path, old_norm, sizeof(old_norm), &old_res);
    if (rc != 0) return rc;
    rc = overlay_resolve(ctx, new_path, new_norm, sizeof(new_norm), &new_res);
    if (rc != 0) return rc;

    if (strcmp(old_norm, "/") == 0 || strcmp(new_norm, "/") == 0) return -EINVAL;
    if (!old_res.exists) return -ENOENT;
    if (same_path(ctx, old_norm, new_norm)) return 0;

    rc = ext4_vfs_split_parent(new_norm, new_parent, sizeof(new_parent), new_leaf, sizeof(new_leaf));
    if (rc != 0) return rc;
    if (is_reserved_wh_name(new_leaf)) return -EACCES;
    rc = ext4_vfs_split_parent(old_norm, old_parent, sizeof(old_parent), old_leaf, sizeof(old_leaf));
    if (rc != 0) return rc;

    EnterCriticalSection(&ctx->overlay->mut_cs);
    rc = overlay_resolve(ctx, new_parent, NULL, 0, &new_parent_res);
    if (rc != 0) goto out_unlock;
    if (!new_parent_res.exists) { rc = -ENOENT; goto out_unlock; }
    if (!new_parent_res.is_dir) { rc = -ENOTDIR; goto out_unlock; }

    rc = overlay_resolve(ctx, old_parent, NULL, 0, &old_parent_res);
    if (rc != 0) goto out_unlock;

    if (new_res.exists) {
        if (!replace_if_exists) { rc = -EEXIST; goto out_unlock; }
        if (new_res.is_dir) {
            ov_empty_check_t chk = {0};
            ext2_ino_t dst_ino = overlay_assign_ino(ctx, new_norm, new_res.in_lower ? new_res.lower_st.ino : 0);
            rc = overlay_vfs_readdir(ctx, dst_ino, ov_empty_cb, &chk);
            if (rc != 0) goto out_unlock;
            if (chk.has_entry) { rc = -ENOTEMPTY; goto out_unlock; }
        }
        if (new_res.in_upper) {
            rc = delete_tree_w(new_res.upper_path_w);
            if (rc != 0) goto out_unlock;
        }
    }

    /* Materialize old_norm completely in Upper before moving */
    if (old_res.is_dir) {
        rc = copy_lower_tree_to_upper(ctx, old_norm);
    } else {
        rc = ensure_upper_copy(ctx, old_norm, &old_res);
    }
    if (rc != 0) goto out_unlock;

    rc = ensure_upper_parents_w(ctx->overlay, new_norm);
    if (rc != 0) goto out_unlock;

    if (make_whiteout_path_w(ctx->overlay, new_norm, new_wh_w, _countof(new_wh_w)) == 0 &&
        file_exists_w(new_wh_w, NULL)) {
        DeleteFileW(new_wh_w);
    }

    if (!MoveFileExW(old_res.upper_path_w, new_res.upper_path_w, MOVEFILE_REPLACE_EXISTING)) {
        rc = win32_err_to_errno(GetLastError());
        goto out_unlock;
    }

    /* If moving a directory and new_norm also existed as a lower dir, mark new_norm opaque */
    if (old_res.is_dir && new_parent_res.lower_scope_active && new_parent_res.in_lower) {
        ext4_stat_t lower_dst;
        if (ext4_vfs_lower_stat(ctx, new_norm, &lower_dst) == 0 && lower_dst.is_dir) {
            wchar_t opq_w[1024];
            if (make_opaque_path_w(ctx->overlay, new_norm, opq_w, _countof(opq_w)) == 0) {
                create_marker_file_w(opq_w);
            }
        }
    }

    /* Whiteout old_norm if it still exists in Lower */
    if (old_parent_res.lower_scope_active && old_parent_res.in_lower) {
        ext4_stat_t lower_old;
        if (ext4_vfs_lower_stat(ctx, old_norm, &lower_old) == 0) {
            wchar_t old_wh_w[1024];
            if (make_whiteout_path_w(ctx->overlay, old_norm, old_wh_w, _countof(old_wh_w)) == 0) {
                create_marker_file_w(old_wh_w);
            }
        }
    }

    overlay_rename_map(ctx, old_norm, new_norm);
    rc = 0;

out_unlock:
    LeaveCriticalSection(&ctx->overlay->mut_cs);
    return rc;
}

int overlay_vfs_truncate(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint64_t new_size) {
    char norm[1024];
    ov_resolution_t res;
    int rc;

    if (!ctx || !ctx->overlay || ino == 0) return -EINVAL;
    if (ctx->read_only) return -EROFS;

    rc = overlay_lookup_ino(ctx, ino, norm, sizeof(norm), NULL);
    if (rc != 0) return rc;

    EnterCriticalSection(&ctx->overlay->mut_cs);
    rc = ensure_upper_copy(ctx, norm, &res);
    if (rc == 0) {
        if (res.is_dir) {
            rc = -EISDIR;
        } else {
            HANDLE hf = CreateFileW(res.upper_path_w, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf == INVALID_HANDLE_VALUE) {
                rc = win32_err_to_errno(GetLastError());
            } else {
                LARGE_INTEGER li;
                li.QuadPart = (LONGLONG)new_size;
                if (!SetFilePointerEx(hf, li, NULL, FILE_BEGIN) || !SetEndOfFile(hf)) {
                    rc = win32_err_to_errno(GetLastError());
                }
                CloseHandle(hf);
            }
        }
    }
    LeaveCriticalSection(&ctx->overlay->mut_cs);
    return rc;
}

int overlay_vfs_statfs(ext4_vfs_ctx_t *ctx, ext4_volume_stat_t *out_vol) {
    int rc = ext4_vfs_lower_statfs(ctx, out_vol);
    if (rc != 0) return rc;
    if (ctx->overlay && !ctx->read_only) {
        ULARGE_INTEGER avail, total_upper, free_upper;
        if (GetDiskFreeSpaceExW(ctx->overlay->upper_dir_w, &avail, &total_upper, &free_upper)) {
            /* Report writable NTFS upperdir free space alongside total capacity */
            out_vol->free_bytes = avail.QuadPart;
        }
        out_vol->read_only = 0;
    }
    return 0;
}

int overlay_vfs_flush(ext4_vfs_ctx_t *ctx) {
    (void)ctx;
    return 0;
}
