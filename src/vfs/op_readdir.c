#include "ext4_vfs.h"
#include "../common/ext4_errno.h"
#include <errno.h>
#include <string.h>

struct vfs_readdir_bridge {
    ext4_vfs_ctx_t *ctx;
    ext4_readdir_cb cb;
    void           *user_ctx;
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

int ext4_vfs_readdir(ext4_vfs_ctx_t *ctx, ext2_ino_t dir_ino, ext4_readdir_cb cb, void *user_ctx) {
    struct vfs_readdir_bridge bridge;
    int ret;

    if (!ctx || !cb || dir_ino == 0) return -EINVAL;

    memset(&bridge, 0, sizeof(bridge));
    bridge.ctx = ctx;
    bridge.cb = cb;
    bridge.user_ctx = user_ctx;

    AcquireSRWLockShared(&ctx->rwlock);
    ret = ext2fs_dir_iterate2(ctx->fs, dir_ino, 0, NULL, vfs_readdir_cb_internal, &bridge);
    ReleaseSRWLockShared(&ctx->rwlock);

    if (ret != 0) return ext4_errcode_to_errno((errcode_t)ret);
    return bridge.err;
}
