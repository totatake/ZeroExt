#include "winfsp_driver.h"
#include "../common/ext4_errno.h"
#include "../common/win_namespace.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <sddl.h>
#include <shlobj.h>
#include <winver.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE     0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE    0x00001000
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif

struct winfsp_mount_handle {
    FSP_FILE_SYSTEM *fs;
    ext4_vfs_ctx_t  *vfs;
    WCHAR            drive[3];        /* "X:" when mounted as a drive letter, else empty */
    WCHAR            volume_name[256];/* \Device\Volume{...} target of user-session DOS device */
    int              user_dosdev;     /* 1 if DOS device was defined in desktop user's session */
};

static PSECURITY_DESCRIPTOR g_default_sd = NULL;
static DWORD g_default_sd_len = 0;

static void ensure_default_sd(void) {
    if (g_default_sd) return;
    PSECURITY_DESCRIPTOR sd = NULL;
    ULONG sd_len = 0;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:(A;;FA;;;WD)", SDDL_REVISION_1, &sd, &sd_len)) {
        g_default_sd = sd;
        g_default_sd_len = sd_len;
    }
}

static void fill_fsp_file_info(const ext4_stat_t *st, int symlink_is_dir, FSP_FSCTL_FILE_INFO *fi) {
    memset(fi, 0, sizeof(*fi));
    fi->FileAttributes = st->file_attributes;
    fi->ReparseTag = 0;
    if (st->is_symlink) {
        fi->FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
        if (symlink_is_dir) {
            fi->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        }
        fi->ReparseTag = IO_REPARSE_TAG_SYMLINK;
        fi->AllocationSize = 0;
        fi->FileSize = 0;
    } else {
        fi->AllocationSize = st->alloc_size;
        fi->FileSize = st->size;
    }
    fi->CreationTime = st->creation_time;
    fi->LastAccessTime = st->last_access_time;
    fi->LastWriteTime = st->last_write_time;
    fi->ChangeTime = st->change_time;
    fi->IndexNumber = st->ino;
    fi->HardLinks = st->hard_links;
    fi->EaSize = 0;
}

static int split_norm_segments(const char *norm_path, char segs[64][EXT2_NAME_LEN + 1], int *out_cnt) {
    const char *p = norm_path;
    int cnt = 0;
    if (!norm_path || norm_path[0] != '/') return -EINVAL;
    while (*p) {
        size_t len = 0;
        if (*p == '/') {
            p++;
            continue;
        }
        while (p[len] && p[len] != '/') {
            if (len >= EXT2_NAME_LEN) return -ENAMETOOLONG;
            len++;
        }
        if (cnt >= 64) return -ENAMETOOLONG;
        memcpy(segs[cnt], p, len);
        segs[cnt][len] = '\0';
        cnt++;
        p += len;
    }
    *out_cnt = cnt;
    return 0;
}

static int resolve_posix_target_in_volume(const char *link_norm_path, const char *target_utf8,
                                          char *out_norm, size_t out_sz,
                                          char (*out_win_rel_buf)[1024]) {
    char par_path[1024], dummy_leaf[EXT2_NAME_LEN + 1];
    char (*segs)[EXT2_NAME_LEN + 1] = NULL;
    char (*par_segs)[EXT2_NAME_LEN + 1] = NULL;
    int par_cnt = 0;
    int depth = 0;
    int min_depth = 0;
    int is_abs;
    const char *p;
    int rc = 0;

    if (!link_norm_path || !target_utf8 || target_utf8[0] == '\0') return -EINVAL;

    /* Reject Windows drive letters, UNC paths, backslashes, or invalid control/reserved chars */
    if (((target_utf8[0] >= 'a' && target_utf8[0] <= 'z') ||
         (target_utf8[0] >= 'A' && target_utf8[0] <= 'Z')) &&
        target_utf8[1] == ':') {
        return -EACCES;
    }
    if (strchr(target_utf8, '\\') != NULL) return -EINVAL;

    if (ext4_vfs_split_parent(link_norm_path, par_path, sizeof(par_path), dummy_leaf, sizeof(dummy_leaf)) != 0) {
        return -EINVAL;
    }

    segs = (char (*)[EXT2_NAME_LEN + 1])calloc(64, sizeof(*segs));
    par_segs = (char (*)[EXT2_NAME_LEN + 1])calloc(64, sizeof(*par_segs));
    if (!segs || !par_segs) {
        free(segs);
        free(par_segs);
        return -ENOMEM;
    }

    if (split_norm_segments(par_path, par_segs, &par_cnt) != 0) {
        rc = -ENAMETOOLONG;
        goto out;
    }

    is_abs = (target_utf8[0] == '/') ? 1 : 0;
    if (is_abs) {
        depth = 0;
        min_depth = 0;
    } else {
        for (int i = 0; i < par_cnt; i++) {
            strcpy_s(segs[i], EXT2_NAME_LEN + 1, par_segs[i]);
        }
        depth = par_cnt;
        min_depth = par_cnt;
    }

    p = target_utf8;
    while (*p) {
        size_t len = 0;
        char comp[EXT2_NAME_LEN + 1];
        if (*p == '/') {
            p++;
            continue;
        }
        while (p[len] && p[len] != '/') {
            if (len >= EXT2_NAME_LEN) {
                rc = -ENAMETOOLONG;
                goto out;
            }
            comp[len] = p[len];
            len++;
        }
        comp[len] = '\0';
        p += len;

        if (strcmp(comp, ".") == 0) {
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            if (depth == 0) {
                if (!is_abs) {
                    ext4_log(EXT4_LOG_WARN,
                             "Refusing symlink '%s' -> '%s': relative target escapes mounted volume root.",
                             link_norm_path, target_utf8);
                    rc = -EACCES;
                    goto out;
                }
                /* POSIX root "/.." stays at "/" */
                continue;
            }
            depth--;
            if (depth < min_depth) min_depth = depth;
            continue;
        }
        if (ext4_win_check_component(comp) != EXT4_WIN_NAME_OK) {
            rc = -EINVAL;
            goto out;
        }
        if (depth >= 64) {
            rc = -ENAMETOOLONG;
            goto out;
        }
        strcpy_s(segs[depth++], EXT2_NAME_LEN + 1, comp);
    }

    if (out_norm && out_sz > 0) {
        if (depth == 0) {
            strcpy_s(out_norm, out_sz, "/");
        } else {
            size_t pos = 0;
            out_norm[0] = '\0';
            for (int i = 0; i < depth; i++) {
                int n = _snprintf_s(out_norm + pos, out_sz - pos, _TRUNCATE, "/%s", segs[i]);
                if (n < 0 || pos + (size_t)n >= out_sz) {
                    rc = -ENAMETOOLONG;
                    goto out;
                }
                pos += (size_t)n;
            }
        }
    }

    if (out_win_rel_buf) {
        char *rel = *out_win_rel_buf;
        int common = 0;
        size_t pos = 0;
        while (common < par_cnt && common < depth &&
               strcmp(par_segs[common], segs[common]) == 0) {
            common++;
        }
        rel[0] = '\0';
        for (int i = common; i < par_cnt; i++) {
            int n = _snprintf_s(rel + pos, sizeof(*out_win_rel_buf) - pos, _TRUNCATE,
                                "%s..", (pos > 0) ? "\\" : "");
            if (n < 0 || pos + (size_t)n >= sizeof(*out_win_rel_buf)) {
                rc = -ENAMETOOLONG;
                goto out;
            }
            pos += (size_t)n;
        }
        for (int i = common; i < depth; i++) {
            int n = _snprintf_s(rel + pos, sizeof(*out_win_rel_buf) - pos, _TRUNCATE,
                                "%s%s", (pos > 0) ? "\\" : "", segs[i]);
            if (n < 0 || pos + (size_t)n >= sizeof(*out_win_rel_buf)) {
                rc = -ENAMETOOLONG;
                goto out;
            }
            pos += (size_t)n;
        }
        if (pos == 0) {
            strcpy_s(rel, sizeof(*out_win_rel_buf), ".");
        }
    }

out:
    free(segs);
    free(par_segs);
    return rc;
}

static int resolve_symlink_target_is_dir(ext4_vfs_ctx_t *ctx, const char *link_norm_path) {
    char cur[1024];
    char target[1024];
    char next[1024];
    ext4_stat_t st;

    if (!ctx || !link_norm_path) return 0;
    strcpy_s(cur, sizeof(cur), link_norm_path);
    for (int hop = 0; hop < 16; hop++) {
        if (ext4_vfs_readlink(ctx, cur, target, sizeof(target)) != 0) return 0;
        if (resolve_posix_target_in_volume(cur, target, next, sizeof(next), NULL) != 0) return 0;
        if (ext4_vfs_stat(ctx, next, &st) != 0) return 0;
        if (!st.is_symlink) return st.is_dir ? 1 : 0;
        strcpy_s(cur, sizeof(cur), next);
    }
    return 0;
}

static NTSTATUS build_symlink_reparse_data(ext4_vfs_ctx_t *ctx, const char *norm_path,
                                           PVOID Buffer, PSIZE_T PSize) {
    char target_utf8[1024];
    char rel_utf8[1024];
    wchar_t rel_w[512];
    size_t rel_cch;
    UINT16 rel_bytes;
    size_t needed;
    int rc;

    rc = ext4_vfs_readlink(ctx, norm_path, target_utf8, sizeof(target_utf8));
    if (rc == -ENOENT) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (rc != 0) return STATUS_NOT_A_REPARSE_POINT;

    if (resolve_posix_target_in_volume(norm_path, target_utf8, NULL, 0, &rel_utf8) != 0) {
        return STATUS_REPARSE_POINT_NOT_RESOLVED;
    }
    if (ext4_utf8_to_wide(rel_utf8, rel_w, _countof(rel_w)) != 0) {
        return STATUS_REPARSE_POINT_NOT_RESOLVED;
    }

    rel_cch = wcslen(rel_w);
    if (rel_cch * sizeof(WCHAR) > 16000U) {
        return STATUS_REPARSE_POINT_NOT_RESOLVED;
    }
    rel_bytes = (UINT16)(rel_cch * sizeof(WCHAR));
    needed = sizeof(FSP_SYMLINK_REPARSE_DATA_BUFFER) + 2U * (size_t)rel_bytes;

    if (!Buffer && !PSize) {
        return STATUS_SUCCESS;
    }
    if (!PSize) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Buffer || *PSize < needed) {
        *PSize = needed;
        return STATUS_BUFFER_TOO_SMALL;
    }

    {
        FSP_SYMLINK_REPARSE_DATA_BUFFER *rb = (FSP_SYMLINK_REPARSE_DATA_BUFFER *)Buffer;
        memset(rb, 0, sizeof(*rb));
        rb->ReparseTag = IO_REPARSE_TAG_SYMLINK;
        rb->ReparseDataLength = (UINT16)(12U + 2U * rel_bytes);
        rb->SubstituteNameOffset = 0;
        rb->SubstituteNameLength = rel_bytes;
        rb->PrintNameOffset = rel_bytes;
        rb->PrintNameLength = rel_bytes;
        rb->Flags = SYMLINK_FLAG_RELATIVE;
        if (rel_bytes > 0) {
            memcpy(rb->PathBuffer, rel_w, rel_bytes);
            memcpy(rb->PathBuffer + rel_cch, rel_w, rel_bytes);
        }
        *PSize = needed;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ext4_get_reparse_point_by_name(FSP_FILE_SYSTEM *FileSystem, PVOID Context,
                                               PWSTR FileName, BOOLEAN IsDirectory,
                                               PVOID Buffer, PSIZE_T PSize) {
    ext4_vfs_ctx_t *ctx = FileSystem ? (ext4_vfs_ctx_t *)FileSystem->UserContext : NULL;
    char raw_utf8[1024];
    char norm_path[1024];
    ext4_stat_t st;
    int rc;
    (void)Context;
    (void)IsDirectory;

    if (!ctx || !FileName) return STATUS_INVALID_PARAMETER;
    if (ext4_wide_to_utf8(FileName, raw_utf8, sizeof(raw_utf8)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    if (ext4_vfs_normalize_path(raw_utf8, norm_path, sizeof(norm_path)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    rc = ext4_vfs_stat(ctx, norm_path, &st);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    if (!st.is_symlink) return STATUS_NOT_A_REPARSE_POINT;
    return build_symlink_reparse_data(ctx, norm_path, Buffer, PSize);
}

static NTSTATUS copy_default_sd(PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize) {
    ensure_default_sd();
    if (!PSecurityDescriptorSize) return STATUS_SUCCESS;
    if (!g_default_sd || g_default_sd_len == 0) {
        *PSecurityDescriptorSize = 0;
        return STATUS_SUCCESS;
    }
    if (!SecurityDescriptor || *PSecurityDescriptorSize < g_default_sd_len) {
        *PSecurityDescriptorSize = g_default_sd_len;
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(SecurityDescriptor, g_default_sd, g_default_sd_len);
    *PSecurityDescriptorSize = g_default_sd_len;
    return STATUS_SUCCESS;
}

static NTSTATUS FspGetVolumeInfo(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_VOLUME_INFO *VolumeInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_volume_stat_t vol;
    int rc;
    WCHAR wlabel[32];

    if (!ctx || !VolumeInfo) return STATUS_INVALID_PARAMETER;
    rc = ext4_vfs_statfs(ctx, &vol);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    memset(VolumeInfo, 0, sizeof(*VolumeInfo));
    VolumeInfo->TotalSize = vol.total_bytes;
    VolumeInfo->FreeSize = vol.free_bytes;

    memset(wlabel, 0, sizeof(wlabel));
    if (vol.volume_label[0] != '\0') {
        ext4_utf8_to_wide(vol.volume_label, wlabel, _countof(wlabel));
    } else {
        wcscpy_s(wlabel, _countof(wlabel), L"ext4");
    }
    wcsncpy_s(VolumeInfo->VolumeLabel, _countof(VolumeInfo->VolumeLabel), wlabel, _TRUNCATE);
    VolumeInfo->VolumeLabelLength = (UINT16)(wcslen(VolumeInfo->VolumeLabel) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

static NTSTATUS FspSetVolumeLabel(FSP_FILE_SYSTEM *FileSystem, PWSTR VolumeLabel,
                                  FSP_FSCTL_VOLUME_INFO *VolumeInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    char label_utf8[64];
    int rc;

    if (!ctx) return STATUS_INVALID_PARAMETER;
    if (VolumeLabel) {
        ext4_wide_to_utf8(VolumeLabel, label_utf8, sizeof(label_utf8));
    } else {
        label_utf8[0] = '\0';
    }
    rc = ext4_vfs_set_label(ctx, label_utf8);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    return FspGetVolumeInfo(FileSystem, VolumeInfo);
}

static NTSTATUS FspGetSecurityByName_impl(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName,
                                          PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
                                          SIZE_T *PSecurityDescriptorSize) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    char path_utf8[1024];
    ext4_stat_t st;
    int rc;

    if (!ctx || !FileName) return STATUS_INVALID_PARAMETER;
    {
        const WCHAR *first = FileName;
        while (*first == L'\\') first++;
        if (wcschr(first, L'\\') != NULL) {
            UINT32 reparse_idx = 0;
            if (FspFileSystemFindReparsePoint(FileSystem, ext4_get_reparse_point_by_name, NULL,
                                              FileName, &reparse_idx) &&
                wcschr(FileName + reparse_idx, L'\\') != NULL) {
                if (PFileAttributes) *PFileAttributes = reparse_idx;
                return STATUS_REPARSE;
            }
        }
    }
    if (ext4_wide_to_utf8(FileName, path_utf8, sizeof(path_utf8)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    rc = ext4_vfs_stat(ctx, path_utf8, &st);
    if (rc != 0) {
        if (rc == -ENOENT || rc == -ENOTDIR || rc == -ELOOP) {
            UINT32 reparse_idx = 0;
            if (FspFileSystemFindReparsePoint(FileSystem, ext4_get_reparse_point_by_name, NULL,
                                              FileName, &reparse_idx)) {
                if (PFileAttributes) *PFileAttributes = reparse_idx;
                return STATUS_REPARSE;
            }
        }
        return ext4_errno_to_ntstatus(rc);
    }

    if (PFileAttributes) {
        UINT32 attrs = st.file_attributes;
        if (st.is_symlink) {
            char norm[1024];
            attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
            if (ext4_vfs_normalize_path(path_utf8, norm, sizeof(norm)) == 0 &&
                resolve_symlink_target_is_dir(ctx, norm)) {
                attrs |= FILE_ATTRIBUTE_DIRECTORY;
            }
        }
        *PFileAttributes = attrs;
    }
    if (PSecurityDescriptorSize) {
        return copy_default_sd(SecurityDescriptor, PSecurityDescriptorSize);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspGetSecurityByName(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName,
                                     PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
                                     SIZE_T *PSecurityDescriptorSize) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspGetSecurityByName_impl(FileSystem, FileName, PFileAttributes,
                                            SecurityDescriptor, PSecurityDescriptorSize);
    if (_zt0) {
        ztrace_record_complete("winfsp", "FspGetSecurityByName", _zt0, ztrace_now_qpc() - _zt0,
                               0, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspCreate_impl(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
                               UINT32 GrantedAccess, UINT32 FileAttributes,
                               PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
                               PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    char path_utf8[1024];
    int is_dir = (CreateOptions & FILE_DIRECTORY_FILE) != 0;
    ext2_ino_t ino = 0;
    ext4_file_handle_t *handle = NULL;
    ext4_stat_t st;
    int rc;
    (void)GrantedAccess;
    (void)SecurityDescriptor;
    (void)AllocationSize;

    if (!ctx || !FileName || !PFileContext) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;
    if (ext4_wide_to_utf8(FileName, path_utf8, sizeof(path_utf8)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    {
        char norm[1024];
        if (ext4_vfs_normalize_path(path_utf8, norm, sizeof(norm)) != 0 ||
            ext4_win_check_path(norm) != EXT4_WIN_NAME_OK) {
            return STATUS_OBJECT_NAME_INVALID;
        }
    }

    rc = ext4_vfs_create(ctx, path_utf8, is_dir, is_dir ? 0755 : 0644, FileAttributes, &ino);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    rc = ext4_vfs_open_stat(ctx, path_utf8, 1, &handle, FileInfo ? &st : NULL);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    handle->create_options = CreateOptions;
    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        handle->delete_on_close = 1;
    }

    if (FileInfo) {
        fill_fsp_file_info(&st, 0, FileInfo);
    }

    *PFileContext = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS FspCreate(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
                          UINT32 GrantedAccess, UINT32 FileAttributes,
                          PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
                          PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspCreate_impl(FileSystem, FileName, CreateOptions, GrantedAccess, FileAttributes,
                                 SecurityDescriptor, AllocationSize, PFileContext, FileInfo);
    if (_zt0) {
        ext4_file_handle_t *h = (NT_SUCCESS(st) && PFileContext) ? (ext4_file_handle_t *)(*PFileContext) : NULL;
        ztrace_record_complete("winfsp", "FspCreate", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspOpen_impl(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
                             UINT32 GrantedAccess, PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    char path_utf8[1024];
    ext4_file_handle_t *handle = NULL;
    ext4_stat_t st;
    int wants_write = (GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    int rc;

    if (!ctx || !FileName || !PFileContext) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only && wants_write) return STATUS_MEDIA_WRITE_PROTECTED;

    if (ext4_wide_to_utf8(FileName, path_utf8, sizeof(path_utf8)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    rc = ext4_vfs_open_stat(ctx, path_utf8, wants_write && !ctx->read_only,
                            &handle, &st);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    {
        int sym_dir = st.is_symlink ? resolve_symlink_target_is_dir(ctx, handle->path) : 0;
        if (!(CreateOptions & FILE_OPEN_REPARSE_POINT)) {
            if ((CreateOptions & FILE_DIRECTORY_FILE) && !handle->is_dir && !sym_dir) {
                ext4_vfs_close_handle(handle);
                return STATUS_NOT_A_DIRECTORY;
            }
            if ((CreateOptions & FILE_NON_DIRECTORY_FILE) && (handle->is_dir || sym_dir)) {
                ext4_vfs_close_handle(handle);
                return STATUS_FILE_IS_A_DIRECTORY;
            }
        }
        handle->create_options = CreateOptions;
        if ((CreateOptions & FILE_DELETE_ON_CLOSE) && !ctx->read_only) {
            handle->delete_on_close = 1;
        }
        if (FileInfo) {
            fill_fsp_file_info(&st, sym_dir, FileInfo);
        }
    }

    *PFileContext = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS FspOpen(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
                        UINT32 GrantedAccess, PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspOpen_impl(FileSystem, FileName, CreateOptions, GrantedAccess, PFileContext, FileInfo);
    if (_zt0) {
        ext4_file_handle_t *h = (NT_SUCCESS(st) && PFileContext) ? (ext4_file_handle_t *)(*PFileContext) : NULL;
        ztrace_record_complete("winfsp", "FspOpen", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspOverwrite_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
                                  BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize,
                                  FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    ext4_stat_t st;
    int rc;
    (void)ReplaceFileAttributes;
    (void)AllocationSize;

    if (!ctx || !handle) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;

    rc = ext4_vfs_truncate(ctx, handle->ino, 0);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    {
        uint64_t now = ext4_now_filetime();
        ext4_vfs_set_times(ctx, handle->ino, FileAttributes, 0, now, now, now);
    }

    if (FileInfo) {
        const char *leaf = strrchr(handle->path, '/');
        leaf = leaf ? (leaf + 1) : handle->path;
        if (ext4_vfs_stat_ino(ctx, handle->ino, leaf, &st) == 0) {
            int sym_dir = st.is_symlink ? resolve_symlink_target_is_dir(ctx, handle->path) : 0;
            fill_fsp_file_info(&st, sym_dir, FileInfo);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspOverwrite(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
                             BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize,
                             FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspOverwrite_impl(FileSystem, FileContext, FileAttributes,
                                    ReplaceFileAttributes, AllocationSize, FileInfo);
    if (_zt0) {
        ext4_file_handle_t *h = (ext4_file_handle_t *)FileContext;
        ztrace_record_complete("winfsp", "FspOverwrite", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static VOID FspCleanup(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    (void)FileName;

    if (ctx && handle) {
        if (!ctx->read_only && ((Flags & FspCleanupDelete) || handle->delete_on_close)) {
            ext4_vfs_unlink(ctx, handle->path);
        } else if (!ctx->read_only && (Flags & (FspCleanupSetLastWriteTime | FspCleanupSetChangeTime | FspCleanupSetLastAccessTime))) {
            uint64_t now = ext4_now_filetime();
            ext4_vfs_set_times(ctx, handle->ino, INVALID_FILE_ATTRIBUTES, 0,
                               (Flags & FspCleanupSetLastAccessTime) ? now : 0,
                               (Flags & FspCleanupSetLastWriteTime) ? now : 0,
                               (Flags & FspCleanupSetChangeTime) ? now : 0);
        }
    }
    if (_zt0) {
        ztrace_record_complete("winfsp", "FspCleanup", _zt0, ztrace_now_qpc() - _zt0,
                               handle ? (uint64_t)handle->ino : 0ULL, 0, (int64_t)Flags, ZTRACE_ARGS_STATUS);
    }
}

static VOID FspClose(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    uint64_t ino = FileContext ? (uint64_t)((ext4_file_handle_t *)FileContext)->ino : 0ULL;
    (void)FileSystem;
    ext4_vfs_close_handle((ext4_file_handle_t *)FileContext);
    if (_zt0) {
        ztrace_record_complete("winfsp", "FspClose", _zt0, ztrace_now_qpc() - _zt0,
                               ino, 0, 0, ZTRACE_ARGS_STATUS);
    }
}

static NTSTATUS FspRead_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                             UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    uint32_t got = 0;
    int rc;

    if (!ctx || !handle || !PBytesTransferred) return STATUS_INVALID_PARAMETER;
    *PBytesTransferred = 0;
    if (Length == 0) return STATUS_SUCCESS;

    rc = ext4_vfs_read_handle(ctx, handle, Buffer, Offset, Length, &got);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    if (got == 0) return STATUS_END_OF_FILE;

    if (ctx->stats_enabled) {
        InterlockedAdd64(&ctx->stat_read_bytes, (LONG64)got);
        InterlockedIncrement64(&ctx->stat_read_ops);
    }

    *PBytesTransferred = got;
    return STATUS_SUCCESS;
}

static NTSTATUS FspRead(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                        UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspRead_impl(FileSystem, FileContext, Buffer, Offset, Length, PBytesTransferred);
    if (_zt0) {
        ULONG bytes = PBytesTransferred ? *PBytesTransferred : 0;
        ztrace_record_complete("winfsp", "FspRead", _zt0, ztrace_now_qpc() - _zt0,
                               Offset, (uint64_t)bytes, (int64_t)st, ZTRACE_ARGS_IO);
    }
    return st;
}

static NTSTATUS FspWrite_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                              UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile,
                              BOOLEAN ConstrainedIo, PULONG PBytesTransferred,
                              FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    uint32_t written = 0;
    uint64_t new_size = 0;
    ULONG effective_len = Length;
    int rc;

    if (!ctx || !handle || !PBytesTransferred) return STATUS_INVALID_PARAMETER;
    *PBytesTransferred = 0;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;

    if (ConstrainedIo) {
        ext4_stat_t cur_st;
        rc = ext4_vfs_stat_ino(ctx, handle->ino, NULL, &cur_st);
        if (rc != 0) return ext4_errno_to_ntstatus(rc);
        if (Offset >= cur_st.size) {
            if (FileInfo) fill_fsp_file_info(&cur_st, 0, FileInfo);
            return STATUS_SUCCESS;
        }
        if ((UINT64)effective_len > cur_st.size - Offset) {
            effective_len = (ULONG)(cur_st.size - Offset);
        }
    }

    {
        ext4_stat_t st;
        const char *leaf = strrchr(handle->path, '/');
        leaf = leaf ? (leaf + 1) : handle->path;
        rc = ext4_vfs_write_stat(ctx, handle->ino, Buffer, Offset, effective_len,
                                 WriteToEndOfFile ? 1 : 0, leaf, &written, &new_size,
                                 FileInfo ? &st : NULL);
        if (rc != 0) return ext4_errno_to_ntstatus(rc);

        if (ctx->stats_enabled && written > 0) {
            InterlockedAdd64(&ctx->stat_write_bytes, (LONG64)written);
            InterlockedIncrement64(&ctx->stat_write_ops);
        }

        *PBytesTransferred = written;
        if (FileInfo) {
            fill_fsp_file_info(&st, 0, FileInfo);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspWrite(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                         UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile,
                         BOOLEAN ConstrainedIo, PULONG PBytesTransferred,
                         FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspWrite_impl(FileSystem, FileContext, Buffer, Offset, Length,
                                WriteToEndOfFile, ConstrainedIo, PBytesTransferred, FileInfo);
    if (_zt0) {
        ULONG bytes = PBytesTransferred ? *PBytesTransferred : 0;
        ztrace_record_complete("winfsp", "FspWrite", _zt0, ztrace_now_qpc() - _zt0,
                               Offset, (uint64_t)bytes, (int64_t)st, ZTRACE_ARGS_IO);
    }
    return st;
}

static NTSTATUS FspFlush_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                              FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    int rc;

    if (!ctx) return STATUS_INVALID_PARAMETER;
    rc = ext4_vfs_flush(ctx);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    if (FileInfo && handle) {
        ext4_stat_t st;
        const char *leaf = strrchr(handle->path, '/');
        leaf = leaf ? (leaf + 1) : handle->path;
        if (ext4_vfs_stat_ino(ctx, handle->ino, leaf, &st) == 0) {
            int sym_dir = st.is_symlink ? resolve_symlink_target_is_dir(ctx, handle->path) : 0;
            fill_fsp_file_info(&st, sym_dir, FileInfo);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspFlush(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                         FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspFlush_impl(FileSystem, FileContext, FileInfo);
    if (_zt0) {
        ext4_file_handle_t *h = (ext4_file_handle_t *)FileContext;
        ztrace_record_complete("winfsp", "FspFlush", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspGetFileInfo_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                    FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    ext4_stat_t st;
    const char *leaf;
    int sym_dir = 0;
    int rc;

    if (!ctx || !handle || !FileInfo) return STATUS_INVALID_PARAMETER;
    leaf = strrchr(handle->path, '/');
    leaf = leaf ? (leaf + 1) : handle->path;
    rc = ext4_vfs_stat_ino(ctx, handle->ino, leaf, &st);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    if (st.is_symlink) {
        sym_dir = resolve_symlink_target_is_dir(ctx, handle->path);
    }
    fill_fsp_file_info(&st, sym_dir, FileInfo);
    return STATUS_SUCCESS;
}

static NTSTATUS FspGetFileInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                               FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspGetFileInfo_impl(FileSystem, FileContext, FileInfo);
    if (_zt0) {
        ext4_file_handle_t *h = (ext4_file_handle_t *)FileContext;
        ztrace_record_complete("winfsp", "FspGetFileInfo", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspSetBasicInfo_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
                                     UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime,
                                     UINT64 ChangeTime, FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    int rc;

    if (!ctx || !handle) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;

    rc = ext4_vfs_set_times(ctx, handle->ino, FileAttributes, CreationTime,
                            LastAccessTime, LastWriteTime, ChangeTime);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    if (FileInfo) return FspGetFileInfo(FileSystem, FileContext, FileInfo);
    return STATUS_SUCCESS;
}

static NTSTATUS FspSetBasicInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
                                UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime,
                                UINT64 ChangeTime, FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspSetBasicInfo_impl(FileSystem, FileContext, FileAttributes, CreationTime,
                                       LastAccessTime, LastWriteTime, ChangeTime, FileInfo);
    if (_zt0) {
        ext4_file_handle_t *h = (ext4_file_handle_t *)FileContext;
        ztrace_record_complete("winfsp", "FspSetBasicInfo", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspSetFileSize_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize,
                                    BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    int rc;

    if (!ctx || !handle) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;

    if (SetAllocationSize) {
        ext4_stat_t st;
        rc = ext4_vfs_stat_ino(ctx, handle->ino, NULL, &st);
        if (rc != 0) return ext4_errno_to_ntstatus(rc);
        if (NewSize < st.size) {
            rc = ext4_vfs_truncate(ctx, handle->ino, NewSize);
            if (rc != 0) return ext4_errno_to_ntstatus(rc);
        }
    } else {
        rc = ext4_vfs_truncate(ctx, handle->ino, NewSize);
        if (rc != 0) return ext4_errno_to_ntstatus(rc);
    }

    if (FileInfo) return FspGetFileInfo(FileSystem, FileContext, FileInfo);
    return STATUS_SUCCESS;
}

static NTSTATUS FspSetFileSize(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize,
                               BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO *FileInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspSetFileSize_impl(FileSystem, FileContext, NewSize, SetAllocationSize, FileInfo);
    if (_zt0) {
        ext4_file_handle_t *h = (ext4_file_handle_t *)FileContext;
        ztrace_record_complete("winfsp", "FspSetFileSize", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, NewSize, (int64_t)st, ZTRACE_ARGS_RW_OP);
    }
    return st;
}

static NTSTATUS FspCanDelete(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    int rc;
    (void)FileName;

    if (!ctx || !handle) return STATUS_INVALID_PARAMETER;
    rc = ext4_vfs_can_delete(ctx, handle->ino);
    return ext4_errno_to_ntstatus(rc);
}

static NTSTATUS FspSetDelete(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                             BOOLEAN DeleteFile) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    (void)FileName;

    if (!ctx || !handle) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;

    if (DeleteFile) {
        int rc = ext4_vfs_can_delete(ctx, handle->ino);
        if (rc != 0) return ext4_errno_to_ntstatus(rc);
    }
    handle->delete_on_close = DeleteFile ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS FspRename_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                               PWSTR NewFileName, BOOLEAN ReplaceIfExists) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    char old_utf8[1024], new_utf8[1024], norm_new[1024];
    int rc;

    if (!ctx || !FileName || !NewFileName) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;
    if (ext4_wide_to_utf8(FileName, old_utf8, sizeof(old_utf8)) != 0 ||
        ext4_wide_to_utf8(NewFileName, new_utf8, sizeof(new_utf8)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    if (ext4_vfs_normalize_path(new_utf8, norm_new, sizeof(norm_new)) != 0 ||
        ext4_win_check_path(norm_new) != EXT4_WIN_NAME_OK) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    if (ReplaceIfExists) {
        ext4_stat_t src_st, dst_st;
        if (ext4_vfs_stat(ctx, old_utf8, &src_st) == 0 &&
            ext4_vfs_stat(ctx, new_utf8, &dst_st) == 0 &&
            dst_st.is_dir && dst_st.ino != src_st.ino) {
            return STATUS_ACCESS_DENIED;
        }
    }

    rc = ext4_vfs_rename(ctx, old_utf8, new_utf8, ReplaceIfExists ? 1 : 0);
    if (rc == 0 && handle) {
        if (ext4_vfs_normalize_path(new_utf8, norm_new, sizeof(norm_new)) == 0) {
            strcpy_s(handle->path, sizeof(handle->path), norm_new);
        }
    }
    return ext4_errno_to_ntstatus(rc);
}

static NTSTATUS FspRename(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
                          PWSTR NewFileName, BOOLEAN ReplaceIfExists) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspRename_impl(FileSystem, FileContext, FileName, NewFileName, ReplaceIfExists);
    if (_zt0) {
        ext4_file_handle_t *h = (ext4_file_handle_t *)FileContext;
        ztrace_record_complete("winfsp", "FspRename", _zt0, ztrace_now_qpc() - _zt0,
                               h ? (uint64_t)h->ino : 0ULL, 0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspGetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                               PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize) {
    (void)FileSystem;
    (void)FileContext;
    return copy_default_sd(SecurityDescriptor, PSecurityDescriptorSize);
}

static NTSTATUS FspSetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                               SECURITY_INFORMATION SecurityInformation,
                               PSECURITY_DESCRIPTOR ModificationDescriptor) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    (void)FileContext;
    (void)SecurityInformation;
    (void)ModificationDescriptor;
    if (ctx && ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;
    return STATUS_SUCCESS;
}

struct fsp_readdir_state {
    const wchar_t *marker;
    int            past_marker;
    PVOID          buffer;
    ULONG          length;
    PULONG         transferred;
    int            buffer_full;
};

static int fsp_readdir_cb(const char *name, const ext4_stat_t *st, void *user_ctx) {
    struct fsp_readdir_state *s = (struct fsp_readdir_state *)user_ctx;
    WCHAR wname[EXT2_NAME_LEN + 1];
    UINT8 info_buf[sizeof(FSP_FSCTL_DIR_INFO) + (EXT2_NAME_LEN + 1) * sizeof(WCHAR)];
    FSP_FSCTL_DIR_INFO *dir_info = (FSP_FSCTL_DIR_INFO *)info_buf;
    size_t wlen;

    if (ext4_utf8_to_wide(name, wname, _countof(wname)) != 0) {
        return 0;
    }

    if (s->marker && !s->past_marker) {
        if (wcscmp(wname, s->marker) == 0) {
            s->past_marker = 1;
        }
        return 0;
    }

    wlen = wcslen(wname);
    memset(info_buf, 0, sizeof(FSP_FSCTL_DIR_INFO));
    dir_info->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + wlen * sizeof(WCHAR));
    fill_fsp_file_info(st, 0, &dir_info->FileInfo);
    memcpy(dir_info->FileNameBuf, wname, wlen * sizeof(WCHAR));

    if (!FspFileSystemAddDirInfo(dir_info, s->buffer, s->length, s->transferred)) {
        s->buffer_full = 1;
        return 1; /* stop */
    }
    return 0;
}

static NTSTATUS FspReadDirectory_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR Pattern,
                                      PWSTR Marker, PVOID Buffer, ULONG Length, PULONG PBytesTransferred) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    struct fsp_readdir_state state;
    int rc;
    (void)Pattern;

    if (!ctx || !handle || !PBytesTransferred) return STATUS_INVALID_PARAMETER;
    *PBytesTransferred = 0;

    memset(&state, 0, sizeof(state));
    state.buffer = Buffer;
    state.length = Length;
    state.transferred = PBytesTransferred;

    if (!ctx->overlay && Marker && Marker[0] != L'\0') {
        char marker_utf8[EXT2_NAME_LEN * 3 + 1];
        if (ext4_wide_to_utf8(Marker, marker_utf8, sizeof(marker_utf8)) == 0) {
            state.marker = NULL;
            state.past_marker = 1;
            rc = ext4_vfs_readdir_from(ctx, handle->ino, marker_utf8, fsp_readdir_cb, &state);
        } else {
            state.marker = Marker;
            state.past_marker = 0;
            rc = ext4_vfs_readdir(ctx, handle->ino, fsp_readdir_cb, &state);
        }
    } else {
        state.marker = Marker;
        state.past_marker = (Marker == NULL);
        rc = ext4_vfs_readdir(ctx, handle->ino, fsp_readdir_cb, &state);
    }
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    /* Post-pass outside VFS locks: augment FILE_ATTRIBUTE_DIRECTORY on symlinks targeting directories. */
    {
        ULONG off = 0;
        ULONG total = *PBytesTransferred;
        while (off + sizeof(FSP_FSCTL_DIR_INFO) <= total) {
            FSP_FSCTL_DIR_INFO *di = (FSP_FSCTL_DIR_INFO *)((PUINT8)Buffer + off);
            if (di->Size < sizeof(FSP_FSCTL_DIR_INFO) || off + di->Size > total) break;
            if ((di->FileInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                size_t nchars = (di->Size - sizeof(FSP_FSCTL_DIR_INFO)) / sizeof(WCHAR);
                if (nchars > 0 && nchars <= EXT2_NAME_LEN) {
                    WCHAR leaf_w[EXT2_NAME_LEN + 1];
                    char leaf_utf8[EXT2_NAME_LEN * 3 + 1];
                    memcpy(leaf_w, di->FileNameBuf, nchars * sizeof(WCHAR));
                    leaf_w[nchars] = L'\0';
                    if (ext4_wide_to_utf8(leaf_w, leaf_utf8, sizeof(leaf_utf8)) == 0 &&
                        strcmp(leaf_utf8, ".") != 0 && strcmp(leaf_utf8, "..") != 0) {
                        char child_norm[1024];
                        int n;
                        if (strcmp(handle->path, "/") == 0) {
                            n = snprintf(child_norm, sizeof(child_norm), "/%s", leaf_utf8);
                        } else {
                            n = snprintf(child_norm, sizeof(child_norm), "%s/%s", handle->path, leaf_utf8);
                        }
                        if (n > 0 && (size_t)n < sizeof(child_norm) &&
                            resolve_symlink_target_is_dir(ctx, child_norm)) {
                            di->FileInfo.FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
                        }
                    }
                }
            }
            off += FSP_FSCTL_DEFAULT_ALIGN_UP(di->Size, 8);
        }
    }

    if (!state.buffer_full) {
        FspFileSystemAddDirInfo(NULL, Buffer, Length, PBytesTransferred);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspReadDirectory(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR Pattern,
                                 PWSTR Marker, PVOID Buffer, ULONG Length, PULONG PBytesTransferred) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspReadDirectory_impl(FileSystem, FileContext, Pattern, Marker,
                                        Buffer, Length, PBytesTransferred);
    if (_zt0) {
        ULONG bytes = PBytesTransferred ? *PBytesTransferred : 0;
        ztrace_record_complete("winfsp", "FspReadDirectory", _zt0, ztrace_now_qpc() - _zt0,
                               0, (uint64_t)bytes, (int64_t)st, ZTRACE_ARGS_IO);
    }
    return st;
}

static NTSTATUS FspGetDirInfoByName_impl(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                         PWSTR FileName, FSP_FSCTL_DIR_INFO *DirInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    char req_utf8[1024];
    const char *leaf_req;
    char actual_leaf[EXT2_NAME_LEN + 1];
    ext4_stat_t st;
    WCHAR wname[EXT2_NAME_LEN + 1];
    size_t wlen;
    int rc;

    if (!ctx || !handle || !FileName || !DirInfo) return STATUS_INVALID_PARAMETER;
    if (!handle->is_dir) return STATUS_NOT_A_DIRECTORY;
    if (ext4_wide_to_utf8(FileName, req_utf8, sizeof(req_utf8)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    leaf_req = req_utf8;
    if (leaf_req[0] == '\\' || leaf_req[0] == '/') {
        leaf_req++;
    }
    if (leaf_req[0] == '\0' || strchr(leaf_req, '\\') != NULL || strchr(leaf_req, '/') != NULL) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    actual_leaf[0] = '\0';
    if (strcmp(leaf_req, ".") == 0) {
        rc = ext4_vfs_stat_ino(ctx, handle->ino, ".", &st);
        strcpy_s(actual_leaf, sizeof(actual_leaf), ".");
    } else if (strcmp(leaf_req, "..") == 0) {
        char parent_path[1024];
        char dummy_leaf[EXT2_NAME_LEN + 1];
        if (strcmp(handle->path, "/") == 0) {
            strcpy_s(parent_path, sizeof(parent_path), "/");
        } else if (ext4_vfs_split_parent(handle->path, parent_path, sizeof(parent_path),
                                         dummy_leaf, sizeof(dummy_leaf)) != 0) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        rc = ext4_vfs_stat(ctx, parent_path, &st);
        strcpy_s(actual_leaf, sizeof(actual_leaf), "..");
    } else {
        char full_path[1024];
        int n;
        if (strcmp(handle->path, "/") == 0) {
            n = snprintf(full_path, sizeof(full_path), "/%s", leaf_req);
        } else {
            n = snprintf(full_path, sizeof(full_path), "%s/%s", handle->path, leaf_req);
        }
        if (n < 0 || (size_t)n >= sizeof(full_path)) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (!ctx->overlay) {
            rc = ext4_vfs_lower_stat_with_name(ctx, full_path, &st, actual_leaf, sizeof(actual_leaf));
        } else {
            rc = overlay_vfs_stat_with_name(ctx, full_path, &st, actual_leaf, sizeof(actual_leaf));
        }
    }

    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    if (actual_leaf[0] == '\0') {
        strcpy_s(actual_leaf, sizeof(actual_leaf), leaf_req);
    }
    if (ext4_utf8_to_wide(actual_leaf, wname, _countof(wname)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    wlen = wcslen(wname);
    memset(DirInfo, 0, sizeof(FSP_FSCTL_DIR_INFO));
    DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + wlen * sizeof(WCHAR));
    {
        int sym_dir = 0;
        if (st.is_symlink && strcmp(actual_leaf, ".") != 0 && strcmp(actual_leaf, "..") != 0) {
            char full_child[1024];
            int n;
            if (strcmp(handle->path, "/") == 0) {
                n = snprintf(full_child, sizeof(full_child), "/%s", actual_leaf);
            } else {
                n = snprintf(full_child, sizeof(full_child), "%s/%s", handle->path, actual_leaf);
            }
            if (n > 0 && (size_t)n < sizeof(full_child)) {
                sym_dir = resolve_symlink_target_is_dir(ctx, full_child);
            }
        }
        fill_fsp_file_info(&st, sym_dir, &DirInfo->FileInfo);
    }
    memcpy(DirInfo->FileNameBuf, wname, wlen * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

static NTSTATUS FspGetDirInfoByName(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                    PWSTR FileName, FSP_FSCTL_DIR_INFO *DirInfo) {
    uint64_t _zt0 = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
    NTSTATUS st = FspGetDirInfoByName_impl(FileSystem, FileContext, FileName, DirInfo);
    if (_zt0) {
        ztrace_record_complete("winfsp", "FspGetDirInfoByName", _zt0, ztrace_now_qpc() - _zt0,
                               (st == STATUS_SUCCESS && DirInfo) ? DirInfo->FileInfo.IndexNumber : 0ULL,
                               0, (int64_t)st, ZTRACE_ARGS_STATUS);
    }
    return st;
}

static NTSTATUS FspResolveReparsePoints(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName,
                                        UINT32 ReparsePointIndex, BOOLEAN ResolveLastPathComponent,
                                        PIO_STATUS_BLOCK PIoStatus, PVOID Buffer, PSIZE_T PSize) {
    return FspFileSystemResolveReparsePoints(
        FileSystem, ext4_get_reparse_point_by_name, NULL,
        FileName, ReparsePointIndex, ResolveLastPathComponent,
        PIoStatus, Buffer, PSize);
}

static NTSTATUS FspGetReparsePoint(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                   PWSTR FileName, PVOID Buffer, PSIZE_T PSize) {
    ext4_vfs_ctx_t *ctx = FileSystem ? (ext4_vfs_ctx_t *)FileSystem->UserContext : NULL;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    ext4_stat_t st;
    int rc;

    if (!ctx) return STATUS_INVALID_PARAMETER;
    if (handle && handle->path[0] != '\0') {
        rc = ext4_vfs_stat(ctx, handle->path, &st);
        if (rc != 0) return ext4_errno_to_ntstatus(rc);
        if (!st.is_symlink) return STATUS_NOT_A_REPARSE_POINT;
        return build_symlink_reparse_data(ctx, handle->path, Buffer, PSize);
    }
    return ext4_get_reparse_point_by_name(FileSystem, NULL, FileName, FALSE, Buffer, PSize);
}

static NTSTATUS FspSetReparsePoint(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                   PWSTR FileName, PVOID Buffer, SIZE_T Size) {
    ext4_vfs_ctx_t *ctx = FileSystem ? (ext4_vfs_ctx_t *)FileSystem->UserContext : NULL;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    const FSP_SYMLINK_REPARSE_DATA_BUFFER *rb;
    char link_norm[1024];
    char target_posix[1024];
    WCHAR target_w[512];
    size_t sub_cch;
    ext4_stat_t st;
    int rc;

    if (!ctx) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;
    if (ctx->overlay) return STATUS_NOT_SUPPORTED;
    if (!Buffer || Size < sizeof(FSP_SYMLINK_REPARSE_DATA_BUFFER)) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    rb = (const FSP_SYMLINK_REPARSE_DATA_BUFFER *)Buffer;
    if (rb->ReparseTag != IO_REPARSE_TAG_SYMLINK) {
        return STATUS_IO_REPARSE_TAG_MISMATCH;
    }
    if ((rb->SubstituteNameOffset & 1U) != 0 || (rb->SubstituteNameLength & 1U) != 0 ||
        rb->SubstituteNameLength == 0 ||
        (size_t)sizeof(FSP_SYMLINK_REPARSE_DATA_BUFFER) + (size_t)rb->SubstituteNameOffset + (size_t)rb->SubstituteNameLength > Size) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    sub_cch = rb->SubstituteNameLength / sizeof(WCHAR);
    if (sub_cch >= _countof(target_w)) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    memcpy(target_w, (const UINT8 *)rb->PathBuffer + rb->SubstituteNameOffset, rb->SubstituteNameLength);
    target_w[sub_cch] = L'\0';

    if (ext4_wide_to_utf8(target_w, target_posix, sizeof(target_posix)) != 0) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if (strncmp(target_posix, "\\??\\", 4) == 0 || strncmp(target_posix, "\\\\", 2) == 0 ||
        (target_posix[0] && target_posix[1] == ':')) {
        return STATUS_NOT_SUPPORTED;
    }
    for (size_t i = 0; target_posix[i]; i++) {
        if (target_posix[i] == '\\') target_posix[i] = '/';
    }

    if (handle && handle->path[0] != '\0') {
        strcpy_s(link_norm, sizeof(link_norm), handle->path);
    } else if (FileName) {
        char raw[1024];
        if (ext4_wide_to_utf8(FileName, raw, sizeof(raw)) != 0 ||
            ext4_vfs_normalize_path(raw, link_norm, sizeof(link_norm)) != 0) {
            return STATUS_OBJECT_NAME_INVALID;
        }
    } else {
        return STATUS_INVALID_PARAMETER;
    }

    if (resolve_posix_target_in_volume(link_norm, target_posix, NULL, 0, NULL) != 0) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    rc = ext4_vfs_stat(ctx, link_norm, &st);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    if (st.is_dir) {
        rc = ext4_vfs_can_delete(ctx, st.ino);
        if (rc != 0) return ext4_errno_to_ntstatus(rc);
    } else if (st.size != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    rc = ext4_vfs_unlink(ctx, link_norm);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    rc = ext4_vfs_symlink(ctx, link_norm, target_posix);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    if (handle && ext4_vfs_stat(ctx, link_norm, &st) == 0) {
        handle->ino = st.ino;
        handle->is_dir = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspDeleteReparsePoint(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                      PWSTR FileName, PVOID Buffer, SIZE_T Size) {
    ext4_vfs_ctx_t *ctx = FileSystem ? (ext4_vfs_ctx_t *)FileSystem->UserContext : NULL;
    (void)FileContext;
    (void)FileName;
    (void)Buffer;
    (void)Size;
    if (!ctx) return STATUS_INVALID_PARAMETER;
    if (ctx->read_only) return STATUS_MEDIA_WRITE_PROTECTED;
    return STATUS_NOT_SUPPORTED;
}

static const FSP_FILE_SYSTEM_INTERFACE g_winfsp_iface = {
    .GetVolumeInfo        = FspGetVolumeInfo,
    .SetVolumeLabel       = FspSetVolumeLabel,
    .GetSecurityByName    = FspGetSecurityByName,
    .Create               = FspCreate,
    .Open                 = FspOpen,
    .Overwrite            = FspOverwrite,
    .Cleanup              = FspCleanup,
    .Close                = FspClose,
    .Read                 = FspRead,
    .Write                = FspWrite,
    .Flush                = FspFlush,
    .GetFileInfo          = FspGetFileInfo,
    .SetBasicInfo         = FspSetBasicInfo,
    .SetFileSize          = FspSetFileSize,
    .CanDelete            = FspCanDelete,
    .Rename               = FspRename,
    .GetSecurity          = FspGetSecurity,
    .SetSecurity          = FspSetSecurity,
    .ReadDirectory        = FspReadDirectory,
    .ResolveReparsePoints = FspResolveReparsePoints,
    .GetReparsePoint      = FspGetReparsePoint,
    .SetReparsePoint      = FspSetReparsePoint,
    .DeleteReparsePoint   = FspDeleteReparsePoint,
    .GetDirInfoByName     = FspGetDirInfoByName,
    .SetDelete            = FspSetDelete,
};

const FSP_FILE_SYSTEM_INTERFACE *winfsp_driver_get_interface(void) {
    return &g_winfsp_iface;
}

void winfsp_driver_build_volume_params(ext4_vfs_ctx_t *ctx, FSP_FSCTL_VOLUME_PARAMS *out_params) {
    ext4_volume_stat_t vol;
    memset(out_params, 0, sizeof(*out_params));
    ext4_vfs_statfs(ctx, &vol);

    /* Version = sizeof(official 2.x FSP_FSCTL_VOLUME_PARAMS) = 504 (ABI-asserted in winfsp.h) */
    out_params->Version = (UINT16)sizeof(FSP_FSCTL_VOLUME_PARAMS);
    out_params->SectorSize = (UINT16)vol.block_size;
    out_params->SectorsPerAllocationUnit = 1;
    out_params->MaxComponentLength = EXT2_NAME_LEN;
    out_params->VolumeCreationTime = ext4_now_filetime();
    out_params->VolumeSerialNumber = vol.serial_number;
    /* On Read-Only volumes, INFINITE ((UINT32)-1) enables the Windows NT Cache Manager (CcCopyRead)
     * and avoids repeated metadata round-trips. RW and writable Overlay keep 1000 ms for coherence. */
    if (ctx->has_file_info_timeout) {
        out_params->FileInfoTimeout = ctx->file_info_timeout;
    } else {
        out_params->FileInfoTimeout = ctx->read_only ? (UINT32)-1 : 1000U;
    }
    out_params->CaseSensitiveSearch = ctx->case_insensitive ? 0 : 1;
    out_params->CasePreservedNames = 1;
    out_params->UnicodeOnDisk = 1;
    out_params->PersistentAcls = 0;
    out_params->ReparsePoints = 1;
    out_params->ReadOnlyVolume = ctx->read_only ? 1 : 0;
    out_params->PostCleanupWhenModifiedOnly = 1;
    out_params->PassQueryDirectoryFileName = 1;
    out_params->SupportsPosixUnlinkRename = 1;
    out_params->UmFileContextIsUserContext2 = 1;
    wcscpy_s(out_params->FileSystemName, _countof(out_params->FileSystemName), L"ext4");
}

int winfsp_driver_create_test_fs(ext4_vfs_ctx_t *ctx, FSP_FILE_SYSTEM **out_fs) {
    FSP_FILE_SYSTEM *fs;
    if (!ctx || !out_fs) return -EINVAL;
    fs = (FSP_FILE_SYSTEM *)calloc(1, sizeof(*fs));
    if (!fs) return -ENOMEM;
    fs->Version = sizeof(FSP_FILE_SYSTEM);
    fs->UserContext = ctx;
    fs->Interface = &g_winfsp_iface;
    *out_fs = fs;
    return 0;
}

void winfsp_driver_destroy_test_fs(FSP_FILE_SYSTEM *fs) {
    free(fs);
}

/* ---- Drive-letter visibility helpers (UAC split-token / DosDevices isolation) ---- */

static int is_drive_letter(const wchar_t *mp) {
    return mp && ((mp[0] >= L'A' && mp[0] <= L'Z') || (mp[0] >= L'a' && mp[0] <= L'z')) &&
           mp[1] == L':' && mp[2] == 0;
}

static int process_is_elevated(void) {
    HANDLE tok = NULL;
    TOKEN_ELEVATION elev;
    DWORD len = 0;
    int result = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &len)) {
            result = elev.TokenIsElevated ? 1 : 0;
        }
        CloseHandle(tok);
    }
    return result;
}

typedef BOOL (*user_session_fn)(const wchar_t *drive, const wchar_t *target, int remove);

static BOOL define_user_dosdev(const wchar_t *drive, const wchar_t *target, int remove) {
    if (remove) {
        return DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH | DDD_EXACT_MATCH_ON_REMOVE,
                                drive, target);
    }
    return DefineDosDeviceW(DDD_RAW_TARGET_PATH, drive, target);
}

/* CSRSS (BaseSrvDefineDosDevice) uses the caller's effective (impersonation) token to pick the
 * logon session's DosDevices directory, so impersonating the non-elevated shell token makes the
 * definition land in the namespace Explorer actually looks at. */
static int run_as_desktop_user(user_session_fn fn, const wchar_t *drive, const wchar_t *target, int remove) {
    HWND shell = GetShellWindow();
    DWORD pid = 0;
    HANDLE proc = NULL, tok = NULL, imp = NULL;
    int ok = 0;

    if (!shell) return 0;
    GetWindowThreadProcessId(shell, &pid);
    if (!pid) return 0;
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return 0;
    if (OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &tok)) {
        if (DuplicateTokenEx(tok, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, NULL,
                             SecurityImpersonation, TokenImpersonation, &imp)) {
            if (ImpersonateLoggedOnUser(imp)) {
                ok = fn(drive, target, remove) ? 1 : 0;
                RevertToSelf();
            }
            CloseHandle(imp);
        }
        CloseHandle(tok);
    }
    CloseHandle(proc);
    return ok;
}

static void remove_user_dosdev(winfsp_mount_handle_t *mh) {
    if (mh && mh->user_dosdev) {
        run_as_desktop_user(define_user_dosdev, mh->drive[0] ? mh->drive : NULL, mh->volume_name, 1);
        mh->user_dosdev = 0;
    }
}

static void notify_shell_drive(const wchar_t *drive, int added) {
    WCHAR root[4] = { drive[0], L':', L'\\', 0 };
    SHChangeNotify(added ? SHCNE_DRIVEADD : SHCNE_DRIVEREMOVED, SHCNF_PATHW, root, NULL);
}

static BOOL remove_any_dosdev(const wchar_t *drive, const wchar_t *target, int remove) {
    (void)target; (void)remove;
    return DefineDosDeviceW(DDD_REMOVE_DEFINITION, drive, NULL);
}

int winfsp_driver_is_safe_unmount_target(const wchar_t *dos_target,
                                         int volume_alive,
                                         const wchar_t *fs_name) {
    if (!dos_target || !dos_target[0]) return 0;
    if (_wcsnicmp(dos_target, L"\\Device\\Volume{", 15) != 0 &&
        _wcsnicmp(dos_target, L"\\Device\\WinFsp.", 15) != 0) {
        return 0;
    }
    if (volume_alive) {
        return (fs_name && _wcsicmp(fs_name, L"ext4") == 0) ? 1 : 0;
    }
    return 1;
}

typedef struct unmount_probe_state {
    const wchar_t *drive;
    const wchar_t *root;
    wchar_t        target[512];
    wchar_t        fs_name[64];
    int            has_target;
    int            volume_alive;
} unmount_probe_state_t;

static void probe_drive_in_current_session(unmount_probe_state_t *st) {
    DWORD rc;
    st->has_target = 0;
    st->volume_alive = 0;
    st->target[0] = 0;
    st->fs_name[0] = 0;

    rc = QueryDosDeviceW(st->drive, st->target, (DWORD)_countof(st->target));
    if (rc != 0) {
        st->has_target = 1;
    } else if (GetDriveTypeW(st->root) != DRIVE_NO_ROOT_DIR) {
        /* Drive root exists even if QueryDosDeviceW failed (e.g. restricted session) */
        st->has_target = 1;
        wcscpy_s(st->target, _countof(st->target), L"<unknown>");
    } else {
        return;
    }

    if (GetVolumeInformationW(st->root, NULL, 0, NULL, NULL, NULL,
                              st->fs_name, (DWORD)_countof(st->fs_name))) {
        st->volume_alive = 1;
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
            /* Treat inaccessible live volumes conservatively as alive/non-ext4 */
            st->volume_alive = 1;
            st->fs_name[0] = 0;
        }
    }
}

static void probe_drive_as_desktop_user(unmount_probe_state_t *st) {
    HWND shell = GetShellWindow();
    DWORD pid = 0;
    HANDLE proc = NULL, tok = NULL, imp = NULL;

    st->has_target = 0;
    st->volume_alive = 0;
    st->target[0] = 0;
    st->fs_name[0] = 0;

    if (!shell) return;
    GetWindowThreadProcessId(shell, &pid);
    if (!pid) return;
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return;
    if (OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &tok)) {
        if (DuplicateTokenEx(tok, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, NULL,
                             SecurityImpersonation, TokenImpersonation, &imp)) {
            if (ImpersonateLoggedOnUser(imp)) {
                probe_drive_in_current_session(st);
                RevertToSelf();
            }
            CloseHandle(imp);
        }
        CloseHandle(tok);
    }
    CloseHandle(proc);
}

int winfsp_driver_force_unmount(const wchar_t *drive) {
    WCHAR root[4];
    unmount_probe_state_t cur_probe;
    unmount_probe_state_t desk_probe;
    int elevated;
    int removed = 0;
    if (!is_drive_letter(drive)) return -EINVAL;
    if (drive[0] == L'C' || drive[0] == L'c') {
        fprintf(stderr, "Refusing to unmount system drive %ls.\n", drive);
        return -EACCES;
    }
    root[0] = drive[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;

    cur_probe.drive = drive;
    cur_probe.root = root;
    probe_drive_in_current_session(&cur_probe);

    memset(&desk_probe, 0, sizeof(desk_probe));
    elevated = process_is_elevated();
    if (elevated) {
        desk_probe.drive = drive;
        desk_probe.root = root;
        probe_drive_as_desktop_user(&desk_probe);
    }

    if (!cur_probe.has_target && !desk_probe.has_target) {
        return -ENOENT;
    }

    if (cur_probe.has_target &&
        !winfsp_driver_is_safe_unmount_target(cur_probe.target, cur_probe.volume_alive, cur_probe.fs_name)) {
        fprintf(stderr, "Refusing to unmount %ls: target '%ls' (%ls) is not a ZeroExt4/WinFsp volume.\n",
                drive, cur_probe.target, cur_probe.fs_name[0] ? cur_probe.fs_name : L"unknown");
        return -EACCES;
    }
    if (desk_probe.has_target &&
        !winfsp_driver_is_safe_unmount_target(desk_probe.target, desk_probe.volume_alive, desk_probe.fs_name)) {
        fprintf(stderr, "Refusing to unmount %ls: target '%ls' (%ls) in desktop session is not a ZeroExt4/WinFsp volume.\n",
                drive, desk_probe.target, desk_probe.fs_name[0] ? desk_probe.fs_name : L"unknown");
        return -EACCES;
    }

    /* 1. Mount Manager (global "\\.\X:" mounts) */
    if (DeleteVolumeMountPointW(root)) {
        printf("Removed Mount Manager drive letter %ls\n", drive);
        removed = 1;
    }
    /* 2. DOS device in this (possibly elevated) logon session */
    if (DefineDosDeviceW(DDD_REMOVE_DEFINITION, drive, NULL)) {
        printf("Removed DOS device %ls from current session\n", drive);
        removed = 1;
    }
    /* 3. DOS device in the non-elevated desktop user's session */
    if (elevated && run_as_desktop_user(remove_any_dosdev, drive, NULL, 1)) {
        printf("Removed DOS device %ls from desktop user session\n", drive);
        removed = 1;
    }
    notify_shell_drive(drive, 0);
    return removed ? 0 : -ENOENT;
}

int winfsp_driver_start(ext4_vfs_ctx_t *ctx, const wchar_t *mountpoint,
                        uint32_t debug_log, winfsp_mount_handle_t **out_handle) {
    FSP_RUNTIME_API *api = FspGetRuntimeApi();
    FSP_FSCTL_VOLUME_PARAMS vol_params;
    winfsp_mount_handle_t *mh;
    NTSTATUS status;

    if (!ctx || !mountpoint || !out_handle) return -EINVAL;
    *out_handle = NULL;

    if (!api || !api->Module || !api->Create || !api->SetMountPoint ||
        !api->StartDispatcher || !api->StopDispatcher || !api->Delete) {
        ext4_log(EXT4_LOG_ERROR,
                 "WinFsp runtime (winfsp-x64.dll) is not installed on this system.");
        return -ENODEV;
    }

    mh = (winfsp_mount_handle_t *)calloc(1, sizeof(*mh));
    if (!mh) return -ENOMEM;
    mh->vfs = ctx;

    winfsp_driver_build_volume_params(ctx, &vol_params);

    status = api->Create(L"WinFsp.Disk", &vol_params, &g_winfsp_iface, &mh->fs);
    if (!NT_SUCCESS(status)) {
        fprintf(stderr, "FspFileSystemCreate failed: NTSTATUS=0x%08lX\n", (unsigned long)status);
        free(mh);
        return -EIO;
    }

    mh->fs->UserContext = ctx;
    if (debug_log && api->SetDebugLogF) {
        if (api->DebugLogSetHandle) api->DebugLogSetHandle(GetStdHandle(STD_ERROR_HANDLE));
        api->SetDebugLogF(mh->fs, debug_log);
    }

    if (is_drive_letter(mountpoint) && process_is_elevated()) {
        /*
         * ADR: An elevated process (required for \\.\PhysicalDriveN) creating "X:" via
         * DefineDosDevice only populates the *elevated* logon session's DosDevices, so the
         * non-elevated Explorer never sees the drive. WinFsp's documented "\\.\X:" syntax
         * registers the drive through the Mount Manager (global, all sessions). If that is
         * unavailable we fall back to "X:" + defining the DOS device in the desktop user's
         * session via impersonation of the shell (explorer.exe) token.
         */
        WCHAR global_mp[8];
        _snwprintf_s(global_mp, _countof(global_mp), _TRUNCATE, L"\\\\.\\%c:", mountpoint[0]);
        status = api->SetMountPoint(mh->fs, global_mp);
        if (NT_SUCCESS(status)) {
            ext4_log(EXT4_LOG_INFO, "Mounted via Mount Manager (global drive letter).");
        } else {
            ext4_log(EXT4_LOG_WARN, "Mount Manager mount failed (0x%08lX); falling back to session DOS device.",
                     (unsigned long)status);
            status = api->SetMountPoint(mh->fs, (PWSTR)mountpoint);
            if (NT_SUCCESS(status)) {
                wcsncpy_s(mh->volume_name, _countof(mh->volume_name), mh->fs->VolumeName, _TRUNCATE);
                if (run_as_desktop_user(define_user_dosdev, mountpoint, mh->volume_name, 0)) {
                    mh->user_dosdev = 1;
                } else {
                    ext4_log(EXT4_LOG_WARN, "Could not define %ls in the desktop user session; "
                             "the drive may be invisible to non-elevated Explorer.", mountpoint);
                }
            }
        }
    } else {
        status = api->SetMountPoint(mh->fs, (PWSTR)mountpoint);
    }
    if (!NT_SUCCESS(status)) {
        fprintf(stderr, "FspFileSystemSetMountPoint failed: NTSTATUS=0x%08lX\n", (unsigned long)status);
        api->Delete(mh->fs);
        free(mh);
        return -EIO;
    }
    if (is_drive_letter(mountpoint)) {
        mh->drive[0] = mountpoint[0];
        mh->drive[1] = L':';
        mh->drive[2] = 0;
    }

    status = api->StartDispatcher(mh->fs, 0);
    if (!NT_SUCCESS(status)) {
        fprintf(stderr, "FspFileSystemStartDispatcher failed: NTSTATUS=0x%08lX\n", (unsigned long)status);
        remove_user_dosdev(mh);
        api->Delete(mh->fs);
        free(mh);
        return -EIO;
    }

    if (mh->drive[0]) notify_shell_drive(mh->drive, 1);
    *out_handle = mh;
    return 0;
}

void winfsp_driver_stop(winfsp_mount_handle_t *handle) {
    FSP_RUNTIME_API *api = FspGetRuntimeApi();
    if (!handle) return;
    remove_user_dosdev(handle);
    if (handle->fs && api && api->StopDispatcher && api->Delete) {
        api->StopDispatcher(handle->fs);
        api->Delete(handle->fs);
    }
    if (handle->drive[0]) notify_shell_drive(handle->drive, 0);
    free(handle);
}

int winfsp_driver_is_runtime_available(void) {
    FSP_RUNTIME_API *api = FspGetRuntimeApi();
    return (api && api->Module && api->Create && api->SetMountPoint &&
            api->StartDispatcher && api->StopDispatcher && api->Delete) ? 1 : 0;
}

int winfsp_driver_get_runtime_info(char *out_dll_path, size_t path_sz,
                                   char *out_version, size_t ver_sz) {
    FSP_RUNTIME_API *api;
    WCHAR wpath[MAX_PATH * 2];
    DWORD n;
    DWORD ver_handle = 0;
    DWORD ver_size;

    if (out_dll_path && path_sz > 0) out_dll_path[0] = '\0';
    if (out_version && ver_sz > 0) out_version[0] = '\0';

    if (!winfsp_driver_is_runtime_available()) {
        return 0;
    }

    api = FspGetRuntimeApi();
    n = GetModuleFileNameW(api->Module, wpath, _countof(wpath));
    if (n > 0 && n < _countof(wpath)) {
        if (out_dll_path && path_sz > 0) {
            ext4_wide_to_utf8(wpath, out_dll_path, path_sz);
        }
        if (out_version && ver_sz > 0) {
            ver_size = GetFileVersionInfoSizeW(wpath, &ver_handle);
            if (ver_size > 0) {
                void *ver_buf = malloc(ver_size);
                if (ver_buf) {
                    if (GetFileVersionInfoW(wpath, 0, ver_size, ver_buf)) {
                        VS_FIXEDFILEINFO *ffi = NULL;
                        UINT ffi_len = 0;
                        if (VerQueryValueW(ver_buf, L"\\", (LPVOID *)&ffi, &ffi_len) &&
                            ffi && ffi_len >= sizeof(*ffi)) {
                            _snprintf_s(out_version, ver_sz, _TRUNCATE, "%u.%u.%u.%u",
                                        (unsigned)HIWORD(ffi->dwFileVersionMS),
                                        (unsigned)LOWORD(ffi->dwFileVersionMS),
                                        (unsigned)HIWORD(ffi->dwFileVersionLS),
                                        (unsigned)LOWORD(ffi->dwFileVersionLS));
                        }
                    }
                    free(ver_buf);
                }
            }
        }
    }
    return 1;
}

int winfsp_driver_is_process_elevated(void) {
    return process_is_elevated();
}
