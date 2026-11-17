#ifndef LIBEXT2FS_WIN_EXT4_ERRNO_H
#define LIBEXT2FS_WIN_EXT4_ERRNO_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <et/com_err.h>

#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif

#ifdef __cplusplus
extern "C" {
#endif

int ext4_errcode_to_errno(errcode_t err);
NTSTATUS ext4_errno_to_ntstatus(int neg_errno);
NTSTATUS ext4_errcode_to_ntstatus(errcode_t err);

int ext4_utf8_to_wide(const char *utf8, wchar_t *out, size_t out_chars);
int ext4_wide_to_utf8(const wchar_t *wide, char *out, size_t out_bytes);

void ext4_set_log_level(int level);
int ext4_get_log_level(void);
void ext4_log(int level, const char *fmt, ...);

#define EXT4_LOG_ERROR 0
#define EXT4_LOG_WARN  1
#define EXT4_LOG_INFO  2
#define EXT4_LOG_DEBUG 3

#ifdef __cplusplus
}
#endif

#endif /* LIBEXT2FS_WIN_EXT4_ERRNO_H */
