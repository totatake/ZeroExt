#ifndef ZEROEXT4_EXT4_VFS_H
#define ZEROEXT4_EXT4_VFS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <ext2fs/ext2fs.h>
#include "../disk/superblock_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ext4_extent_map;
struct ext4_extent_cache;
struct ext4_overlay_ctx;
struct ext4_dentry_cache;

typedef struct ext4_mount_config {
    const char *source_path;
    uint32_t    partition_number;     /* 0 = auto / raw */
    uint64_t    partition_offset;     /* explicit override if > 0 */
    uint64_t    partition_size;       /* explicit override if > 0 */
    uint32_t    sector_size;          /* 0 = default/auto */
    uint32_t    max_cache_blocks;     /* 0 = default 1024 */
    int         read_only;            /* 1 = RO mount */
    int         ro_fallback_on_dirty; /* 1 = automatically fall back to RO when dirty */
    int         force_rw_on_dirty;    /* 1 = allow RW even if dirty journal detected */
    int         ignore_csum_errors;   /* 1 = set EXT2_FLAG_IGNORE_CSUM_ERRORS */
    int         case_insensitive;     /* 1 = fallback case-insensitive lookup (Windows default) */
    const char *overlay_upper_dir;    /* Optional NTFS upperdir for Overlay mount (lower opens RO) */
    int         enable_direct_io;     /* 1 = require direct extent I/O (default ON in RO/Overlay; error on RW) */
    int         disable_direct_io;    /* 1 = disable direct extent I/O in RO/Overlay (use ext2fs_file_read) */
    int         enable_rw_fast_io;    /* 1 = opt-in batched extent read/write in --rw mode (default 0: upstream e2fsprogs) */
    int         has_file_info_timeout; /* 1 = use explicit file_info_timeout below */
    uint32_t    file_info_timeout;     /* WinFsp FileInfoTimeout ms, or (uint32_t)-1 for INFINITE */
} ext4_mount_config_t;

typedef struct ext4_vfs_ctx {
    ext2_filsys               fs;
    SRWLOCK                   rwlock;
    CRITICAL_SECTION          fs_lock;
    int                       read_only;
    int                       lower_read_only;
    int                       dirty_detected;
    int                       fell_back_to_ro;
    int                       case_insensitive;
    int                       enable_direct_io;
    int                       enable_rw_fast_io;
    int                       has_file_info_timeout;
    uint32_t                  file_info_timeout;
    ext4_journal_state_t      journal_state;
    char                      source_path[1024];
    uint64_t                  partition_offset;
    uint64_t                  partition_size;
    char                      partition_uuid[48];
    char                      volume_label[32];
    struct ext4_extent_cache *extent_cache;
    struct ext4_dentry_cache *dentry_cache;
    struct ext4_overlay_ctx  *overlay;
    int                       stats_enabled;
    volatile LONG64           stat_read_bytes;
    volatile LONG64           stat_write_bytes;
    volatile LONG64           stat_read_ops;
    volatile LONG64           stat_write_ops;
} ext4_vfs_ctx_t;

typedef struct ext4_stat {
    ext2_ino_t ino;
    uint16_t   mode;
    uint32_t   file_attributes; /* Windows FILE_ATTRIBUTE_* */
    uint64_t   size;
    uint64_t   alloc_size;
    uint64_t   creation_time;   /* Windows FILETIME (100ns) */
    uint64_t   last_access_time;
    uint64_t   last_write_time;
    uint64_t   change_time;
    uint32_t   hard_links;
    uint32_t   uid;
    uint32_t   gid;
    int        is_dir;
    int        is_symlink;
} ext4_stat_t;

typedef struct ext4_volume_stat {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t block_size;
    uint32_t serial_number;
    int      read_only;
    char     volume_label[32];
} ext4_volume_stat_t;

typedef struct ext4_file_handle {
    ext2_ino_t               ino;
    int                      is_dir;
    int                      open_writable;
    int                      delete_on_close;
    uint32_t                 create_options;
    struct ext4_extent_map  *cached_extents;
    char                     path[1024];
} ext4_file_handle_t;

typedef struct ext4_dentry_cache_stats {
    uint64_t pos_hits;
    uint64_t neg_hits;
    uint64_t misses;
    uint64_t invalidations;
} ext4_dentry_cache_stats_t;

typedef int (*ext4_readdir_cb)(const char *name, const ext4_stat_t *st, void *user_ctx);

/* Lifecycle & mkfs (op_init.c) */
int ext4_vfs_mount(const ext4_mount_config_t *cfg, ext4_vfs_ctx_t **out_ctx);
int ext4_vfs_unmount(ext4_vfs_ctx_t *ctx);
int ext4_vfs_mkfs(const char *path_utf8, uint64_t size_bytes, const char *label);

/* Internal helpers (op_init.c / op_getattr.c) */
int ext4_vfs_normalize_path(const char *in_path, char *out_path, size_t out_sz);
int ext4_vfs_split_parent(const char *norm_path, char *out_parent, size_t parent_sz,
                          char *out_name, size_t name_sz);
int ext4_vfs_lookup_locked(ext4_vfs_ctx_t *ctx, const char *norm_path, ext2_ino_t *out_ino);
int ext4_vfs_lookup_locked_ex(ext4_vfs_ctx_t *ctx, const char *norm_path, ext2_ino_t *out_ino,
                              char *out_leaf, size_t out_leaf_sz);
void ext4_dentry_cache_invalidate(ext4_vfs_ctx_t *ctx);
void ext4_vfs_get_dentry_cache_stats(ext4_vfs_ctx_t *ctx, ext4_dentry_cache_stats_t *out_stats);
void ext4_fill_stat_from_inode(ext4_vfs_ctx_t *ctx, ext2_ino_t ino,
                               const struct ext2_inode_large *ino_st,
                               const char *leaf_name, ext4_stat_t *out_st);
uint64_t ext4_now_filetime(void);
void ext4_inode_touch_times(struct ext2_inode_large *ino_st, int touch_atime,
                            int touch_mtime, int touch_ctime);

/* Metadata & getattr (op_getattr.c) */
int ext4_vfs_stat(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st);
int ext4_vfs_stat_ino(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const char *leaf_name, ext4_stat_t *out_st);
int ext4_vfs_set_times(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint32_t win_attrs,
                       uint64_t ctime_ft, uint64_t atime_ft, uint64_t mtime_ft, uint64_t chgtime_ft);

/* Directory enumeration (op_readdir.c) */
int ext4_vfs_readdir(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, ext4_readdir_cb cb, void *user_ctx);
int ext4_vfs_readdir_from(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, const char *marker_utf8,
                          ext4_readdir_cb cb, void *user_ctx);

/* Handle open/close (op_open.c) */
int ext4_vfs_open(ext4_vfs_ctx_t *ctx, const char *path, int writable, ext4_file_handle_t **out_handle);
int ext4_vfs_open_stat(ext4_vfs_ctx_t *ctx, const char *path, int writable,
                       ext4_file_handle_t **out_handle, ext4_stat_t *out_st);
void ext4_vfs_close_handle(ext4_file_handle_t *handle);

/* File read/write (op_read.c / op_write.c) */
int ext4_vfs_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                  uint32_t length, uint32_t *out_read);
int ext4_vfs_read_handle(ext4_vfs_ctx_t *ctx, ext4_file_handle_t *handle, void *buf,
                         uint64_t offset, uint32_t length, uint32_t *out_read);
int ext4_vfs_write(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                   uint32_t length, int write_to_eof, uint32_t *out_written, uint64_t *out_new_size);
int ext4_vfs_write_stat(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const void *buf, uint64_t offset,
                        uint32_t length, int write_to_eof, const char *leaf_name,
                        uint32_t *out_written, uint64_t *out_new_size, ext4_stat_t *out_st);

/* Creation, mkdir, delete, rename, truncate, symlink, statfs, flush */
int ext4_vfs_create(ext4_vfs_ctx_t *ctx, const char *path, int is_dir, uint16_t mode,
                    uint32_t win_attrs, ext2_ino_t *out_ino);
int ext4_vfs_mkdir(ext4_vfs_ctx_t *ctx, const char *path, uint16_t mode, ext2_ino_t *out_ino);
int ext4_vfs_can_delete(ext4_vfs_ctx_t *ctx, ext2_ino_t ino);
int ext4_vfs_unlink(ext4_vfs_ctx_t *ctx, const char *path);
int ext4_vfs_rename(ext4_vfs_ctx_t *ctx, const char *old_path, const char *new_path, int replace_if_exists);
int ext4_vfs_truncate(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, uint64_t new_size);
int ext4_vfs_symlink(ext4_vfs_ctx_t *ctx, const char *link_path, const char *target_path);
int ext4_vfs_readlink(ext4_vfs_ctx_t *ctx, const char *link_path, char *out_target, size_t out_sz);
int ext4_vfs_statfs(ext4_vfs_ctx_t *ctx, ext4_volume_stat_t *out_vol);
int ext4_vfs_set_label(ext4_vfs_ctx_t *ctx, const char *label);
int ext4_vfs_flush(ext4_vfs_ctx_t *ctx);

/* Lower-layer direct helpers (bypass overlay routing; used by overlay_vfs & tools) */
int ext4_vfs_lower_stat(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st);
int ext4_vfs_lower_stat_with_name(ext4_vfs_ctx_t *ctx, const char *path, ext4_stat_t *out_st,
                                  char *out_leaf, size_t out_leaf_sz);
int ext4_vfs_lower_stat_ino(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, const char *leaf_name, ext4_stat_t *out_st);
int ext4_vfs_lower_readdir(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, ext4_readdir_cb cb, void *user_ctx);
int ext4_vfs_lower_readdir_from(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, const char *marker_utf8,
                                ext4_readdir_cb cb, void *user_ctx);
int ext4_vfs_lower_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                        uint32_t length, uint32_t *out_read);
int ext4_vfs_lower_statfs(ext4_vfs_ctx_t *ctx, ext4_volume_stat_t *out_vol);
int ext4_vfs_get_extents(ext4_vfs_ctx_t *ctx, const char *path, struct ext4_extent_map **out_map);

#ifdef __cplusplus
}
#endif

#endif /* ZEROEXT4_EXT4_VFS_H */
