#ifndef ZEROEXT4_WINFSP_DRIVER_H
#define ZEROEXT4_WINFSP_DRIVER_H

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

/* Classify whether a drive target is safe for force-unmount (WinFsp/ext4 or dead WinFsp/Volume link).
 * Returns 1 if safe to remove, 0 if it belongs to another filesystem or device. */
int winfsp_driver_is_safe_unmount_target(const wchar_t *dos_target,
                                         int volume_alive,
                                         const wchar_t *fs_name);

/* Remove a stale drive letter left behind by an abnormally terminated mount process
 * (Mount Manager mount point + DOS device in current and desktop-user sessions).
 * Refuses with -EACCES if the drive belongs to a non-ZeroExt4/WinFsp volume. */
int winfsp_driver_force_unmount(const wchar_t *drive);

/* Runtime capability helpers */
int winfsp_driver_is_runtime_available(void);
int winfsp_driver_is_process_elevated(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_WINFSP_DRIVER_H */
