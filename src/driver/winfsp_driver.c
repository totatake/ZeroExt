#include "winfsp_driver.h"
#include "../common/ext4_errno.h"
#include <sddl.h>
#include <shlobj.h>
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

static void fill_fsp_file_info(const ext4_stat_t *st, FSP_FSCTL_FILE_INFO *fi) {
    memset(fi, 0, sizeof(*fi));
    fi->FileAttributes = st->file_attributes;
    fi->ReparseTag = 0;
    fi->AllocationSize = st->alloc_size;
    fi->FileSize = st->size;
    fi->CreationTime = st->creation_time;
    fi->LastAccessTime = st->last_access_time;
    fi->LastWriteTime = st->last_write_time;
    fi->ChangeTime = st->change_time;
    fi->IndexNumber = st->ino;
    fi->HardLinks = st->hard_links;
    fi->EaSize = 0;
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

static NTSTATUS FspGetSecurityByName(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName,
                                     PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
                                     SIZE_T *PSecurityDescriptorSize) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    char path_utf8[1024];
    ext4_stat_t st;
    int rc;

    if (!ctx || !FileName) return STATUS_INVALID_PARAMETER;
    if (ext4_wide_to_utf8(FileName, path_utf8, sizeof(path_utf8)) != 0) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    rc = ext4_vfs_stat(ctx, path_utf8, &st);
    if (rc != 0) {
        return ext4_errno_to_ntstatus(rc);
    }

    if (PFileAttributes) {
        *PFileAttributes = st.file_attributes;
    }
    if (PSecurityDescriptorSize) {
        return copy_default_sd(SecurityDescriptor, PSecurityDescriptorSize);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspCreate(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
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

    rc = ext4_vfs_create(ctx, path_utf8, is_dir, is_dir ? 0755 : 0644, FileAttributes, &ino);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    rc = ext4_vfs_open(ctx, path_utf8, 1, &handle);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    handle->create_options = CreateOptions;
    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        handle->delete_on_close = 1;
    }

    if (FileInfo) {
        const char *leaf = strrchr(handle->path, '/');
        leaf = leaf ? (leaf + 1) : handle->path;
        rc = ext4_vfs_stat_ino(ctx, ino, leaf, &st);
        if (rc == 0) {
            fill_fsp_file_info(&st, FileInfo);
        }
    }

    *PFileContext = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS FspOpen(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
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

    rc = ext4_vfs_open(ctx, path_utf8, wants_write && !ctx->read_only, &handle);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    if ((CreateOptions & FILE_DIRECTORY_FILE) && !handle->is_dir) {
        ext4_vfs_close_handle(handle);
        return STATUS_NOT_A_DIRECTORY;
    }
    if ((CreateOptions & FILE_NON_DIRECTORY_FILE) && handle->is_dir) {
        ext4_vfs_close_handle(handle);
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    handle->create_options = CreateOptions;
    if ((CreateOptions & FILE_DELETE_ON_CLOSE) && !ctx->read_only) {
        handle->delete_on_close = 1;
    }

    if (FileInfo) {
        const char *leaf = strrchr(handle->path, '/');
        leaf = leaf ? (leaf + 1) : handle->path;
        rc = ext4_vfs_stat_ino(ctx, handle->ino, leaf, &st);
        if (rc != 0) {
            ext4_vfs_close_handle(handle);
            return ext4_errno_to_ntstatus(rc);
        }
        fill_fsp_file_info(&st, FileInfo);
    }

    *PFileContext = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS FspOverwrite(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
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
            fill_fsp_file_info(&st, FileInfo);
        }
    }
    return STATUS_SUCCESS;
}

static VOID FspCleanup(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    (void)FileName;

    if (!ctx || !handle) return;
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

static VOID FspClose(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext) {
    (void)FileSystem;
    ext4_vfs_close_handle((ext4_file_handle_t *)FileContext);
}

static NTSTATUS FspRead(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
                        UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    uint32_t got = 0;
    int rc;

    if (!ctx || !handle || !PBytesTransferred) return STATUS_INVALID_PARAMETER;
    *PBytesTransferred = 0;
    if (Length == 0) return STATUS_SUCCESS;

    rc = ext4_vfs_read(ctx, handle->ino, Buffer, Offset, Length, &got);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    if (got == 0) return STATUS_END_OF_FILE;

    *PBytesTransferred = got;
    return STATUS_SUCCESS;
}

static NTSTATUS FspWrite(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer,
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
            if (FileInfo) fill_fsp_file_info(&cur_st, FileInfo);
            return STATUS_SUCCESS;
        }
        if (Offset + effective_len > cur_st.size) {
            effective_len = (ULONG)(cur_st.size - Offset);
        }
    }

    rc = ext4_vfs_write(ctx, handle->ino, Buffer, Offset, effective_len,
                        WriteToEndOfFile ? 1 : 0, &written, &new_size);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    *PBytesTransferred = written;
    if (FileInfo) {
        ext4_stat_t st;
        const char *leaf = strrchr(handle->path, '/');
        leaf = leaf ? (leaf + 1) : handle->path;
        if (ext4_vfs_stat_ino(ctx, handle->ino, leaf, &st) == 0) {
            fill_fsp_file_info(&st, FileInfo);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspFlush(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
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
            fill_fsp_file_info(&st, FileInfo);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FspGetFileInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                               FSP_FSCTL_FILE_INFO *FileInfo) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    ext4_stat_t st;
    const char *leaf;
    int rc;

    if (!ctx || !handle || !FileInfo) return STATUS_INVALID_PARAMETER;
    leaf = strrchr(handle->path, '/');
    leaf = leaf ? (leaf + 1) : handle->path;
    rc = ext4_vfs_stat_ino(ctx, handle->ino, leaf, &st);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);
    fill_fsp_file_info(&st, FileInfo);
    return STATUS_SUCCESS;
}

static NTSTATUS FspSetBasicInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
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

static NTSTATUS FspSetFileSize(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize,
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

static NTSTATUS FspRename(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName,
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

    rc = ext4_vfs_rename(ctx, old_utf8, new_utf8, ReplaceIfExists ? 1 : 0);
    if (rc == 0 && handle) {
        if (ext4_vfs_normalize_path(new_utf8, norm_new, sizeof(norm_new)) == 0) {
            strcpy_s(handle->path, sizeof(handle->path), norm_new);
        }
    }
    return ext4_errno_to_ntstatus(rc);
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
    fill_fsp_file_info(st, &dir_info->FileInfo);
    memcpy(dir_info->FileNameBuf, wname, wlen * sizeof(WCHAR));

    if (!FspFileSystemAddDirInfo(dir_info, s->buffer, s->length, s->transferred)) {
        s->buffer_full = 1;
        return 1; /* stop */
    }
    return 0;
}

static NTSTATUS FspReadDirectory(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR Pattern,
                                 PWSTR Marker, PVOID Buffer, ULONG Length, PULONG PBytesTransferred) {
    ext4_vfs_ctx_t *ctx = (ext4_vfs_ctx_t *)FileSystem->UserContext;
    ext4_file_handle_t *handle = (ext4_file_handle_t *)FileContext;
    struct fsp_readdir_state state;
    int rc;
    (void)Pattern;

    if (!ctx || !handle || !PBytesTransferred) return STATUS_INVALID_PARAMETER;
    *PBytesTransferred = 0;

    memset(&state, 0, sizeof(state));
    state.marker = Marker;
    state.past_marker = (Marker == NULL);
    state.buffer = Buffer;
    state.length = Length;
    state.transferred = PBytesTransferred;

    rc = ext4_vfs_readdir(ctx, handle->ino, fsp_readdir_cb, &state);
    if (rc != 0) return ext4_errno_to_ntstatus(rc);

    if (!state.buffer_full) {
        FspFileSystemAddDirInfo(NULL, Buffer, Length, PBytesTransferred);
    }
    return STATUS_SUCCESS;
}

static const FSP_FILE_SYSTEM_INTERFACE g_winfsp_iface = {
    .GetVolumeInfo     = FspGetVolumeInfo,
    .SetVolumeLabel    = FspSetVolumeLabel,
    .GetSecurityByName = FspGetSecurityByName,
    .Create            = FspCreate,
    .Open              = FspOpen,
    .Overwrite         = FspOverwrite,
    .Cleanup           = FspCleanup,
    .Close             = FspClose,
    .Read              = FspRead,
    .Write             = FspWrite,
    .Flush             = FspFlush,
    .GetFileInfo       = FspGetFileInfo,
    .SetBasicInfo      = FspSetBasicInfo,
    .SetFileSize       = FspSetFileSize,
    .CanDelete         = FspCanDelete,
    .Rename            = FspRename,
    .GetSecurity       = FspGetSecurity,
    .SetSecurity       = FspSetSecurity,
    .ReadDirectory     = FspReadDirectory,
    .SetDelete         = FspSetDelete,
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
    out_params->FileInfoTimeout = 1000;
    out_params->CaseSensitiveSearch = ctx->case_insensitive ? 0 : 1;
    out_params->CasePreservedNames = 1;
    out_params->UnicodeOnDisk = 1;
    out_params->PersistentAcls = 0;
    out_params->ReadOnlyVolume = ctx->read_only ? 1 : 0;
    out_params->PostCleanupWhenModifiedOnly = 1;
    out_params->SupportsPosixUnlinkRename = 1;
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

int winfsp_driver_force_unmount(const wchar_t *drive) {
    WCHAR root[4];
    int removed = 0;
    if (!is_drive_letter(drive)) return -EINVAL;
    root[0] = drive[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;

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
    if (process_is_elevated() && run_as_desktop_user(remove_any_dosdev, drive, NULL, 1)) {
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
