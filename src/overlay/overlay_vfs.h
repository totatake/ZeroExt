#ifndef ZEROEXT4_OVERLAY_VFS_H
#define ZEROEXT4_OVERLAY_VFS_H

#include "../vfs/ext4_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ext4_overlay_ctx ext4_overlay_ctx_t;

typedef enum overlay_upper_status {
    OV_UPPER_OK_WILL_CREATE = 0,
    OV_UPPER_OK_EMPTY = 1,
    OV_UPPER_OK_MATCH_V2 = 2,
    OV_UPPER_OK_MATCH_V1 = 3,
    OV_UPPER_NON_EMPTY_UNMARKED = 4,
    OV_UPPER_MISMATCH_LOWER = 5,
    OV_UPPER_MISMATCH_CASE_MODE = 6,
    OV_UPPER_CORRUPT_MARKER = 7,
    OV_UPPER_UNSUPPORTED_VERSION = 8,
    OV_UPPER_INVALID_PATH = 9,
    OV_UPPER_CS_NOT_SUPPORTED = 10,
} overlay_upper_status_t;

typedef struct overlay_lower_identity {
    char     fs_uuid[37];
    uint64_t blocks_count;
    uint32_t block_size;
    char     partition_uuid[48];
    uint64_t partition_offset;
    uint64_t partition_size;
    int      case_sensitive;
} overlay_lower_identity_t;

typedef struct overlay_upper_check {
    overlay_upper_status_t status;
    int                    can_mount;
    int                    marker_version; /* 0=none, 1=v1, 2=v2, >2=future, -1=corrupt */
    char                   expected_id[256];
    char                   found_id[256];
    char                   reason[256];
} overlay_upper_check_t;

void overlay_format_lower_id_v2(const overlay_lower_identity_t *id, char *out, size_t out_sz);
void overlay_check_upper_dir(const char *upper_dir_utf8,
                             const overlay_lower_identity_t *expected,
                             overlay_upper_check_t *out);
const char *overlay_upper_status_name(overlay_upper_status_t status);

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
int overlay_vfs_readlink(ext4_vfs_ctx_t *ctx, const char *link_path, char *out_target, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_OVERLAY_VFS_H */
