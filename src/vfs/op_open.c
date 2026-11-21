#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../extent/extent_map.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int ext4_vfs_open_stat(ext4_vfs_ctx_t *ctx, const char *path, int writable,
                       ext4_file_handle_t **out_handle, ext4_stat_t *out_st) {
    char norm[1024];
    char actual_leaf[EXT2_NAME_LEN + 1];
    ext2_ino_t ino = 0;
    struct ext2_inode_large ino_st;
    ext4_file_handle_t *h;
    int rc;
    errcode_t retval;

    if (!ctx || !path || !out_handle) return -EINVAL;
    if (ctx->overlay) {
        rc = overlay_vfs_open(ctx, path, writable, out_handle);
        if (rc == 0 && out_st && *out_handle) {
            const char *leaf = strrchr((*out_handle)->path, '/');
            leaf = leaf ? (leaf + 1) : (*out_handle)->path;
            rc = overlay_vfs_stat_ino(ctx, (*out_handle)->ino, leaf, out_st);
            if (rc != 0) {
                ext4_vfs_close_handle(*out_handle);
                *out_handle = NULL;
            }
        }
        return rc;
    }
    *out_handle = NULL;
    memset(&ino_st, 0, sizeof(ino_st));
    actual_leaf[0] = '\0';

    if (writable && ctx->read_only) {
        return -EROFS;
    }

    rc = ext4_vfs_normalize_path(path, norm, sizeof(norm));
    if (rc != 0) return rc;

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
            &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
        rc = ext4_vfs_lookup_locked_ex(ctx, norm, &ino, actual_leaf, sizeof(actual_leaf));
        if (rc == 0) {
            if (out_st) {
                retval = ext2fs_read_inode_full(ctx->fs, ino, (struct ext2_inode *)&ino_st, sizeof(ino_st));
            } else {
                retval = ext2fs_read_inode(ctx->fs, ino, (struct ext2_inode *)&ino_st);
            }
            if (retval) {
                rc = ext4_errcode_to_errno(retval);
            } else if (out_st) {
                ext4_fill_stat_from_inode(ctx, ino, &ino_st, actual_leaf, out_st);
            }
        }
        ztrace_srw_release_shared(&ctx->rwlock, &lg);
    }

    if (rc != 0) return rc;

    h = (ext4_file_handle_t *)calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    h->ino = ino;
    h->is_dir = LINUX_S_ISDIR(ino_st.i_mode);
    h->open_writable = writable;
    strcpy_s(h->path, sizeof(h->path), norm);
    *out_handle = h;
    return 0;
}

int ext4_vfs_open(ext4_vfs_ctx_t *ctx, const char *path, int writable, ext4_file_handle_t **out_handle) {
    return ext4_vfs_open_stat(ctx, path, writable, out_handle, NULL);
}

void ext4_vfs_close_handle(ext4_file_handle_t *handle) {
    if (!handle) return;
    ext4_extent_map_release(handle->cached_extents);
    free(handle);
}
