#ifndef ZEROEXT4_OVERLAY_VFS_H
#define ZEROEXT4_OVERLAY_VFS_H

#include "../vfs/ext4_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ext4_overlay_ctx ext4_overlay_ctx_t;

int overlay_vfs_create(ext4_vfs_ctx_t *lower_ctx, const char *upper_dir_utf8,
                       ext4_overlay_ctx_t **out_ov);
void overlay_vfs_destroy(ext4_overlay_ctx_t *ov);

int overlay_vfs_stat(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st);
int overlay_vfs_stat_ino(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const char *leaf_name, ext4_stat_t *out_st);
int overlay_vfs_set_times(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint32_t win_attrs,
                          uint64_t ctime_ft, uint64_t atime_ft, uint64_t mtime_ft, uint64_t chgtime_ft);
int overlay_vfs_readdir(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, ext4_readdir_cb cb, void *user_ctx);
int overlay_vfs_open(ext4_vfs_ctx_t *ctx, const char *path, int writable, ext4_file_handle_t **out_handle);
int overlay_vfs_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                     uint32_t length, uint32_t *out_read);
int overlay_vfs_write(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                      uint32_t length, int write_to_eof, uint32_t *out_written, uint64_t *out_new_size);
int overlay_vfs_create_file(ext4_vfs_ctx_t *ctx, const char *path, int is_dir, uint16_t mode,
                            uint32_t win_attrs, ext2_ino_t *out_ino);
int overlay_vfs_mkdir(ext4_vfs_ctx_t *ctx, const char *path, uint16_t mode, ext2_ino_t *out_ino);
int overlay_vfs_can_delete(ext4_vfs_ctx_t *ctx, ext2_ino_t ino);
int overlay_vfs_unlink(ext4_vfs_ctx_t *ctx, const char *path);
int overlay_vfs_rename(ext4_vfs_ctx_t *ctx, const char *old_path, const char *new_path, int replace_if_exists);
int overlay_vfs_truncate(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint64_t new_size);
int overlay_vfs_statfs(ext4_vfs_ctx_t *ctx, ext4_volume_stat_t *out_vol);
int overlay_vfs_flush(ext4_vfs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_OVERLAY_VFS_H */
