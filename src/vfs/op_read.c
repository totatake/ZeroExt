#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../extent/extent_map.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>

int ext4_vfs_lower_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                        uint32_t length, uint32_t *out_read) {
    ext2_file_t file = NULL;
    errcode_t retval;
    unsigned int got = 0;

    if (!ctx || !out_read || ino == 0) return -EINVAL;
    *out_read = 0;
    if (length == 0) return 0;
    if (!buf) return -EINVAL;

    if (ctx->enable_direct_io && ctx->lower_read_only && ctx->extent_cache) {
        ext4_extent_map_t *map = ext4_extent_cache_get(ctx->extent_cache, ino);
        int rc = 0;
        if (!map) {
            ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
                &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
            rc = ext4_extent_extract(ctx->fs, ino, &map);
            ztrace_srw_release_shared(&ctx->rwlock, &lg);
            if (rc == 0 && map) {
                ext4_extent_cache_put(ctx->extent_cache, map);
            }
        }
        if (rc == 0 && map) {
            rc = ext4_extent_pread(ctx->fs->io, map, buf, offset, length, out_read);
            ext4_extent_map_release(map);
            return rc;
        }
        if (map) {
            ext4_extent_map_release(map);
        }
        if (rc == -EISDIR) {
            return rc;
        }
    }

    {
        uint64_t t_vfs = ztrace_is_enabled() ? ztrace_now_qpc() : 0;
        ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
            &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
        retval = ext2fs_file_open(ctx->fs, ino, 0, &file);
        if (retval == 0) {
            struct ext2_inode *inode = ext2fs_file_get_inode(file);
            if (LINUX_S_ISDIR(inode->i_mode)) {
                retval = EXT2_ET_IS_A_DIR;
            } else {
                retval = ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, NULL);
                if (retval == 0) {
                    retval = ext2fs_file_read(file, buf, length, &got);
                }
            }
            ext2fs_file_close(file);
        }
        ztrace_srw_release_shared(&ctx->rwlock, &lg);
        if (t_vfs) {
            uint64_t end = ztrace_now_qpc();
            int rc_vfs = (retval != 0) ? ext4_errcode_to_errno(retval) : 0;
            ztrace_record_complete("vfs", "ext2fs_file_read", t_vfs, end - t_vfs,
                                   (uint64_t)ino, (uint64_t)got, (int64_t)rc_vfs, ZTRACE_ARGS_RW_OP);
        }
    }

    if (retval != 0) return ext4_errcode_to_errno(retval);
    *out_read = (uint32_t)got;
    return 0;
}

int ext4_vfs_read(ext4_vfs_ctx_t *ctx, ext2_ino_t ino, void *buf, uint64_t offset,
                  uint32_t length, uint32_t *out_read) {
    if (ctx && ctx->overlay) {
        return overlay_vfs_read(ctx, ino, buf, offset, length, out_read);
    }
    return ext4_vfs_lower_read(ctx, ino, buf, offset, length, out_read);
}

int ext4_vfs_read_handle(ext4_vfs_ctx_t *ctx, ext4_file_handle_t *handle, void *buf,
                         uint64_t offset, uint32_t length, uint32_t *out_read) {
    if (!ctx || !handle || !out_read) return -EINVAL;
    *out_read = 0;
    if (length == 0) return 0;
    if (!buf) return -EINVAL;

    if (!ctx->overlay && ctx->enable_direct_io && ctx->lower_read_only &&
        ctx->extent_cache && !handle->is_dir) {
        ext4_extent_map_t *map = (ext4_extent_map_t *)InterlockedCompareExchangePointer(
            (PVOID volatile *)&handle->cached_extents, NULL, NULL);
        int rc = 0;
        if (!map) {
            map = ext4_extent_cache_get(ctx->extent_cache, handle->ino);
            if (!map) {
                ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
                    &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
                rc = ext4_extent_extract(ctx->fs, handle->ino, &map);
                ztrace_srw_release_shared(&ctx->rwlock, &lg);
                if (rc == 0 && map) {
                    ext4_extent_cache_put(ctx->extent_cache, map);
                }
            }
            if (rc == 0 && map) {
                ext4_extent_map_t *prev = (ext4_extent_map_t *)InterlockedCompareExchangePointer(
                    (PVOID volatile *)&handle->cached_extents, map, NULL);
                if (prev != NULL) {
                    ext4_extent_map_release(map);
                    map = prev;
                }
            } else if (map) {
                ext4_extent_map_release(map);
                map = NULL;
            }
        }
        if (rc == 0 && map) {
            return ext4_extent_pread(ctx->fs->io, map, buf, offset, length, out_read);
        }
        if (rc == -EISDIR) {
            return rc;
        }
    }

    return ext4_vfs_read(ctx, handle->ino, buf, offset, length, out_read);
}

int ext4_vfs_get_extents(ext4_vfs_ctx_t *ctx, const char *path, ext4_extent_map_t **out_map) {
    char norm[1024];
    ext2_ino_t ino = 0;
    int rc;

    if (!ctx || !path || !out_map) return -EINVAL;
    *out_map = NULL;

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
            &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
        rc = ext4_vfs_lookup_locked(ctx, norm, &ino);
        if (rc == 0) {
            rc = ext4_extent_extract(ctx->fs, ino, out_map);
        }
        ztrace_srw_release_shared(&ctx->rwlock, &lg);
    }
    return rc;
}
