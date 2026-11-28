#include "overlay_tools.h"
#include "../common/ext4_errno.h"
#include "../common/win_namespace.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OV_TOOLS_LOWER_ID_NAME_W  L".wh.__ext4_lower_id"
#define OV_TOOLS_OPAQUE_NAME      ".wh.__dir_opaque"
#define OV_TOOLS_OPAQUE_NAME_W    L".wh.__dir_opaque"
#define OV_TOOLS_WHITEOUT_PREFIX  ".wh."
#define OV_TOOLS_MAX_WALK_DEPTH   128

static int tools_win32_err_to_errno(DWORD err) {
    switch (err) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NO_MORE_FILES: return -ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: return -EACCES;
    case ERROR_DIR_NOT_EMPTY: return -ENOTEMPTY;
    case ERROR_DIRECTORY: return -ENOTDIR;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return -ENOSPC;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME: return -EINVAL;
    default: return -EIO;
    }
}

static uint64_t filetime_to_unix_sec(const FILETIME *ft) {
    ULARGE_INTEGER ui;
    ui.LowPart = ft->dwLowDateTime;
    ui.HighPart = ft->dwHighDateTime;
    if (ui.QuadPart < 116444736000000000ULL) return 0;
    return (ui.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

static int normalize_dir_full_w(const char *dir_utf8, wchar_t *out_w, size_t out_cch) {
    wchar_t wdir[1024];
    DWORD full_len;
    size_t len;
    if (!dir_utf8 || !dir_utf8[0] || !out_w || out_cch == 0) return -EINVAL;
    if (ext4_utf8_to_wide(dir_utf8, wdir, _countof(wdir)) != 0) return -EINVAL;
    full_len = GetFullPathNameW(wdir, (DWORD)out_cch, out_w, NULL);
    if (full_len == 0 || full_len >= out_cch) return -EINVAL;
    len = wcslen(out_w);
    while (len > 3 && (out_w[len - 1] == L'\\' || out_w[len - 1] == L'/')) {
        out_w[--len] = L'\0';
    }
    if (len == 3 && out_w[1] == L':' && (out_w[2] == L'\\' || out_w[2] == L'/')) {
        return -EINVAL;
    }
    return 0;
}

static void join_logical_child(const char *parent_norm, const char *leaf, char *out, size_t out_sz) {
    if (strcmp(parent_norm, "/") == 0) {
        _snprintf_s(out, out_sz, _TRUNCATE, "/%s", leaf);
    } else {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s/%s", parent_norm, leaf);
    }
}

static int is_temp_copyup_name(const char *name) {
    return (name && strstr(name, ".ext4tmp_") != NULL) ? 1 : 0;
}

const char *overlay_diff_kind_name(overlay_diff_kind_t kind) {
    switch (kind) {
    case OV_DIFF_ADDED:      return "added";
    case OV_DIFF_MODIFIED:   return "modified";
    case OV_DIFF_DELETED:    return "deleted";
    case OV_DIFF_OPAQUE_DIR: return "opaque_dir";
    case OV_DIFF_UPPER:      return "upper";
    default:                 return "unknown";
    }
}

static int diff_report_add_entry(overlay_diff_report_t *rep, overlay_diff_kind_t kind,
                                 const char *path, int is_dir, uint64_t size,
                                 uint32_t mode, uint64_t mtime_unix) {
    if (rep->entry_count == rep->entry_cap) {
        size_t ncap = rep->entry_cap ? rep->entry_cap * 2 : 64;
        overlay_diff_entry_t *nb = (overlay_diff_entry_t *)realloc(rep->entries, ncap * sizeof(*nb));
        if (!nb) return -ENOMEM;
        rep->entries = nb;
        rep->entry_cap = ncap;
    }
    {
        overlay_diff_entry_t *e = &rep->entries[rep->entry_count++];
        memset(e, 0, sizeof(*e));
        e->kind = kind;
        strncpy_s(e->path, sizeof(e->path), path, _TRUNCATE);
        e->is_dir = is_dir ? 1 : 0;
        e->size = size;
        e->mode = mode;
        e->mtime_unix = mtime_unix;
    }
    return 0;
}

typedef struct ov_walk_frame {
    wchar_t          pattern[1040];
    wchar_t          child_w[1040];
    WIN32_FIND_DATAW fd;
} ov_walk_frame_t;

static int scan_upper_recursive(const wchar_t *dir_w, const char *dir_norm, int opaque_ancestor,
                                ext4_vfs_ctx_t *lower_ctx, int collect_entries,
                                int depth, overlay_diff_report_t *rep) {
    ov_walk_frame_t *fr;
    HANDLE hfind;
    int cur_dir_opaque = opaque_ancestor;
    int rc = 0;

    if (depth > OV_TOOLS_MAX_WALK_DEPTH) return -ELOOP;
    fr = (ov_walk_frame_t *)calloc(1, sizeof(*fr));
    if (!fr) return -ENOMEM;

    /* Check whether this directory itself is marked opaque */
    _snwprintf_s(fr->child_w, _countof(fr->child_w), _TRUNCATE, L"%ls\\%ls", dir_w, OV_TOOLS_OPAQUE_NAME_W);
    if (GetFileAttributesW(fr->child_w) != INVALID_FILE_ATTRIBUTES) {
        cur_dir_opaque = 1;
    }

    _snwprintf_s(fr->pattern, _countof(fr->pattern), _TRUNCATE, L"%ls\\*", dir_w);
    hfind = FindFirstFileW(fr->pattern, &fr->fd);
    if (hfind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(fr);
        return (err == ERROR_NO_MORE_FILES) ? 0 : tools_win32_err_to_errno(err);
    }

    do {
        char leaf[1024];
        char child_norm[1024];
        int is_dir;
        uint64_t fsize;
        uint64_t mtime_unix;

        if (wcscmp(fr->fd.cFileName, L".") == 0 || wcscmp(fr->fd.cFileName, L"..") == 0) {
            continue;
        }
        if (depth == 0 && wcscmp(fr->fd.cFileName, OV_TOOLS_LOWER_ID_NAME_W) == 0) {
            continue;
        }
        if (ext4_wide_to_utf8(fr->fd.cFileName, leaf, sizeof(leaf)) != 0) {
            continue;
        }
        if (is_temp_copyup_name(leaf)) {
            continue;
        }

        _snwprintf_s(fr->child_w, _countof(fr->child_w), _TRUNCATE, L"%ls\\%ls", dir_w, fr->fd.cFileName);
        is_dir = (fr->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        fsize = is_dir ? 0ULL : (((uint64_t)fr->fd.nFileSizeHigh << 32) | (uint64_t)fr->fd.nFileSizeLow);
        mtime_unix = filetime_to_unix_sec(&fr->fd.ftLastWriteTime);

        if (strcmp(leaf, OV_TOOLS_OPAQUE_NAME) == 0) {
            rep->summary.opaque_dir_count++;
            if (collect_entries) {
                rc = diff_report_add_entry(rep, OV_DIFF_OPAQUE_DIR, dir_norm, 1, 0, 0040755, mtime_unix);
                if (rc != 0) break;
            }
            continue;
        }

        if (strncmp(leaf, OV_TOOLS_WHITEOUT_PREFIX, 4) == 0) {
            const char *target_leaf = leaf + 4;
            if (target_leaf[0] != '\0') {
                join_logical_child(dir_norm, target_leaf, child_norm, sizeof(child_norm));
                rep->summary.deleted_count++;
                if (collect_entries) {
                    rc = diff_report_add_entry(rep, OV_DIFF_DELETED, child_norm, 0, 0, 0, mtime_unix);
                    if (rc != 0) break;
                }
            }
            continue;
        }

        join_logical_child(dir_norm, leaf, child_norm, sizeof(child_norm));
        if (is_dir) {
            rep->summary.dir_count++;
        } else {
            rep->summary.file_count++;
            rep->summary.total_upper_bytes += fsize;
        }

        {
            overlay_diff_kind_t kind = OV_DIFF_UPPER;
            uint32_t mode = is_dir ? 0040755U : 0100644U;
            if (lower_ctx) {
                ext4_stat_t lst;
                if (!cur_dir_opaque && ext4_vfs_lower_stat(lower_ctx, child_norm, &lst) == 0) {
                    kind = OV_DIFF_MODIFIED;
                    rep->summary.modified_count++;
                    mode = lst.mode;
                } else {
                    kind = OV_DIFF_ADDED;
                    rep->summary.added_count++;
                }
            } else {
                rep->summary.upper_count++;
            }

            if (collect_entries) {
                rc = diff_report_add_entry(rep, kind, child_norm, is_dir, fsize, mode, mtime_unix);
                if (rc != 0) break;
            }
        }

        if (is_dir) {
            rc = scan_upper_recursive(fr->child_w, child_norm, cur_dir_opaque,
                                      lower_ctx, collect_entries, depth + 1, rep);
            if (rc != 0) break;
        }
    } while (FindNextFileW(hfind, &fr->fd));

    FindClose(hfind);
    free(fr);
    return rc;
}

int overlay_tools_scan(const char *upper_dir_utf8,
                       ext4_vfs_ctx_t *lower_ctx_opt,
                       const overlay_lower_identity_t *expected_opt,
                       int collect_entries,
                       overlay_diff_report_t *out_report) {
    wchar_t full_w[1024];
    DWORD attrs;
    int rc;

    if (!upper_dir_utf8 || !out_report) return -EINVAL;
    memset(out_report, 0, sizeof(*out_report));

    overlay_check_upper_dir(upper_dir_utf8, expected_opt, &out_report->upper_check);
    rc = normalize_dir_full_w(upper_dir_utf8, full_w, _countof(full_w));
    if (rc != 0) return rc;

    attrs = GetFileAttributesW(full_w);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return 0; /* Non-existent directory: empty summary, upper_check explains state */
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return -ENOTDIR;
    }

    rc = scan_upper_recursive(full_w, "/", 0, lower_ctx_opt, collect_entries, 0, out_report);
    if (rc != 0) {
        overlay_tools_free_diff_report(out_report);
    }
    return rc;
}

void overlay_tools_free_diff_report(overlay_diff_report_t *report) {
    if (!report) return;
    free(report->entries);
    report->entries = NULL;
    report->entry_count = 0;
    report->entry_cap = 0;
}

static int validate_add_issue(overlay_validate_report_t *rep, const char *severity,
                              const char *code, const char *path, const char *msg) {
    if (rep->issue_count == rep->issue_cap) {
        size_t ncap = rep->issue_cap ? rep->issue_cap * 2 : 32;
        overlay_validate_issue_t *nb = (overlay_validate_issue_t *)realloc(rep->issues, ncap * sizeof(*nb));
        if (!nb) return -ENOMEM;
        rep->issues = nb;
        rep->issue_cap = ncap;
    }
    {
        overlay_validate_issue_t *iss = &rep->issues[rep->issue_count++];
        memset(iss, 0, sizeof(*iss));
        strcpy_s(iss->severity, sizeof(iss->severity), severity);
        strcpy_s(iss->code, sizeof(iss->code), code);
        strncpy_s(iss->path, sizeof(iss->path), path ? path : "/", _TRUNCATE);
        strncpy_s(iss->message, sizeof(iss->message), msg, _TRUNCATE);
    }
    if (strcmp(severity, "error") == 0) {
        rep->error_count++;
    } else {
        rep->warning_count++;
    }
    return 0;
}

static int validate_upper_recursive(const wchar_t *dir_w, const char *dir_norm,
                                    ext4_vfs_ctx_t *lower_ctx, int depth,
                                    overlay_validate_report_t *rep) {
    ov_walk_frame_t *fr;
    HANDLE hfind;
    int rc = 0;

    if (depth > OV_TOOLS_MAX_WALK_DEPTH) {
        return validate_add_issue(rep, "error", "max_depth_exceeded", dir_norm,
                                  "Upperdir directory nesting exceeds maximum depth.");
    }

    fr = (ov_walk_frame_t *)calloc(1, sizeof(*fr));
    if (!fr) return -ENOMEM;

    _snwprintf_s(fr->pattern, _countof(fr->pattern), _TRUNCATE, L"%ls\\*", dir_w);
    hfind = FindFirstFileW(fr->pattern, &fr->fd);
    if (hfind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(fr);
        return (err == ERROR_NO_MORE_FILES) ? 0 : tools_win32_err_to_errno(err);
    }

    do {
        char leaf[1024];
        char child_norm[1024];
        int is_dir;

        if (wcscmp(fr->fd.cFileName, L".") == 0 || wcscmp(fr->fd.cFileName, L"..") == 0) {
            continue;
        }
        if (depth == 0 && wcscmp(fr->fd.cFileName, OV_TOOLS_LOWER_ID_NAME_W) == 0) {
            continue;
        }
        if (ext4_wide_to_utf8(fr->fd.cFileName, leaf, sizeof(leaf)) != 0) {
            rc = validate_add_issue(rep, "error", "invalid_utf8", dir_norm,
                                    "Upperdir entry name cannot be converted to UTF-8.");
            if (rc != 0) break;
            continue;
        }

        join_logical_child(dir_norm, leaf, child_norm, sizeof(child_norm));
        _snwprintf_s(fr->child_w, _countof(fr->child_w), _TRUNCATE, L"%ls\\%ls", dir_w, fr->fd.cFileName);
        is_dir = (fr->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

        if (fr->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            rc = validate_add_issue(rep, "warn", "upper_reparse_point", child_norm,
                                    "Upperdir contains an NTFS reparse point which is treated as a regular entry.");
            if (rc != 0) break;
        }

        if (is_temp_copyup_name(leaf)) {
            rc = validate_add_issue(rep, "warn", "stale_temp_file", child_norm,
                                    "Leftover temporary copy-up file (.ext4tmp_*) found in upperdir.");
            if (rc != 0) break;
            continue;
        }

        if (strcmp(leaf, OV_TOOLS_OPAQUE_NAME) == 0) {
            if (is_dir) {
                rc = validate_add_issue(rep, "error", "invalid_opaque_marker", child_norm,
                                        "Opaque marker .wh.__dir_opaque must be a regular file, not a directory.");
                if (rc != 0) break;
            }
            continue;
        }

        if (strncmp(leaf, OV_TOOLS_WHITEOUT_PREFIX, 4) == 0) {
            const char *target_leaf = leaf + 4;
            if (target_leaf[0] == '\0') {
                rc = validate_add_issue(rep, "error", "invalid_whiteout", child_norm,
                                        "Whiteout marker '.wh.' has an empty target name.");
                if (rc != 0) break;
                continue;
            }
            if (strlen(target_leaf) > EXT2_NAME_LEN) {
                rc = validate_add_issue(rep, "error", "path_too_long", child_norm,
                                        "Whiteout target name exceeds ext4 255-byte component limit.");
                if (rc != 0) break;
                continue;
            }
            if (lower_ctx) {
                char logical_target[1024];
                ext4_stat_t lst;
                join_logical_child(dir_norm, target_leaf, logical_target, sizeof(logical_target));
                if (ext4_vfs_lower_stat(lower_ctx, logical_target, &lst) != 0) {
                    rc = validate_add_issue(rep, "warn", "orphan_whiteout", logical_target,
                                            "Whiteout entry has no matching object in lower ext4 filesystem.");
                    if (rc != 0) break;
                }
            }
            continue;
        }

        if (strlen(leaf) > EXT2_NAME_LEN) {
            rc = validate_add_issue(rep, "error", "path_too_long", child_norm,
                                    "Filename exceeds ext4 255-byte UTF-8 component limit.");
            if (rc != 0) break;
        } else {
            ext4_win_name_issue_t win_issue = ext4_win_check_component(leaf);
            if (win_issue != EXT4_WIN_NAME_OK) {
                rc = validate_add_issue(rep, "warn", "invalid_win_name", child_norm,
                                        "Filename uses a Windows-incompatible character or reserved device name.");
                if (rc != 0) break;
            }
        }

        if (is_dir && !(fr->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            rc = validate_upper_recursive(fr->child_w, child_norm, lower_ctx, depth + 1, rep);
            if (rc != 0) break;
        }
    } while (FindNextFileW(hfind, &fr->fd));

    FindClose(hfind);
    free(fr);
    return rc;
}

int overlay_tools_validate(const char *upper_dir_utf8,
                           ext4_vfs_ctx_t *lower_ctx_opt,
                           const overlay_lower_identity_t *expected_opt,
                           overlay_validate_report_t *out_report) {
    wchar_t full_w[1024];
    DWORD attrs;
    int rc;

    if (!upper_dir_utf8 || !out_report) return -EINVAL;
    memset(out_report, 0, sizeof(*out_report));

    overlay_check_upper_dir(upper_dir_utf8, expected_opt, &out_report->upper_check);
    rc = normalize_dir_full_w(upper_dir_utf8, full_w, _countof(full_w));
    if (rc != 0) {
        validate_add_issue(out_report, "error", "invalid_upper_path", upper_dir_utf8,
                           out_report->upper_check.reason);
        out_report->valid = 0;
        return 0;
    }

    attrs = GetFileAttributesW(full_w);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!out_report->upper_check.can_mount) {
            validate_add_issue(out_report, "error", "marker_error", upper_dir_utf8,
                               out_report->upper_check.reason);
        } else {
            validate_add_issue(out_report, "warn", "upper_not_created", upper_dir_utf8,
                               "Upperdir does not exist yet (will be created on mount).");
        }
        out_report->valid = (out_report->error_count == 0) ? 1 : 0;
        return 0;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        validate_add_issue(out_report, "error", "not_a_directory", upper_dir_utf8,
                           "Specified upperdir path is not a directory.");
        out_report->valid = 0;
        return 0;
    }

    if (!out_report->upper_check.can_mount) {
        validate_add_issue(out_report, "error", overlay_upper_status_name(out_report->upper_check.status),
                           upper_dir_utf8, out_report->upper_check.reason);
    }

    rc = validate_upper_recursive(full_w, "/", lower_ctx_opt, 0, out_report);
    if (rc != 0) {
        overlay_tools_free_validate_report(out_report);
        return rc;
    }
    out_report->valid = (out_report->error_count == 0) ? 1 : 0;
    return 0;
}

void overlay_tools_free_validate_report(overlay_validate_report_t *report) {
    if (!report) return;
    free(report->issues);
    report->issues = NULL;
    report->issue_count = 0;
    report->issue_cap = 0;
}

/* ---- USTAR / PAX .tar Exporter ---- */

static int tar_write_all(FILE *fp, const void *data, size_t len, uint64_t *io_total) {
    if (len == 0) return 0;
    if (fwrite(data, 1, len, fp) != len) return -EIO;
    *io_total += (uint64_t)len;
    return 0;
}

static int tar_write_zero_pad(FILE *fp, size_t data_len, uint64_t *io_total) {
    uint8_t pad[512];
    size_t rem = data_len % 512U;
    if (rem == 0) return 0;
    memset(pad, 0, sizeof(pad));
    return tar_write_all(fp, pad, 512U - rem, io_total);
}

static void tar_build_header(uint8_t hdr[512], const char *name, char typeflag,
                             uint32_t mode, uint64_t size, uint64_t mtime_unix) {
    unsigned int sum = 0;
    memset(hdr, 0, 512);
    strncpy_s((char *)hdr, 100, name, _TRUNCATE);
    _snprintf_s((char *)(hdr + 100), 8, _TRUNCATE, "%07o", mode & 07777U);
    _snprintf_s((char *)(hdr + 108), 8, _TRUNCATE, "%07o", 0U);
    _snprintf_s((char *)(hdr + 116), 8, _TRUNCATE, "%07o", 0U);
    _snprintf_s((char *)(hdr + 124), 12, _TRUNCATE, "%011llo", (unsigned long long)size);
    _snprintf_s((char *)(hdr + 136), 12, _TRUNCATE, "%011llo", (unsigned long long)mtime_unix);
    memset(hdr + 148, ' ', 8);
    hdr[156] = (uint8_t)typeflag;
    memcpy(hdr + 257, "ustar\0", 6);
    memcpy(hdr + 263, "00", 2);
    strcpy_s((char *)(hdr + 265), 32, "zeroext4");
    strcpy_s((char *)(hdr + 297), 32, "zeroext4");
    for (int i = 0; i < 512; i++) sum += hdr[i];
    _snprintf_s((char *)(hdr + 148), 8, _TRUNCATE, "%06o", sum);
    hdr[154] = '\0';
    hdr[155] = ' ';
}

static int tar_write_pax_if_long(FILE *fp, const char *tar_path, uint64_t mtime_unix, uint64_t *io_total) {
    size_t plen = strlen(tar_path);
    if (plen <= 99U) return 0;

    {
        char rec[1400];
        char pax_name[128];
        uint8_t pax_hdr[512];
        size_t total_len = 0;
        for (int pass = 0; pass < 3; pass++) {
            size_t body = strlen(" path=") + plen + 1U;
            size_t digits = 1;
            size_t tmp = total_len ? total_len : (body + 4U);
            while (tmp >= 10U) { digits++; tmp /= 10U; }
            total_len = digits + body;
        }
        _snprintf_s(rec, sizeof(rec), _TRUNCATE, "%zu path=%s\n", total_len, tar_path);
        _snprintf_s(pax_name, sizeof(pax_name), _TRUNCATE, "PaxHeaders/%u", (unsigned)GetCurrentProcessId());
        tar_build_header(pax_hdr, pax_name, 'x', 0644U, (uint64_t)strlen(rec), mtime_unix);
        if (tar_write_all(fp, pax_hdr, 512, io_total) != 0) return -EIO;
        if (tar_write_all(fp, rec, strlen(rec), io_total) != 0) return -EIO;
        if (tar_write_zero_pad(fp, strlen(rec), io_total) != 0) return -EIO;
    }
    return 0;
}

typedef struct str_builder {
    char  *buf;
    size_t len;
    size_t cap;
    int    oom;
} str_builder_t;

static void sb_append(str_builder_t *sb, const char *s) {
    size_t slen;
    if (sb->oom || !s) return;
    slen = strlen(s);
    if (sb->len + slen + 1U > sb->cap) {
        size_t ncap = sb->cap ? sb->cap * 2U : 4096U;
        while (ncap < sb->len + slen + 1U) ncap *= 2U;
        char *nb = (char *)realloc(sb->buf, ncap);
        if (!nb) {
            sb->oom = 1;
            return;
        }
        sb->buf = nb;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void sb_append_json_str(str_builder_t *sb, const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    char esc[8];
    sb_append(sb, "\"");
    while (*p && !sb->oom) {
        unsigned char c = *p++;
        switch (c) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\b': sb_append(sb, "\\b"); break;
        case '\f': sb_append(sb, "\\f"); break;
        case '\n': sb_append(sb, "\\n"); break;
        case '\r': sb_append(sb, "\\r"); break;
        case '\t': sb_append(sb, "\\t"); break;
        default:
            if (c < 0x20U) {
                _snprintf_s(esc, sizeof(esc), _TRUNCATE, "\\u%04x", (unsigned)c);
                sb_append(sb, esc);
            } else {
                esc[0] = (char)c;
                esc[1] = '\0';
                sb_append(sb, esc);
            }
            break;
        }
    }
    sb_append(sb, "\"");
}

static int build_manifest_json(const overlay_diff_report_t *rep, char **out_json, size_t *out_len) {
    str_builder_t sb;
    char num[256];
    memset(&sb, 0, sizeof(sb));

    sb_append(&sb, "{\n  \"format_version\": 1,\n");
    sb_append(&sb, "  \"lower_id_status\": ");
    sb_append_json_str(&sb, overlay_upper_status_name(rep->upper_check.status));
    sb_append(&sb, ",\n  \"expected_id\": ");
    sb_append_json_str(&sb, rep->upper_check.expected_id);
    sb_append(&sb, ",\n  \"found_id\": ");
    sb_append_json_str(&sb, rep->upper_check.found_id);
    _snprintf_s(num, sizeof(num), _TRUNCATE,
                ",\n  \"summary\": {\"added\": %llu, \"modified\": %llu, \"deleted\": %llu, "
                "\"opaque_dirs\": %llu, \"upper_only\": %llu, \"total_upper_bytes\": %llu},\n",
                (unsigned long long)rep->summary.added_count,
                (unsigned long long)rep->summary.modified_count,
                (unsigned long long)rep->summary.deleted_count,
                (unsigned long long)rep->summary.opaque_dir_count,
                (unsigned long long)rep->summary.upper_count,
                (unsigned long long)rep->summary.total_upper_bytes);
    sb_append(&sb, num);
    sb_append(&sb, "  \"entries\": [\n");

    for (size_t i = 0; i < rep->entry_count && !sb.oom; i++) {
        const overlay_diff_entry_t *e = &rep->entries[i];
        sb_append(&sb, "    {\"kind\": ");
        sb_append_json_str(&sb, overlay_diff_kind_name(e->kind));
        sb_append(&sb, ", \"path\": ");
        sb_append_json_str(&sb, e->path);
        _snprintf_s(num, sizeof(num), _TRUNCATE,
                    ", \"is_dir\": %s, \"size\": %llu, \"mode\": %u, \"mtime\": %llu}%s\n",
                    e->is_dir ? "true" : "false",
                    (unsigned long long)e->size,
                    (unsigned)e->mode,
                    (unsigned long long)e->mtime_unix,
                    (i + 1 < rep->entry_count) ? "," : "");
        sb_append(&sb, num);
    }
    sb_append(&sb, "  ]\n}\n");

    if (sb.oom || !sb.buf) {
        free(sb.buf);
        return -ENOMEM;
    }
    *out_json = sb.buf;
    *out_len = sb.len;
    return 0;
}

static int join_upper_rel_w(const wchar_t *upper_w, const char *norm_path, wchar_t *out_w, size_t out_cch) {
    wchar_t rel_w[1024];
    if (ext4_utf8_to_wide(norm_path, rel_w, _countof(rel_w)) != 0) return -EINVAL;
    for (size_t i = 0; rel_w[i]; i++) {
        if (rel_w[i] == L'/') rel_w[i] = L'\\';
    }
    _snwprintf_s(out_w, out_cch, _TRUNCATE, L"%ls%ls", upper_w, rel_w);
    return 0;
}

int overlay_tools_export_tar(const char *upper_dir_utf8,
                             ext4_vfs_ctx_t *lower_ctx_opt,
                             const overlay_lower_identity_t *expected_opt,
                             const char *output_tar_utf8,
                             overlay_export_summary_t *out_summary) {
    overlay_diff_report_t rep;
    wchar_t upper_w[1024];
    wchar_t out_w[1024];
    FILE *fp = NULL;
    char *manifest = NULL;
    size_t manifest_len = 0;
    uint8_t *io_buf = NULL;
    const size_t io_buf_sz = 65536U;
    uint64_t total_bytes = 0;
    uint64_t exported_cnt = 0;
    int rc;

    if (!upper_dir_utf8 || !output_tar_utf8 || !output_tar_utf8[0]) return -EINVAL;
    if (out_summary) memset(out_summary, 0, sizeof(*out_summary));

    rc = normalize_dir_full_w(upper_dir_utf8, upper_w, _countof(upper_w));
    if (rc != 0) return rc;
    if (ext4_utf8_to_wide(output_tar_utf8, out_w, _countof(out_w)) != 0) return -EINVAL;

    rc = overlay_tools_scan(upper_dir_utf8, lower_ctx_opt, expected_opt, 1, &rep);
    if (rc != 0) return rc;

    rc = build_manifest_json(&rep, &manifest, &manifest_len);
    if (rc != 0) {
        overlay_tools_free_diff_report(&rep);
        return rc;
    }

    io_buf = (uint8_t *)malloc(io_buf_sz);
    if (!io_buf) {
        free(manifest);
        overlay_tools_free_diff_report(&rep);
        return -ENOMEM;
    }

    if (_wfopen_s(&fp, out_w, L"wb") != 0 || !fp) {
        free(io_buf);
        free(manifest);
        overlay_tools_free_diff_report(&rep);
        return -EIO;
    }

    /* 1. Write manifest.json */
    {
        uint8_t hdr[512];
        tar_build_header(hdr, "manifest.json", '0', 0644U, (uint64_t)manifest_len, 0);
        rc = tar_write_all(fp, hdr, 512, &total_bytes);
        if (rc == 0) rc = tar_write_all(fp, manifest, manifest_len, &total_bytes);
        if (rc == 0) rc = tar_write_zero_pad(fp, manifest_len, &total_bytes);
        if (rc == 0) exported_cnt++;
    }

    /* 2. Write files/ directories and modified/added files */
    for (size_t i = 0; i < rep.entry_count && rc == 0; i++) {
        const overlay_diff_entry_t *e = &rep.entries[i];
        char tar_path[1100];
        uint8_t hdr[512];

        if (e->kind != OV_DIFF_ADDED && e->kind != OV_DIFF_MODIFIED && e->kind != OV_DIFF_UPPER) {
            continue;
        }
        {
            const char *rel = (e->path[0] == '/') ? (e->path + 1) : e->path;
            if (rel[0] == '\0') continue;
            if (e->is_dir) {
                _snprintf_s(tar_path, sizeof(tar_path), _TRUNCATE, "files/%s/", rel);
                rc = tar_write_pax_if_long(fp, tar_path, e->mtime_unix, &total_bytes);
                if (rc != 0) break;
                tar_build_header(hdr, tar_path, '5', e->mode ? e->mode : 0755U, 0, e->mtime_unix);
                rc = tar_write_all(fp, hdr, 512, &total_bytes);
                if (rc == 0) exported_cnt++;
            } else {
                wchar_t src_w[1040];
                HANDLE hf;
                LARGE_INTEGER fsz;
                uint64_t remaining;

                rc = join_upper_rel_w(upper_w, e->path, src_w, _countof(src_w));
                if (rc != 0) break;
                hf = CreateFileW(src_w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
                if (hf == INVALID_HANDLE_VALUE) {
                    rc = tools_win32_err_to_errno(GetLastError());
                    break;
                }
                if (!GetFileSizeEx(hf, &fsz)) {
                    CloseHandle(hf);
                    rc = tools_win32_err_to_errno(GetLastError());
                    break;
                }
                remaining = (uint64_t)fsz.QuadPart;

                _snprintf_s(tar_path, sizeof(tar_path), _TRUNCATE, "files/%s", rel);
                rc = tar_write_pax_if_long(fp, tar_path, e->mtime_unix, &total_bytes);
                if (rc == 0) {
                    tar_build_header(hdr, tar_path, '0', e->mode ? e->mode : 0644U, remaining, e->mtime_unix);
                    rc = tar_write_all(fp, hdr, 512, &total_bytes);
                }
                while (rc == 0 && remaining > 0) {
                    DWORD want = (remaining < io_buf_sz) ? (DWORD)remaining : (DWORD)io_buf_sz;
                    DWORD got = 0;
                    if (!ReadFile(hf, io_buf, want, &got, NULL) || got == 0) {
                        rc = -EIO;
                        break;
                    }
                    rc = tar_write_all(fp, io_buf, got, &total_bytes);
                    remaining -= got;
                }
                CloseHandle(hf);
                if (rc == 0) {
                    rc = tar_write_zero_pad(fp, (size_t)((uint64_t)fsz.QuadPart % 512ULL), &total_bytes);
                }
                if (rc == 0) exported_cnt++;
            }
        }
    }

    /* 3. Two 512-byte zero EOF blocks */
    if (rc == 0) {
        uint8_t eof_blocks[1024];
        memset(eof_blocks, 0, sizeof(eof_blocks));
        rc = tar_write_all(fp, eof_blocks, sizeof(eof_blocks), &total_bytes);
    }

    fclose(fp);
    free(io_buf);
    free(manifest);

    if (rc != 0) {
        DeleteFileW(out_w);
    } else if (out_summary) {
        out_summary->diff_summary = rep.summary;
        out_summary->exported_entries = exported_cnt;
        out_summary->archive_bytes = total_bytes;
    }

    overlay_tools_free_diff_report(&rep);
    return rc;
}

static int reset_delete_tree_w(const wchar_t *path_w, uint64_t *io_removed) {
    DWORD attrs = GetFileAttributesW(path_w);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(path_w, attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!DeleteFileW(path_w)) return tools_win32_err_to_errno(GetLastError());
        if (io_removed) (*io_removed)++;
        return 0;
    }

    {
        ov_walk_frame_t *fr = (ov_walk_frame_t *)calloc(1, sizeof(*fr));
        HANDLE hfind;
        int rc = 0;
        if (!fr) return -ENOMEM;

        _snwprintf_s(fr->pattern, _countof(fr->pattern), _TRUNCATE, L"%ls\\*", path_w);
        hfind = FindFirstFileW(fr->pattern, &fr->fd);
        if (hfind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fr->fd.cFileName, L".") == 0 || wcscmp(fr->fd.cFileName, L"..") == 0) {
                    continue;
                }
                _snwprintf_s(fr->child_w, _countof(fr->child_w), _TRUNCATE, L"%ls\\%ls",
                             path_w, fr->fd.cFileName);
                rc = reset_delete_tree_w(fr->child_w, io_removed);
                if (rc != 0) break;
            } while (FindNextFileW(hfind, &fr->fd));
            FindClose(hfind);
        }
        free(fr);
        if (rc != 0) return rc;
    }

    if (!RemoveDirectoryW(path_w)) {
        return tools_win32_err_to_errno(GetLastError());
    }
    if (io_removed) (*io_removed)++;
    return 0;
}

int overlay_tools_reset_upper(const char *upper_dir_utf8,
                              int keep_marker,
                              uint64_t *out_removed_entries) {
    overlay_upper_check_t chk;
    wchar_t full_w[1024];
    DWORD attrs;
    ov_walk_frame_t *fr;
    HANDLE hfind;
    uint64_t removed = 0;
    int rc;

    if (out_removed_entries) *out_removed_entries = 0;
    if (!upper_dir_utf8 || !upper_dir_utf8[0]) return -EINVAL;

    rc = normalize_dir_full_w(upper_dir_utf8, full_w, _countof(full_w));
    if (rc != 0) return rc;

    attrs = GetFileAttributesW(full_w);
    if (attrs == INVALID_FILE_ATTRIBUTES) return -ENOENT;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -ENOTDIR;

    overlay_check_upper_dir(upper_dir_utf8, NULL, &chk);
    /* Safety guard: only wipe directories that bear a valid ZeroExt4 v1/v2 marker (or are already empty). */
    if (chk.status != OV_UPPER_OK_EMPTY && chk.marker_version != 1 && chk.marker_version != 2) {
        ext4_log(EXT4_LOG_ERROR,
                 "Refusing overlay reset on '%s': %s", upper_dir_utf8, chk.reason);
        return (chk.status == OV_UPPER_NON_EMPTY_UNMARKED) ? -ENOTEMPTY : -EACCES;
    }

    fr = (ov_walk_frame_t *)calloc(1, sizeof(*fr));
    if (!fr) return -ENOMEM;

    _snwprintf_s(fr->pattern, _countof(fr->pattern), _TRUNCATE, L"%ls\\*", full_w);
    hfind = FindFirstFileW(fr->pattern, &fr->fd);
    if (hfind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(fr);
        return (err == ERROR_NO_MORE_FILES) ? 0 : tools_win32_err_to_errno(err);
    }

    rc = 0;
    do {
        if (wcscmp(fr->fd.cFileName, L".") == 0 || wcscmp(fr->fd.cFileName, L"..") == 0) {
            continue;
        }
        if (keep_marker && wcscmp(fr->fd.cFileName, OV_TOOLS_LOWER_ID_NAME_W) == 0) {
            continue;
        }
        _snwprintf_s(fr->child_w, _countof(fr->child_w), _TRUNCATE, L"%ls\\%ls", full_w, fr->fd.cFileName);
        rc = reset_delete_tree_w(fr->child_w, &removed);
        if (rc != 0) break;
    } while (FindNextFileW(hfind, &fr->fd));

    FindClose(hfind);
    free(fr);
    if (rc == 0 && out_removed_entries) {
        *out_removed_entries = removed;
    }
    return rc;
}
