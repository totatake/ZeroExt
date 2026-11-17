#ifndef LIBEXT2FS_WIN_WINFSP_DRIVER_H
#define LIBEXT2FS_WIN_WINFSP_DRIVER_H

#include "../vfs/ext4_vfs.h"
#include <winfsp/winfsp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct winfsp_mount_handle winfsp_mount_handle_t;

const FSP_FILE_SYSTEM_INTERFACE *winfsp_driver_get_interface(void);
void winfsp_driver_build_volume_params(ext4_vfs_ctx_t *ctx, FSP_FSCTL_VOLUME_PARAMS *out_params);

/* Create an in-process FSP_FILE_SYSTEM stub for direct callback testing without WinFsp runtime */
int winfsp_driver_create_test_fs(ext4_vfs_ctx_t *ctx, FSP_FILE_SYSTEM **out_fs);
void winfsp_driver_destroy_test_fs(FSP_FILE_SYSTEM *fs);

/* Full WinFsp mount/unmount lifecycle (requires WinFsp runtime winfsp-x64.dll) */
int winfsp_driver_start(ext4_vfs_ctx_t *ctx, const wchar_t *mountpoint,
                        uint32_t debug_log, winfsp_mount_handle_t **out_handle);
void winfsp_driver_stop(winfsp_mount_handle_t *handle);

/* Remove a stale drive letter left behind by an abnormally terminated mount process
 * (Mount Manager mount point + DOS device in current and desktop-user sessions). */
int winfsp_driver_force_unmount(const wchar_t *drive);

#ifdef __cplusplus
}
#endif

#endif /* LIBEXT2FS_WIN_WINFSP_DRIVER_H */
