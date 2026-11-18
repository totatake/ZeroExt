#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../extent/extent_map.h"
#include "../overlay/overlay_vfs.h"
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

    if (!ctx->disable_direct_io && ctx->extent_cache) {
        ext4_extent_map_t *map = ext4_extent_cache_get(ctx->extent_cache, ino);
        int rc = 0;
        if (!map) {
            AcquireSRWLockShared(&ctx->rwlock);
            rc = ext4_extent_extract(ctx->fs, ino, &map);
            ReleaseSRWLockShared(&ctx->rwlock);
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

    AcquireSRWLockShared(&ctx->rwlock);
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
    ReleaseSRWLockShared(&ctx->rwlock);

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

int ext4_vfs_get_extents(ext4_vfs_ctx_t *ctx, const char *path, ext4_extent_map_t **out_map) {
    char norm[1024];
    ext2_ino_t ino = 0;
    int rc;

    if (!ctx || !path || !out_map) return -EINVAL;
    *out_map = NULL;

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;

    AcquireSRWLockShared(&ctx->rwlock);
    rc = ext4_vfs_lookup_locked(ctx, norm, &ino);
    if (rc == 0) {
        rc = ext4_extent_extract(ctx->fs, ino, out_map);
    }
    ReleaseSRWLockShared(&ctx->rwlock);
    return rc;
}
