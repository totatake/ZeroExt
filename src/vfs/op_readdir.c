#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include "../overlay/overlay_vfs.h"
#include "../trace/perfetto_trace.h"
#include <errno.h>
#include <string.h>

struct vfs_readdir_bridge {
    ext4_vfs_ctx_t *ctx;
    ext4_readdir_cb cb;
    void           *user_ctx;
    const char     *marker_utf8;
    size_t          marker_len;
    int             past_marker;
    int             stop;
    int             err;
};

static int vfs_readdir_cb_internal(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                                   int offset, int blocksize, char *buf, void *priv_data) {
    struct vfs_readdir_bridge *b = (struct vfs_readdir_bridge *)priv_data;
    struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)dirent;
    char name_buf[EXT2_NAME_LEN + 1];
    struct ext2_inode_large ino_st;
    ext4_stat_t st;
    errcode_t retval;
    (void)dir;
    (void)entry;
    (void)offset;
    (void)blocksize;
    (void)buf;

    if (de->inode == 0) return 0;
    if (de->name_len > EXT2_NAME_LEN) return 0;

    if (b->marker_utf8 && !b->past_marker) {
        if (de->name_len == b->marker_len &&
            memcmp(de->name, b->marker_utf8, de->name_len) == 0) {
            b->past_marker = 1;
        }
        return 0;
    }

    memcpy(name_buf, de->name, de->name_len);
    name_buf[de->name_len] = '\0';

    memset(&ino_st, 0, sizeof(ino_st));
    retval = ext2fs_read_inode_full(b->ctx->fs, de->inode, (struct ext2_inode *)&ino_st, sizeof(ino_st));
    if (retval != 0) {
        return 0;
    }
    ext4_fill_stat_from_inode(b->ctx, de->inode, &ino_st, name_buf, &st);

    if (b->cb(name_buf, &st, b->user_ctx) != 0) {
        b->stop = 1;
        return DIRENT_ABORT;
    }
    return 0;
}

int ext4_vfs_lower_readdir_from(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, const char *marker_utf8,
                                ext4_readdir_cb cb, void *user_ctx) {
    struct vfs_readdir_bridge bridge;
    int ret;

    if (!ctx || !cb || dir_ino == 0) return -EINVAL;

    memset(&bridge, 0, sizeof(bridge));
    bridge.ctx = ctx;
    bridge.cb = cb;
    bridge.user_ctx = user_ctx;
    bridge.marker_utf8 = (marker_utf8 && marker_utf8[0] != '\0') ? marker_utf8 : NULL;
    bridge.marker_len = bridge.marker_utf8 ? strlen(bridge.marker_utf8) : 0;
    bridge.past_marker = (bridge.marker_utf8 == NULL);

    {
        ztrace_lock_guard_t lg = ztrace_srw_acquire_shared(
            &ctx->rwlock, "ctx->rwlock wait (S)", "ctx->rwlock hold (S)");
        ret = ext2fs_dir_iterate2(ctx->fs, dir_ino, 0, NULL, vfs_readdir_cb_internal, &bridge);
        ztrace_srw_release_shared(&ctx->rwlock, &lg);
    }

    if (ret != 0) return ext4_errcode_to_errno((errcode_t)ret);
    return bridge.err;
}

int ext4_vfs_lower_readdir(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, ext4_readdir_cb cb, void *user_ctx) {
    return ext4_vfs_lower_readdir_from(ctx, dir_ino, NULL, cb, user_ctx);
}

int ext4_vfs_readdir_from(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, const char *marker_utf8,
                          ext4_readdir_cb cb, void *user_ctx) {
    if (ctx && ctx->overlay) {
        return overlay_vfs_readdir(ctx, dir_ino, cb, user_ctx);
    }
    return ext4_vfs_lower_readdir_from(ctx, dir_ino, marker_utf8, cb, user_ctx);
}

int ext4_vfs_readdir(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, ext4_readdir_cb cb, void *user_ctx) {
    return ext4_vfs_readdir_from(ctx, dir_ino, NULL, cb, user_ctx);
}
