#include "ext4_errno.h"
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <ext2fs/ext2_err.h>
#include <winfsp/winfsp.h>

static int g_ext4_log_level = EXT4_LOG_WARN;

void ext4_set_log_level(int level) {
    g_ext4_log_level = level;
}

int ext4_get_log_level(void) {
    return g_ext4_log_level;
}

void ext4_log(int level, const char *fmt, ...) {
    va_list args;
    const char *tag = "INFO";
    if (level > g_ext4_log_level) {
        return;
    }
    switch (level) {
    case EXT4_LOG_ERROR: tag = "ERROR"; break;
    case EXT4_LOG_WARN:  tag = "WARN"; break;
    case EXT4_LOG_INFO:  tag = "INFO"; break;
    case EXT4_LOG_DEBUG: tag = "DEBUG"; break;
    default: break;
    }
    fprintf(stderr, "[zeroext:%s] ", tag);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

int ext4_errcode_to_errno(errcode_t err) {
    if (err == 0) return 0;
    if (err > 0 && err < 256) return -(int)err;

    switch (err) {
    case EXT2_ET_FILE_NOT_FOUND:
        return -ENOENT;
    case EXT2_ET_NO_DIRECTORY:
        return -ENOTDIR;
    case EXT2_ET_IS_A_DIR:
        return -EISDIR;
    case EXT2_ET_FILE_EXISTS:
    case EXT2_ET_DIR_EXISTS:
        return -EEXIST;
    case EXT2_ET_DIR_NOT_EMPTY:
        return -ENOTEMPTY;
    case EXT2_ET_RO_FILSYS:
    case EXT2_ET_FILE_RO:
    case EXT2_ET_JOURNAL_UNSUPP_INCOMPAT:
        return -EROFS;
    case EXT2_ET_NO_FREE_SPACE:
    case EXT2_ET_INODE_ALLOC_FAIL:
    case EXT2_ET_BLOCK_ALLOC_FAIL:
    case EXT2_ET_DIR_NO_SPACE:
    case EXT2_ET_EXTENT_NO_SPACE:
    case EXT2_ET_INLINE_DATA_NO_SPACE:
        return -ENOSPC;
    case EXT2_ET_FILE_TOO_BIG:
    case EXT2_ET_TOOBIG:
        return -EFBIG;
    case EXT2_ET_NO_MEMORY:
        return -ENOMEM;
    case EXT2_ET_SYMLINK_LOOP:
        return -ELOOP;
    case EXT2_ET_INVALID_ARGUMENT:
    case EXT2_ET_BAD_INODE_NUM:
    case EXT2_ET_BAD_BLOCK_NUM:
        return -EINVAL;
    case EXT2_ET_OP_NOT_SUPPORTED:
    case EXT2_ET_UNIMPLEMENTED:
    case EXT2_ET_UNSUPP_FEATURE:
    case EXT2_ET_RO_UNSUPP_FEATURE:
        return -ENOTSUP;
    default:
        return -EIO;
    }
}

NTSTATUS ext4_errno_to_ntstatus(int neg_errno) {
    int e = (neg_errno < 0) ? -neg_errno : neg_errno;
    switch (e) {
    case 0:
        return STATUS_SUCCESS;
    case ENOENT:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR:
        return STATUS_NOT_A_DIRECTORY;
    case EISDIR:
        return STATUS_FILE_IS_A_DIRECTORY;
    case EEXIST:
        return STATUS_OBJECT_NAME_COLLISION;
    case ENOTEMPTY:
        return STATUS_DIRECTORY_NOT_EMPTY;
    case EROFS:
        return STATUS_MEDIA_WRITE_PROTECTED;
    case EACCES:
    case EPERM:
        return STATUS_ACCESS_DENIED;
    case ENOSPC:
        return STATUS_DISK_FULL;
    case ENOMEM:
        return STATUS_INSUFFICIENT_RESOURCES;
    case EINVAL:
    case ENAMETOOLONG:
        return STATUS_INVALID_PARAMETER;
    case EFBIG:
        return STATUS_DISK_FULL;
    case ENOTSUP:
        return STATUS_NOT_SUPPORTED;
    default:
        return STATUS_DEVICE_DATA_ERROR;
    }
}

NTSTATUS ext4_errcode_to_ntstatus(errcode_t err) {
    return ext4_errno_to_ntstatus(ext4_errcode_to_errno(err));
}

int ext4_utf8_to_wide(const char *utf8, wchar_t *out, size_t out_chars) {
    int n;
    if (!utf8 || !out || out_chars == 0) return -1;
    if (utf8[0] == '\0') {
        out[0] = L'\0';
        return 0;
    }
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, (int)out_chars);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_ACP, 0, utf8, -1, out, (int)out_chars);
    }
    return (n > 0) ? 0 : -1;
}

int ext4_wide_to_utf8(const wchar_t *wide, char *out, size_t out_bytes) {
    int n;
    if (!wide || !out || out_bytes == 0) return -1;
    if (wide[0] == L'\0') {
        out[0] = '\0';
        return 0;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_bytes, NULL, NULL);
    return (n > 0) ? 0 : -1;
}
