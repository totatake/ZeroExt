#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int ext4_vfs_symlink(ext4_vfs_ctx_t *ctx, const char *link_path, const char *target_path) {
    char norm[1024];
    char parent_path[1024];
    char name[EXT2_NAME_LEN + 1];
    ext2_ino_t parent_ino = 0, existing = 0;
    errcode_t retval;
    int rc;

    if (!ctx || !link_path || !target_path) return -EINVAL;
    if (ctx->read_only || ctx->lower_read_only) return -EROFS;

    rc = ext4_vfs_normalize_path(link_path, norm, sizeof(norm));
    if (rc != 0) return rc;
    rc = ext4_vfs_split_parent(norm, parent_path, sizeof(parent_path), name, sizeof(name));
    if (rc != 0) return rc;

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_exclusive(
            &ctx->rwlock, "ctx->rwlock wait (X)", "ctx->rwlock hold (X)");
        rc = ext4_vfs_lookup_locked(ctx, parent_path, &parent_ino);
        if (rc == 0) {
            char full[1024];
            snprintf(full, sizeof(full), "%s%s%s",
                     parent_path, strcmp(parent_path, "/") == 0 ? "" : "/", name);
            if (ext4_vfs_lookup_locked(ctx, full, &existing) == 0) {
                rc = -EEXIST;
            } else {
                retval = ext2fs_symlink(ctx->fs, parent_ino, 0, name, target_path);
                if (retval == 0) {
                    ext4_dentry_cache_invalidate(ctx);
                }
                rc = ext4_errcode_to_errno(retval);
            }
        }
        ztrace_srw_release_exclusive(&ctx->rwlock, &lg);
    }
    return rc;
}

int ext4_vfs_readlink(ext4_vfs_ctx_t *ctx, const char *link_path, char *out_target, size_t out_sz) {
    char norm[1024];
    ext2_ino_t ino = 0;
    struct ext2_inode ino_st;
    errcode_t retval;
    int rc;

    if (!ctx || !link_path || !out_target || out_sz == 0) return -EINVAL;
    rc = ext4_vfs_normalize_path(link_path, norm, sizeof(norm));
    if (rc != 0) return rc;

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
            &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
        rc = ext4_vfs_lookup_locked(ctx, norm, &ino);
        if (rc == 0) {
            retval = ext2fs_read_inode(ctx->fs, ino, &ino_st);
            if (retval) {
                rc = ext4_errcode_to_errno(retval);
            } else if (!LINUX_S_ISLNK(ino_st.i_mode)) {
                rc = -EINVAL;
            } else {
                uint64_t sz = EXT2_I_SIZE(&ino_st);
                if (sz >= out_sz) {
                    rc = -ENAMETOOLONG;
                } else if (ext2fs_is_fast_symlink(&ino_st)) {
                    memcpy(out_target, ino_st.i_block, (size_t)sz);
                    out_target[sz] = '\0';
                    rc = 0;
                } else {
                    ext2_file_t f = NULL;
                    unsigned int got = 0;
                    retval = ext2fs_file_open2(ctx->fs, ino, &ino_st, 0, &f);
                    if (retval == 0) {
                        retval = ext2fs_file_read(f, out_target, (unsigned int)sz, &got);
                        ext2fs_file_close(f);
                    }
                    if (retval == 0) {
                        out_target[got] = '\0';
                        rc = 0;
                    } else {
                        rc = ext4_errcode_to_errno(retval);
                    }
                }
            }
        }
        ztrace_srw_release_shared(&ctx->rwlock, &lg);
    }
    return rc;
}
